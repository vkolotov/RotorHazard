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

# How far clear of the runner-up the winning node has to be. Adjacent channels
#  bleed: a quad on R1 lifted the R6 node 35 counts over its floor while R1
#  itself rose 94. Separation rather than absolute level, because bleed scales
#  with the signal that causes it.
VTX_CONFIRM_SEPARATION_FRACTION = 0.10

# How long to keep looking for the commanded channel to appear. The handset
#  sends the VTX configuration three times at roughly one second intervals, so
#  a change can legitimately take a few seconds to take effect; polling returns
#  as soon as it has, rather than always waiting out the worst case.
VTX_CONFIRM_TIMEOUT_SECONDS = 10.0

# How long each read of the nodes watches for, and the gap between reads.
VTX_CONFIRM_READ_SECONDS = 0.5
VTX_CONFIRM_POLL_SECONDS = 0.3

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

        if callable(uid_getter) and callable(set_uid):
            set_uid(uid_getter(pilot_id))
        try:
            controller.send_set_vtx_config(band, channel)
            logger.info('Commanded VTX channel %s for pilot %s', label, pilot_id)
        finally:
            if callable(reset_uid):
                reset_uid()

    #
    # Confirming
    #

    def _read_excess(self, floors, seconds=VTX_CONFIRM_READ_SECONDS):
        """How far each node is reading above its own noise floor.

        Excess rather than raw RSSI, because the floors differ by more than a
        signal does: one measured fleet spanned 70 to 116 counts idle, so an
        idle high-floor node outranks a genuinely lit low-floor one on raw
        values alone.

        :param floors: Per-node noise floor, None where unknown
        :param seconds: How long to watch the nodes
        :return: Per-node excess, None where it cannot be computed
        """
        nodes = self._racecontext.interface.nodes
        num = self._racecontext.race.num_nodes
        self._racecontext.calibration.eq_reset_extremums()
        gevent.sleep(seconds)

        out = []
        for idx in range(num):
            node = nodes[idx] if idx < len(nodes) else None
            floor = floors[idx] if idx < len(floors) else None
            peak = getattr(node, 'node_peak_rssi', None) if node else None
            if not peak or floor is None or peak >= node.max_rssi_value:
                out.append(None)
                continue
            out.append(int(peak) - int(floor))
        return out

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

    def confirm_channel(self, label, floors, channels,
                        timeout=VTX_CONFIRM_TIMEOUT_SECONDS, cancelled=None):
        """Wait until the nodes agree the quad is on `label`.

        Polls rather than sleeping out a fixed settle: a change that has taken
        effect is confirmed as soon as it is visible, and one that has not is
        reported instead of being captured.

        :param label: The channel that was commanded
        :param floors: Per-node noise floor
        :param channels: Per-node channel label
        :param timeout: How long to keep looking
        :param cancelled: Called each pass; truthy gives up without waiting out
            the timeout, so cancelling a sweep does not cost a full timeout for
            every channel left in it
        :return: (True, detail) once confirmed, or (False, detail) otherwise
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
