"""Commanding the calibration quad's video transmitter.

The equalisation wizard needs the quad on a known channel at each step. Doing
that by hand means the operator walking back to the radio between every
capture; commanding it from here leaves them at the timer.

The command goes out as MSP over whatever VRx controller can carry it - in
practice the ExpressLRS backpack plugin, whose timer backpack forwards any MSP
function it does not recognise verbatim over ESP-NOW. That reaches the bound
handset's TX backpack, which hands it to the ELRS module, which sends the
channel OTA to the receiver and on to the VTX. No node firmware is involved.

Nothing in that chain reports back, and an attempt to confirm the change from
the timer's own receivers did not survive contact with real hardware: adjacent
channels bleed, receivers differ in noise floor by more than a channel change
moves them, and a read can land mid-transition. So this sends the command and
says so, and the operator - who can see the quad's OSD - decides whether it
took. That judgement is the one part of the chain that is actually reliable.

Addressing follows the backpack's own OSD test: nothing is set, so the packet
goes to the timer backpack's bound address. That is the handset bound to this
timer, which during calibration is the quad being calibrated.
"""

import logging
from contextlib import nullcontext

logger = logging.getLogger(__name__)

# Band order used by the ELRS VTX administrator: "Disabled;A;B;E;F;R;L", so A
#  is 1 and the index sent on the wire is (band - 1) * 8 + (channel - 1).
VTX_BANDS = 'ABEFRL'


class VtxChannelError(Exception):
    """A channel could not be commanded."""


def parse_channel(label):
    """Split a channel label such as "R8" into its band and channel number.

    :param label: Band letter followed by channel number
    :return: (band letter, channel number)
    """
    if not label or len(label) < 2:
        raise VtxChannelError('Not a channel this can command: {0}'.format(label))
    band, digits = label[0].upper(), label[1:]
    if band not in VTX_BANDS or not digits.isdigit():
        raise VtxChannelError('Not a channel this can command: {0}'.format(label))
    channel = int(digits)
    if not 1 <= channel <= 8:
        raise VtxChannelError('Not a channel this can command: {0}'.format(label))
    return band, channel


class VtxController:
    """Commands the bound quad's VTX channel."""

    def __init__(self, racecontext):
        self._racecontext = racecontext

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

    def command_channel(self, label):
        """Send one channel change to the bound quad's VTX.

        :param label: Channel label such as "R8"
        """
        band, channel = parse_channel(label)

        controller = self._backpack()
        if controller is None:
            raise VtxChannelError('No VRx controller can set a VTX channel')

        # Share the controller's send lock where it has one, so this does not
        #  interleave with an addressed OSD send already in flight.
        lock = getattr(controller, '_queue_lock', None)
        with lock if lock is not None else nullcontext():
            controller.send_set_vtx_config(band, channel)
        logger.info('Commanded VTX channel %s', label)
