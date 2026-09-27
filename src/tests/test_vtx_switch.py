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
    R_FREQS = (5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917)
    profile = SimpleNamespace(
        id=1, norm_per_freq=None,
        frequencies=json.dumps({'b': ['R'] * count,
                                'c': list(range(1, count + 1)),
                                'f': list(R_FREQS[:count])}))
    vrx = SimpleNamespace(controllers={'elrs': controller}) if controller else None
    ctx = SimpleNamespace(race=SimpleNamespace(profile=profile, num_nodes=count),
                          interface=Mock(nodes=nodes), rhui=Mock(), rhdata=Mock(),
                          events=Mock(), vrx_manager=vrx)
    ctx.rhdata.get_profile.return_value = profile
    def save(data):
        for key, value in data.items():
            if key != 'profile_id':
                setattr(profile, key, json.dumps(value))
        return profile
    ctx.rhdata.alter_profile.side_effect = save
    ctx.interface.set_normalisation.return_value = True
    cal = Calibration(ctx)
    # A run is armed only once a scope is chosen.
    cal._norm_scope_sel = 'current'
    cal._norm_saved_freqs = cal._norm_profile_freqs()
    cal._norm_channels = cal._norm_sweep_channels()
    return ctx, cal


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
        cal._norm_captured = {}
        cal._norm_note_capture_session()

        # Every step carries a channel now, including the first of the noise
        #  pass, so there is always something to command.
        self.assertTrue(cal.norm_vtx_switch())
        self.assertEqual(ctrl.send_set_vtx_config.call_args.args, ('R', 1))

        for key, expected in (('noise:R1', ('R', 2)),
                              ('noise:R2', ('R', 1)),
                              ('high:R1', ('R', 2))):
            cal._norm_captured[key] = [1, 1]
            cal._norm_note_capture_session()
            self.assertTrue(cal.norm_vtx_switch())
            self.assertEqual(ctrl.send_set_vtx_config.call_args.args, expected)

    def test_nothing_is_sent_once_every_step_is_captured(self):
        ctrl = backpack()
        ctx, cal = context(count=1, controller=ctrl)
        cal._norm_captured = {'noise:R1': [10], 'high:R1': [90]}
        cal._norm_note_capture_session()
        self.assertEqual(cal.norm_wizard_state()['state'], 'ready')
        self.assertFalse(cal.norm_vtx_switch())
        ctrl.send_set_vtx_config.assert_not_called()

    def test_a_failed_send_is_reported_and_not_raised(self):
        """The page stays usable: the operator can retune by hand and carry on."""
        ctrl = backpack()
        ctrl.send_set_vtx_config.side_effect = RuntimeError('backpack is offline')
        ctx, cal = context(count=1, controller=ctrl)
        cal._norm_captured = {'noise:R1': [10]}
        cal._norm_note_capture_session()
        self.assertFalse(cal.norm_vtx_switch())
        self.assertTrue(ctx.rhui.emit_priority_message.called)

    def test_the_state_says_whether_a_channel_can_be_commanded(self):
        """The page hides the button without a backpack, so the flag must be right."""
        _, without = context(count=1)
        self.assertFalse(without.norm_wizard_state()['vtx'])

        ctx, with_bp = context(count=1, controller=backpack())
        state = with_bp.norm_wizard_state()
        self.assertTrue(state['vtx'])
        # the first step of the noise pass is already on a channel
        self.assertEqual(state['channel'], 'R1')

        with_bp._norm_captured = {'noise:R1': [10]}
        with_bp._norm_note_capture_session()
        self.assertEqual(with_bp.norm_wizard_state()['channel'], 'R1')

    def test_capturing_still_works_with_no_backpack_at_all(self):
        """Manual normalisation must not start depending on a plugin being there."""
        ctx, cal = context(count=1)
        cal._norm_captured = {}
        cal._norm_note_capture_session()
        self.assertFalse(cal.norm_vtx_switch())
        with patch('calibration.gevent.sleep'):
            ctx.interface.nodes[0].node_nadir_rssi = 12
            self.assertTrue(cal.norm_wizard_capture())
        self.assertEqual(cal._norm_captured['noise:R1'], [12])


if __name__ == '__main__':
    unittest.main()
