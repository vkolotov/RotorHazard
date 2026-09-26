"""Regression tests for the wizard's VTX channel command.

The command is deliberately fire-and-forget: the operator watches the quad and
decides when to capture. So what these pin down is that the right channel goes
out, to the bound address, and that a chain which cannot carry it says so
rather than failing silently.
"""
import json
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

SRC = Path(__file__).resolve().parents[1]
for folder in ('interface', 'server'):
    sys.path.insert(0, str(SRC / folder))

from Node import Node
from calibration import Calibration
from vtx_control import VtxChannelError, VtxController, parse_channel


def context(count=2, controller=None):
    """A race context with `count` nodes on R1..Rn, and an optional backpack."""
    nodes = []
    for _ in range(count):
        node = Node()
        node.api_level = 37
        node.init()
        nodes.append(node)
    profile = SimpleNamespace(
        id=1, frequencies=json.dumps({'b': ['R'] * count,
                                      'c': list(range(1, count + 1))}))
    vrx = SimpleNamespace(controllers={'elrs': controller}) if controller else None
    ctx = SimpleNamespace(race=SimpleNamespace(profile=profile, num_nodes=count),
                          interface=Mock(nodes=nodes), rhui=Mock(), rhdata=Mock(),
                          events=Mock(), vrx_manager=vrx)
    ctx.rhdata.get_profile.return_value = profile
    return ctx, Calibration(ctx)


def backpack():
    """A controller offering the channel entry point, as the ELRS plugin does."""
    return Mock(spec=['send_set_vtx_config'])


class ChannelLabelTest(unittest.TestCase):
    def test_bands_and_channels_round_trip(self):
        self.assertEqual(parse_channel('R1'), ('R', 1))
        self.assertEqual(parse_channel('r8'), ('R', 8))
        self.assertEqual(parse_channel('L4'), ('L', 4))

    def test_a_label_outside_the_bands_is_refused(self):
        for label in ('R0', 'R9', 'Z1', 'R', '', None, 'RX', 'R1x'):
            with self.assertRaises(VtxChannelError):
                parse_channel(label)


class VtxControllerTest(unittest.TestCase):
    def test_the_command_carries_the_band_and_channel(self):
        ctrl = backpack()
        ctx, _ = context(controller=ctrl)
        VtxController(ctx).command_channel('R7')
        ctrl.send_set_vtx_config.assert_called_once_with('R', 7)

    def test_no_address_is_set_so_the_bound_quad_receives_it(self):
        """Addressing follows the backpack's own OSD test, which sets none.

        A UID set here would send to a pilot's bind phrase instead of to the
        handset actually bound to this timer, which is the quad on the bench.
        """
        ctrl = Mock(spec=['send_set_vtx_config', 'set_send_uid',
                          'get_pilot_uid', 'reset_send_uid'])
        ctx, _ = context(controller=ctrl)
        VtxController(ctx).command_channel('R1')
        ctrl.set_send_uid.assert_not_called()
        ctrl.get_pilot_uid.assert_not_called()

    def test_a_controller_without_the_entry_point_is_not_used(self):
        ctx, _ = context(controller=Mock(spec=['send_message']))
        vtx = VtxController(ctx)
        self.assertFalse(vtx.available())
        with self.assertRaises(VtxChannelError):
            vtx.command_channel('R1')

    def test_a_bad_channel_is_refused_before_anything_is_sent(self):
        ctrl = backpack()
        ctx, _ = context(controller=ctrl)
        with self.assertRaises(VtxChannelError):
            VtxController(ctx).command_channel('R9')
        ctrl.send_set_vtx_config.assert_not_called()

    def test_the_send_lock_is_held_across_the_command(self):
        """Shared with the plugin's addressed OSD sends, so it must be taken."""
        lock = Mock(__enter__=Mock(), __exit__=Mock(return_value=False))
        ctrl = Mock(spec=['send_set_vtx_config', '_queue_lock'])
        ctrl._queue_lock = lock
        ctrl.send_set_vtx_config.side_effect = \
            lambda *a: lock.__enter__.assert_called_once()
        ctx, _ = context(controller=ctrl)
        VtxController(ctx).command_channel('R1')
        lock.__exit__.assert_called_once()


class WizardSwitchTest(unittest.TestCase):
    def test_it_sends_the_channel_the_next_capture_needs(self):
        """The button follows the wizard rather than stepping a band.

        Step one is noise and has no channel; the low and high steps for R1
        both want the quad on R1, and only then does it move to R2.
        """
        ctrl = backpack()
        ctx, cal = context(count=2, controller=ctrl)
        cal._eq_captured = {}
        cal._eq_note_capture_session()

        self.assertFalse(cal.eq_vtx_switch())  # noise step: nothing to command
        ctrl.send_set_vtx_config.assert_not_called()

        for key, expected in (('noise', ('R', 1)),
                              ('low:R1', ('R', 1)),
                              ('high:R1', ('R', 2)),
                              ('low:R2', ('R', 2))):
            cal._eq_captured[key] = [1, 1]
            cal._eq_note_capture_session()
            self.assertTrue(cal.eq_vtx_switch())
            self.assertEqual(ctrl.send_set_vtx_config.call_args.args, expected)

    def test_nothing_is_sent_once_every_step_is_captured(self):
        ctrl = backpack()
        ctx, cal = context(count=1, controller=ctrl)
        cal._eq_captured = {'noise': [10], 'low:R1': [50], 'high:R1': [90]}
        cal._eq_note_capture_session()
        self.assertEqual(cal.eq_wizard_state()['state'], 'ready')
        self.assertFalse(cal.eq_vtx_switch())
        ctrl.send_set_vtx_config.assert_not_called()

    def test_a_failed_send_is_reported_and_not_raised(self):
        """The page stays usable: the operator can retune by hand and carry on."""
        ctrl = backpack()
        ctrl.send_set_vtx_config.side_effect = RuntimeError('backpack is offline')
        ctx, cal = context(count=1, controller=ctrl)
        cal._eq_captured = {'noise': [10]}
        cal._eq_note_capture_session()
        self.assertFalse(cal.eq_vtx_switch())
        self.assertTrue(ctx.rhui.emit_priority_message.called)

    def test_the_state_says_whether_a_channel_can_be_commanded(self):
        """The page hides the button without a backpack, so the flag must be right."""
        _, without = context(count=1)
        self.assertFalse(without.eq_wizard_state()['vtx'])

        ctx, with_bp = context(count=1, controller=backpack())
        state = with_bp.eq_wizard_state()
        self.assertTrue(state['vtx'])
        self.assertIsNone(state['channel'])  # noise step, so still no button

        with_bp._eq_captured = {'noise': [10]}
        with_bp._eq_note_capture_session()
        self.assertEqual(with_bp.eq_wizard_state()['channel'], 'R1')

    def test_capturing_still_works_with_no_backpack_at_all(self):
        """Manual equalisation must not start depending on a plugin being there."""
        ctx, cal = context(count=1)
        cal._eq_captured = {}
        cal._eq_note_capture_session()
        self.assertFalse(cal.eq_vtx_switch())
        with patch('calibration.gevent.sleep'):
            ctx.interface.nodes[0].node_nadir_rssi = 12
            self.assertTrue(cal.eq_wizard_capture())
        self.assertEqual(cal._eq_captured['noise'], [12])


if __name__ == '__main__':
    unittest.main()
