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
from contextlib import nullcontext
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

# Six counts on the legacy pipeline clears the measured 1-2 count jitter
# without excluding a weak far-position signal. Require a clear winner and
# consecutive observations as well; a single rise is not confirmation.
VTX_SWITCH_RISE_FRACTION = 6.0 / 255
VTX_SWITCH_SEPARATION_FRACTION = 3.0 / 255

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

# One initial send and at most three retries, all inside one bounded wait.
# ELRS performs its own three-send sequence once the command reaches the TX.
# Do not continually restart that sequence with rapid duplicate commands.
VTX_RESEND_SECONDS = 7.0
VTX_MAX_RETRIES = 3
VTX_CONFIRM_TIMEOUT_SECONDS = VTX_RESEND_SECONDS * (VTX_MAX_RETRIES + 1)
VTX_ADDRESS_SETTLE_SECONDS = 0.5

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

        # Keep the pilot address and command together. Other plugin tasks use
        # this same lock for addressed OSD sends and must not reset our address
        # while we yield for the backpack to settle.
        lock = getattr(controller, '_queue_lock', None)
        with lock if lock is not None else nullcontext():
            try:
                if callable(uid_getter) and callable(set_uid):
                    set_uid(uid_getter(pilot_id))
                    gevent.sleep(VTX_ADDRESS_SETTLE_SECONDS)
                controller.send_set_vtx_config(band, channel)
                logger.info('Commanded VTX channel %s for pilot %s', label, pilot_id)
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

    def read_levels(self, seconds=VTX_CONFIRM_READ_SECONDS):
        """What every node is reading right now, highest over a short window.

        Raw readings, not excess over a floor. Detecting a switch means
        comparing a node against itself a moment earlier, and its floor is the
        same in both readings, so it cancels out. Bringing the floor into it
        only makes the answer depend on a separate measurement that can be
        stale or wrong - a floor captured while the quad was transmitting made
        the occupied channel look quiet and a bleeding neighbour look loud.

        :param seconds: How long to watch
        :return: Per-node reading, None where there is none
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
        return best

    def find_switch(self, before, after, channels):
        """Which channel the signal moved to, comparing two readings.

        A switch shows as one node climbing and usually another falling. The
        climb is what identifies the destination: the node whose reading rose
        most, provided it rose enough to be a signal arriving rather than
        noise or drift.

        A clear largest rise is evidence of arrival; similar rises are ambiguous
        and must not be accepted. Nothing here
        needs a noise floor, a per-node threshold, or a comparison between
        different nodes' levels.

        :param before: Readings from before the command
        :param after: Readings from after it
        :param channels: Per-node channel label
        :return: (label, rise, runner_up_rise), label None if nothing moved
        """
        rises = []
        for idx in range(min(len(before), len(after))):
            if idx >= len(channels) or not channels[idx]:
                continue
            if before[idx] is None or after[idx] is None:
                continue
            rises.append((after[idx] - before[idx], idx))
        if not rises:
            return (None, None, None)

        rises.sort(reverse=True)
        rise, winner = rises[0]
        runner_up = rises[1][0] if len(rises) > 1 else 0

        need = max(1, int(round(
            self._racecontext.calibration.eq_scale(winner)
            * VTX_SWITCH_RISE_FRACTION)))
        separation = max(1, int(round(
            self._racecontext.calibration.eq_scale(winner)
            * VTX_SWITCH_SEPARATION_FRACTION)))
        if rise < need or rise - runner_up < max(separation, rise * 0.2):
            return (None, rise, runner_up)

        label = channels[winner] if winner < len(channels) else None
        return (label, rise, runner_up)

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

    def confirm_channel(self, label, channels, before,
                        timeout=VTX_CONFIRM_TIMEOUT_SECONDS, cancelled=None,
                        resend=None):
        """Wait until the readings show the signal has moved to `label`.

        Compares each node against its own reading from before the command.
        The node for the commanded channel climbs when the transmitter arrives;
        bleed can climb too, so only a clear largest rise is accepted.

        Nothing here uses a noise floor. A floor is needed to turn a reading
        into a signal level, which the captures want, but a switch is a change
        and a change cancels the floor out. Earlier versions passed one in and
        a stale floor - captured while the quad was transmitting - made an
        occupied channel look quiet and a bleeding neighbour look loud, so a
        channel that had plainly switched was retried until the sweep gave up.

        :param label: The channel that was commanded
        :param channels: Per-node channel label
        :param before: Readings from before the command, from `read_levels`
        :param timeout: How long to keep looking
        :param cancelled: Called each pass; truthy gives up immediately
        :param resend: Called to command the channel again while waiting
        :return: (True, detail) once confirmed, or (False, detail) otherwise
        """
        deadline = time.monotonic() + timeout
        next_resend = time.monotonic() + VTX_RESEND_SECONDS
        agreed = 0
        retries = 0
        last = 'nothing moved'

        while time.monotonic() < deadline:
            if cancelled is not None and cancelled():
                return (False, 'cancelled')

            after = self.read_levels()
            seen, rise, runner_up = self.find_switch(before, after, channels)

            if seen == label:
                agreed += 1
                if agreed >= VTX_CONFIRM_CONSECUTIVE:
                    detail = 'rose {0}, next {1}'.format(rise, runner_up)
                    logger.info('Confirmed VTX on %s: %s', label, detail)
                    return (True, detail)
            else:
                agreed = 0
                if seen:
                    last = '{0} rose most ({1}), not {2}'.format(
                        seen, rise, label)
                elif rise is not None:
                    last = 'largest rise {0}, too small to be a switch'.format(
                        rise)

            # Observe first: do not send another command just as the previous
            # one succeeds. Two agreeing reads take priority over a retry.
            if (not agreed and resend is not None and retries < VTX_MAX_RETRIES
                    and time.monotonic() >= next_resend):
                if cancelled is not None and cancelled():
                    return (False, 'cancelled')
                retries += 1
                logger.info('Re-sending VTX channel %s (%s/%s)',
                            label, retries, VTX_MAX_RETRIES)
                resend()
                next_resend = time.monotonic() + VTX_RESEND_SECONDS
            gevent.sleep(VTX_CONFIRM_POLL_SECONDS)

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
