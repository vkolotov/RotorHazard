"""Commanding a quad's video transmitter, and confirming it obeyed.

The equalisation wizard needs the calibration quad on a known channel at each
step. Doing that by hand means the operator retuning the VTX between every
capture; commanding it from here reduces the whole run to placing the quad.

The command goes out as MSP over whatever VRx controller can carry it - in
practice the ExpressLRS backpack plugin, whose timer backpack forwards any MSP
function it does not recognise verbatim over ESP-NOW. That reaches the pilot's
TX backpack, which hands it to the handset's ELRS module, which sends the
channel OTA to the receiver and on to the VTX. No node firmware is involved.

Nothing in that chain reports back. The TX backpack can be asked for its cached
VTX packet, but it replies to its own bound group rather than to whoever asked,
and the cache holds what was last commanded rather than what the VTX did. So
confirmation comes from the timer's own receivers instead, which is the better
witness anyway: it measures what is actually being radiated, and so catches a
VTX that stayed silent, sat in pit mode, or never had a control wire at all.
"""

import logging
import time

import gevent

logger = logging.getLogger(__name__)

# Band order used by the ELRS VTX administrator: "Disabled;A;B;E;F;R;L", so A is
#  1 and the index sent on the wire is (band - 1) * 8 + (channel - 1).
VTX_BANDS = 'ABEFRL'

# How far above its own noise floor a node has to read before its channel counts
#  as carrying the quad. Measured margins on a settled 25 mW quad at the gate
#  were 78 to 94 counts on a byte-wide pipeline, so this leaves better than half
#  the observed margin in hand. A fraction of the noise-to-full-scale range
#  rather than a count, so it follows the width of the pipeline.
VTX_CONFIRM_MARGIN_FRACTION = 0.15

# How far up the pipeline the loudest channel has to be before anything counts
#  as on air at all. Below this every channel is idle and there is nothing to
#  compare. Only has to clear the noise, and has to stay below the weakest
#  receiver on its own channel: the least sensitive node measured reads 30
#  counts up where the best reads a hundred, and a bar above that would call a
#  working transmitter silence.
VTX_ON_AIR_FRACTION = 0.07

# How close to the loudest channel another has to read to count as on air too.
#  Bleed onto a neighbour has been measured at two thirds of the occupied
#  channel, so the line sits above that: one transmitter marks one channel.
VTX_HIGH_OF_LOUDEST = 0.80

# How far clear of the runner-up the winning node has to be. Adjacent channels
#  bleed: a quad on R1 lifted the R6 node 35 counts over its floor while R1
#  itself rose 94. Separation rather than absolute level, because bleed scales
#  with the signal that causes it.
VTX_CONFIRM_SEPARATION_FRACTION = 0.10

# How long to keep looking for the commanded channel to appear. Measured on a
#  quad at the gate, a commanded channel lands three seconds after the command:
#  the handset waits a second before its first send, then repeats twice more at
#  half-second intervals, and the receiver has to pass the change to the VTX.
#
# Long enough to outlast the handset's ten second disconnect debounce and still
#  leave room for a resend afterwards, because a command that arrives during
#  that window is discarded rather than delayed. Polling returns as soon as the
#  change is visible, so this costs nothing when things are working.
VTX_CONFIRM_TIMEOUT_SECONDS = 30.0

# How long to let the backpack change the address it sends to before using it,
#  and to let a packet leave before the address is put back.
VTX_ADDRESS_SETTLE_SECONDS = 0.5

# How long to wait for a change before commanding the channel again.
#
# Past the handset's disconnect debounce, which is the constraint that matters.
#  When the quad's link drops - as it does if the flight controller restarts
#  after a configuration write - the handset discards the queued packets and
#  refuses to send VTX configuration for ten seconds
#  (VTX_DISCONNECT_DEBOUNCE_MS in the ELRS firmware). Anything commanded inside
#  that window updates the handset's stored configuration and never reaches the
#  quad, so resending faster than the debounce achieves nothing and only adds
#  flash writes.
#
# A change that is going to work is visible in about three seconds, so a resend
#  only ever happens once the first command has genuinely not arrived.
VTX_RESEND_SECONDS = 12.0

# How long each read of the nodes watches for, the gap between those reads, and
#  how often the live reading is sampled inside one.
VTX_CONFIRM_READ_SECONDS = 0.5
VTX_CONFIRM_POLL_SECONDS = 0.3
VTX_SAMPLE_SECONDS = 0.1

# How many consecutive good reads confirm a change. Two, because a single read
#  can land mid-transition: the node being left behind decays while the new one
#  rises, and for a moment both sit at similar levels.
VTX_CONFIRM_CONSECUTIVE = 2


class VtxChannelError(Exception):
    """A channel could not be commanded, or could not be confirmed."""


def channel_index(label):
    """The wire index for a channel label such as "R8".

    :param label: Band letter followed by channel number
    :return: The index an MSP_SET_VTX_CONFIG payload carries
    """
    band, channel = label[0].upper(), int(label[1:])
    if band not in VTX_BANDS or not 1 <= channel <= 8:
        raise VtxChannelError('Not a channel this can command: {0}'.format(label))
    return VTX_BANDS.index(band) * 8 + (channel - 1)


class VtxController:
    """Commands one pilot's VTX and confirms the result on the timer's nodes."""

    def __init__(self, racecontext):
        self._racecontext = racecontext

    #
    # Commanding
    #

    def _backpack(self):
        """The VRx controller able to set a VTX channel, or None.

        Found by capability rather than by name, so any controller offering the
        same entry point can drive this.
        """
        manager = getattr(self._racecontext, 'vrx_manager', None)
        controllers = getattr(manager, 'controllers', None) or {}
        if not isinstance(controllers, dict):
            controllers = {c: c for c in controllers}
        for controller in controllers.values():
            if callable(getattr(controller, 'send_set_vtx_config', None)):
                return controller
        return None

    def available(self):
        """True when a VTX channel can be commanded at all."""
        return self._backpack() is not None

    def command_channel(self, pilot_id, label):
        """Send one channel change to a pilot's VTX.

        :param pilot_id: The pilot whose bind phrase addresses the VTX
        :param label: Channel label such as "R8"
        """
        controller = self._backpack()
        if controller is None:
            raise VtxChannelError('No VRx controller can set a VTX channel')

        band, channel = label[0].upper(), int(label[1:])
        channel_index(label)  # validate before addressing anything

        uid_getter = getattr(controller, 'get_pilot_uid', None)
        set_uid = getattr(controller, 'set_send_uid', None)
        reset_uid = getattr(controller, 'reset_send_uid', None)

        # Addressing a pilot makes the backpack drop its peer, change its MAC
        #  and register the new one. That is not instant, and a packet sent
        #  before it completes leaves on the old address - so give it a moment,
        #  as the controller's own addressed sends do.
        if callable(uid_getter) and callable(set_uid):
            set_uid(uid_getter(pilot_id))
            gevent.sleep(VTX_ADDRESS_SETTLE_SECONDS)
        try:
            controller.send_set_vtx_config(band, channel)
            logger.info('Commanded VTX channel %s for pilot %s', label, pilot_id)
            # Hold the address until the packet has been written, for the same
            #  reason: resetting it underneath a queued send re-points it.
            gevent.sleep(VTX_ADDRESS_SETTLE_SECONDS)
        finally:
            if callable(reset_uid):
                reset_uid()

    #
    # Confirming
    #

    def read_excess(self, floors, seconds=VTX_CONFIRM_READ_SECONDS):
        """How far each node is reading above its own noise floor.

        Public so a caller can take a before-reading to compare against.
        """
        return self._read_excess(floors, seconds)

    def _read_excess(self, floors, seconds=VTX_CONFIRM_READ_SECONDS):
        """How far each node is reading above its own noise floor.

        Excess rather than raw RSSI, because the floors differ by more than a
        signal does: one measured fleet spanned 70 to 116 counts idle, so an
        idle high-floor node outranks a genuinely lit low-floor one on raw
        values alone.

        Samples the live reading over a short window and takes the highest each
        node showed. Deliberately not the node's own peak tracking: that would
        have to be cleared before every read, which is a write to every node on
        the bus several times a second, and it throws away the peaks and nadirs
        the rest of the system is displaying. It also reads badly here - a peak
        cleared half a second ago holds whatever arrived in that half second,
        which during a channel change is as likely to be the channel being left
        as the one being joined.

        :param floors: Per-node noise floor, None where unknown
        :param seconds: How long to watch the nodes
        :return: Per-node excess, None where it cannot be computed
        """
        nodes = self._racecontext.interface.nodes
        num = self._racecontext.race.num_nodes

        best = [None] * num
        deadline = time.monotonic() + seconds
        while True:
            for idx in range(num):
                node = nodes[idx] if idx < len(nodes) else None
                value = getattr(node, 'current_rssi', None) if node else None
                if value and value < node.max_rssi_value:
                    if best[idx] is None or value > best[idx]:
                        best[idx] = int(value)
            if time.monotonic() >= deadline:
                break
            gevent.sleep(VTX_SAMPLE_SECONDS)

        out = []
        for idx in range(num):
            floor = floors[idx] if idx < len(floors) else None
            if best[idx] is None or floor is None:
                out.append(None)
                continue
            out.append(best[idx] - int(floor))
        return out

    #
    # Channel state
    #

    def channel_state(self, floors, seconds=VTX_CONFIRM_READ_SECONDS):
        """What every channel is doing: its level, and whether it is on air.

        A channel is HIGH when a transmitter is on it and LOW when it is not.
        The two are far apart - a quad on a channel reads most of the way up
        the pipeline while an idle receiver sits at its own floor - so the line
        between them does not have to be placed precisely to be reliable.

        Bleed is what makes the level alone ambiguous: a neighbour of an
        occupied channel can read two thirds as high. So the line is drawn from
        the loudest channel rather than from a constant, and a channel counts
        as HIGH only if it is within reach of whatever is loudest. That keeps
        one transmitter from marking three channels HIGH.

        :param floors: Per-node noise floor
        :param seconds: How long to watch
        :return: List of (excess, is_high) per node, either may be None
        """
        excess = self._read_excess(floors, seconds)
        levels = [v for v in excess if v is not None]
        if not levels:
            return [(None, None)] * len(excess)

        loudest = max(levels)
        scale = self._racecontext.calibration.eq_scale(0)
        # Nothing is on air at all: everything is down near the floor.
        if loudest < scale * VTX_ON_AIR_FRACTION:
            return [(v, False if v is not None else None) for v in excess]

        line = loudest * VTX_HIGH_OF_LOUDEST
        return [(v, (v >= line) if v is not None else None) for v in excess]

    def channel_change(self, floors, before, channels,
                       seconds=VTX_CONFIRM_READ_SECONDS):
        """What moved since `before`: which channel went off, which came on.

        :param floors: Per-node noise floor
        :param before: A previous `channel_state` result
        :param channels: Per-node channel label
        :param seconds: How long to watch
        :return: (went_low, went_high, state) as channel labels, either may be
            None if nothing moved that way
        """
        state = self.channel_state(floors, seconds)
        went_low = went_high = None

        for idx, (level, high) in enumerate(state):
            was = before[idx][1] if idx < len(before) else None
            if high is None or was is None or high == was:
                continue
            label = channels[idx] if idx < len(channels) else None
            if high:
                went_high = label
            else:
                went_low = label

        return (went_low, went_high, state)

    def _thresholds(self, node_index):
        """Confirmation thresholds in counts, from the node's full scale."""
        scale = self._racecontext.calibration.eq_scale(node_index)
        return (max(1, int(round(scale * VTX_CONFIRM_MARGIN_FRACTION))),
                max(1, int(round(scale * VTX_CONFIRM_SEPARATION_FRACTION))))

    def observed_channel(self, floors, channels):
        """Which channel the nodes currently say the quad is on.

        :param floors: Per-node noise floor
        :param channels: Per-node channel label, None where not participating
        :return: (label, margin, separation), or (None, margin, separation)
        """
        excess = self._read_excess(floors)
        ranked = sorted(
            ((v, i) for i, v in enumerate(excess) if v is not None),
            reverse=True)
        if not ranked:
            return (None, None, None)

        margin, winner = ranked[0]
        runner_up = ranked[1][0] if len(ranked) > 1 else 0
        separation = margin - runner_up

        need_margin, need_separation = self._thresholds(winner)
        if margin < need_margin or separation < need_separation:
            return (None, margin, separation)

        label = channels[winner] if winner < len(channels) else None
        return (label, margin, separation)

    def confirm_channel(self, label, floors, channels, before=None,
                        timeout=VTX_CONFIRM_TIMEOUT_SECONDS, cancelled=None,
                        resend=None):
        """Wait until the commanded channel is the one on air.

        Watches the state of every channel rather than the level of one. A
        channel is HIGH when a transmitter is on it, LOW when it is not, and a
        change is the commanded channel going HIGH - usually with whatever was
        HIGH before going LOW, though not always: the quad may arrive from a
        channel no node is watching.

        This replaces comparing one node's rise against thresholds. Levels vary
        by receiver and by how close the quad is, and bleed lifts the
        neighbours, so no fixed count separates a change from a non-change. The
        state does: what matters is which channel is occupied, and that is a
        question the levels answer clearly once they are read together.

        :param label: The channel that was commanded
        :param floors: Per-node noise floor
        :param channels: Per-node channel label
        :param before: State from before the command, as `channel_state`
            returns it. Without it the target simply has to be HIGH.
        :param timeout: How long to keep looking
        :param cancelled: Called each pass; truthy gives up without waiting out
            the timeout
        :param resend: Called to command the channel again while waiting, so a
            command that was lost is replaced without restarting the wait
        :return: (True, detail) once confirmed, or (False, detail) otherwise
        """
        target = None
        for idx, chan in enumerate(channels):
            if chan == label:
                target = idx
                break
        if target is None:
            return (False, 'no node is tuned to {0}'.format(label))

        was_high = None
        if before is not None and target < len(before):
            was_high = before[target][1]

        deadline = time.monotonic() + timeout
        next_resend = time.monotonic() + VTX_RESEND_SECONDS
        agreed = 0
        last = 'no channel came on air'

        while time.monotonic() < deadline:
            if cancelled is not None and cancelled():
                return (False, 'cancelled')

            if resend is not None and time.monotonic() >= next_resend:
                logger.info('Re-sending VTX channel %s', label)
                resend()
                next_resend = time.monotonic() + VTX_RESEND_SECONDS

            state = self.channel_state(floors)
            level, high = state[target] if target < len(state) else (None, None)

            if high:
                agreed += 1
                if agreed >= VTX_CONFIRM_CONSECUTIVE:
                    others = [channels[i] for i, (_, h) in enumerate(state)
                              if h and i != target and i < len(channels)]
                    detail = '{0} is on air at {1}'.format(label, level)
                    if others:
                        detail += ', with {0}'.format(', '.join(str(o) for o in others))
                    logger.info('Confirmed VTX: %s', detail)
                    return (True, detail)
            else:
                agreed = 0
                on_air = [channels[i] for i, (_, h) in enumerate(state)
                          if h and i < len(channels)]
                if on_air:
                    last = 'on air: {0}, not {1}'.format(
                        ', '.join(str(o) for o in on_air), label)
                elif level is not None:
                    last = 'nothing on air; {0} reads {1}'.format(label, level)

            gevent.sleep(VTX_CONFIRM_POLL_SECONDS)

        # Never having been LOW is worth saying: the quad may already have been
        #  on this channel, in which case nothing was ever going to change.
        if was_high:
            last += ' ({0} was already on air before the command)'.format(label)
        logger.warning('Could not confirm VTX on %s: %s', label, last)
        return (False, last)

    def _confirm_by_ranking(self, label, floors, channels, timeout, cancelled):
        """Confirm by comparing nodes against each other.

        The fallback for when there is no before-reading to compare against, as
        when the quad is already on the channel being asked for. Weaker, because
        bleed from a close quad can put a neighbouring node within a few counts
        of the right one.
        """
        deadline = time.monotonic() + timeout
        agreed = 0
        last = 'no reading above the noise floor'

        while time.monotonic() < deadline:
            if cancelled is not None and cancelled():
                return (False, 'cancelled')
            seen, margin, separation = self.observed_channel(floors, channels)
            if seen == label:
                agreed += 1
                if agreed >= VTX_CONFIRM_CONSECUTIVE:
                    detail = 'margin {0}, clear of the next by {1}'.format(
                        margin, separation)
                    logger.info('Confirmed VTX on %s: %s', label, detail)
                    return (True, detail)
            else:
                agreed = 0
                if seen:
                    last = 'reading {0}, not {1}'.format(seen, label)
                elif margin is not None:
                    last = ('no channel stands out: best margin {0}, clear of '
                            'the next by {1}'.format(margin, separation))
            gevent.sleep(VTX_CONFIRM_POLL_SECONDS)

        logger.warning('Could not confirm VTX on %s: %s', label, last)
        return (False, last)
