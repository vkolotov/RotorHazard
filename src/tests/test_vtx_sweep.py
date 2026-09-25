"""Regression tests for the VTX-commanded equalisation sweep.

The sweep's job is to refuse to capture anything it cannot confirm, so most of
these check that a channel which did not change is skipped rather than measured.
"""
import gevent.event
import gevent.lock
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
import vtx_control


class SweepTest(unittest.TestCase):
    """Fixtures: a fleet of nodes on R1..Rn with settable peaks."""

    def context(self, count=3, bands=None):
        nodes = []
        for _ in range(count):
            node = Node()
            node.api_level = 37
            node.init()
            nodes.append(node)
        bands = bands or ['R'] * count
        profile = SimpleNamespace(
            id=1, frequencies=json.dumps({'b': bands,
                                          'c': list(range(1, count + 1)),
                                          'f': [5658 + 37 * i for i in range(count)]}))
        ctx = SimpleNamespace(
            race=SimpleNamespace(profile=profile, num_nodes=count),
            interface=Mock(nodes=nodes), rhui=Mock(), rhdata=Mock(), events=Mock())
        ctx.rhdata.get_optionInt.return_value = 7
        cal = Calibration(ctx)
        ctx.calibration = cal
        return ctx, nodes, cal

    def controller(self, ctx, sent=None):
        """A stand-in VRx controller that records what it was asked to send."""
        sent = sent if sent is not None else []
        backpack = SimpleNamespace(
            send_set_vtx_config=lambda band, channel: sent.append(f'{band}{channel}'),
            get_pilot_uid=lambda pilot_id: b'\x01\x02\x03\x04\x05\x06',
            set_send_uid=lambda uid: None,
            reset_send_uid=lambda: None)
        ctx.vrx_manager = SimpleNamespace(controllers={'elrs': backpack})
        return sent

    #
    # Channel encoding
    #

    def test_channel_index_matches_the_elrs_band_order(self):
        """A is band 1, so R1 is 32 and L1 is 40."""
        self.assertEqual(vtx_control.channel_index('A1'), 0)
        self.assertEqual(vtx_control.channel_index('R1'), 32)
        self.assertEqual(vtx_control.channel_index('R4'), 35)
        self.assertEqual(vtx_control.channel_index('R8'), 39)
        self.assertEqual(vtx_control.channel_index('L1'), 40)

    def test_channel_index_rejects_what_it_cannot_command(self):
        for label in ('R0', 'R9', 'X1'):
            with self.assertRaises(vtx_control.VtxChannelError):
                vtx_control.channel_index(label)

    #
    # Which channels get swept
    #

    def test_sweep_visits_only_the_bands_it_will_command(self):
        ctx, _, cal = self.context(count=3, bands=['R', 'F', 'L'])
        self.assertEqual(cal.eq_sweep_channels(), ['R1', 'L3'])

    def test_sweep_skips_duplicate_channels(self):
        ctx, _, cal = self.context(count=2)
        ctx.race.profile.frequencies = json.dumps(
            {'b': ['R', 'R'], 'c': [4, 4], 'f': [5769, 5769]})
        self.assertEqual(cal.eq_sweep_channels(), ['R4'])

    #
    # Confirmation
    #

    def test_confirmation_compares_excess_not_raw_rssi(self):
        """A high-floor idle node must not outrank a lit low-floor one.

        Node 1 idles at 116 while node 2 idles at 70. On raw values node 1 wins
        outright; on excess over its own floor node 2 is the one carrying signal.
        """
        ctx, nodes, cal = self.context(count=2)
        nodes[0].node_peak_rssi = 118
        nodes[1].node_peak_rssi = 160
        vtx = vtx_control.VtxController(ctx)
        with patch('vtx_control.gevent.sleep'):
            label, margin, _ = vtx.observed_channel([116, 70], ['R1', 'R2'])
        self.assertEqual(label, 'R2')
        self.assertEqual(margin, 90)

    def test_confirmation_refuses_an_ambiguous_read(self):
        """Adjacent-channel bleed must not be mistaken for the commanded channel."""
        ctx, nodes, cal = self.context(count=2)
        nodes[0].node_peak_rssi = 150  # 60 over its floor
        nodes[1].node_peak_rssi = 145  # 55 over its floor: no clear winner
        vtx = vtx_control.VtxController(ctx)
        with patch('vtx_control.gevent.sleep'):
            label, _, separation = vtx.observed_channel([90, 90], ['R1', 'R2'])
        self.assertIsNone(label)
        self.assertLess(separation, 255 * vtx_control.VTX_CONFIRM_SEPARATION_FRACTION)

    def test_confirmation_refuses_a_silent_vtx(self):
        """Nothing above the floor means the VTX is off, in pit mode, or away."""
        ctx, nodes, cal = self.context(count=2)
        nodes[0].node_peak_rssi = 92
        nodes[1].node_peak_rssi = 91
        vtx = vtx_control.VtxController(ctx)
        with patch('vtx_control.gevent.sleep'):
            label, _, _ = vtx.observed_channel([90, 90], ['R1', 'R2'])
        self.assertIsNone(label)

    def test_confirmation_needs_consecutive_agreement(self):
        """One read can land mid-transition, so a single match is not enough."""
        ctx, nodes, cal = self.context(count=2)
        vtx = vtx_control.VtxController(ctx)
        reads = iter([('R2', 90, 80), ('R1', 90, 80), ('R2', 90, 80),
                      ('R2', 90, 80)])
        with patch.object(vtx, 'observed_channel', side_effect=lambda *a: next(reads)), \
                patch('vtx_control.gevent.sleep'):
            confirmed, _ = vtx.confirm_channel('R2', [90, 90], ['R1', 'R2'])
        self.assertTrue(confirmed)

    #
    # The sweep itself
    #

    def test_sweep_captures_every_confirmed_channel(self):
        ctx, nodes, cal = self.context(count=2)
        sent = self.controller(ctx)
        cal._eq_mode = 'auto'
        cal._eq_captured = {'noise': [90, 90]}
        cal._eq_note_capture_session()

        vtx = cal._vtx()
        with patch.object(vtx, 'confirm_channel', return_value=(True, 'margin 90')), \
                patch('calibration.gevent.sleep'):
            nodes[0].node_peak_rssi = 180
            nodes[1].node_peak_rssi = 175
            self.assertTrue(cal.eq_sweep_level('high'))

        self.assertEqual(sent, ['R1', 'R2'])
        self.assertIn('high:R1', cal._eq_captured)
        self.assertIn('high:R2', cal._eq_captured)

    def test_sweep_skips_a_channel_it_cannot_confirm(self):
        """The whole point: an unconfirmed channel is not captured.

        A reading taken while the VTX sat elsewhere would fit a plausible
        correction to the wrong channel, which is worse than none at all.
        """
        ctx, nodes, cal = self.context(count=2)
        self.controller(ctx)
        cal._eq_mode = 'auto'
        cal._eq_captured = {'noise': [90, 90]}
        cal._eq_note_capture_session()

        answers = iter([(True, 'margin 90'), (False, 'reading R1, not R2')])
        vtx = cal._vtx()
        with patch.object(vtx, 'confirm_channel', side_effect=lambda *a: next(answers)), \
                patch('calibration.gevent.sleep'):
            nodes[0].node_peak_rssi = 180
            nodes[1].node_peak_rssi = 175
            self.assertFalse(cal.eq_sweep_level('high'))

        self.assertIn('high:R1', cal._eq_captured)
        self.assertNotIn('high:R2', cal._eq_captured)
        self.assertEqual(len(cal.eq_sweep_state()['skipped']), 1)

    def test_sweep_refuses_without_a_noise_floor(self):
        """Excess is measured against the floor, so the floor comes first."""
        ctx, _, cal = self.context(count=2)
        self.controller(ctx)
        cal._eq_mode = 'auto'
        with patch('calibration.gevent.sleep'):
            self.assertFalse(cal.eq_sweep_level('high'))

    def test_sweep_refuses_without_a_calibration_pilot(self):
        ctx, _, cal = self.context(count=2)
        self.controller(ctx)
        ctx.rhdata.get_optionInt.return_value = 0
        cal._eq_mode = 'auto'
        cal._eq_captured = {'noise': [90, 90]}
        cal._eq_note_capture_session()
        with patch('calibration.gevent.sleep'):
            self.assertFalse(cal.eq_sweep_level('high'))

    def test_sweep_reports_unavailable_without_a_controller(self):
        ctx, _, cal = self.context(count=2)
        ctx.vrx_manager = SimpleNamespace(controllers={})
        self.assertFalse(cal.eq_sweep_state()['available'])

    #
    # Choosing a method
    #

    def test_wizard_offers_the_choice_before_anything_is_captured(self):
        ctx, _, cal = self.context(count=2)
        self.controller(ctx)
        state = cal.eq_wizard_state()
        self.assertEqual(state['state'], 'choosing')
        self.assertIsNone(state['mode'])

    def test_choosing_manual_arms_the_manual_steps(self):
        ctx, _, cal = self.context(count=2)
        self.controller(ctx)
        self.assertTrue(cal.eq_wizard_set_mode('manual'))
        state = cal.eq_wizard_state()
        self.assertEqual(state['state'], 'capturing')
        self.assertEqual(state['level'], 'noise')
        self.assertEqual(state['mode'], 'manual')

    def test_auto_is_refused_without_a_controller_to_command_with(self):
        ctx, _, cal = self.context(count=2)
        ctx.vrx_manager = SimpleNamespace(controllers={})
        self.assertFalse(cal.eq_wizard_set_mode('auto'))
        self.assertIsNone(cal.eq_wizard_mode())

    def test_the_method_cannot_change_part_way_through_a_run(self):
        """Captures taken one way would be compared against ones taken the other."""
        ctx, _, cal = self.context(count=2)
        self.controller(ctx)
        cal.eq_wizard_set_mode('auto')
        cal._eq_captured = {'noise': [90, 90]}
        cal._eq_note_capture_session()
        self.assertFalse(cal.eq_wizard_set_mode('manual'))
        self.assertEqual(cal.eq_wizard_mode(), 'auto')

    def test_manual_capture_is_refused_in_automatic_mode(self):
        ctx, _, cal = self.context(count=2)
        self.controller(ctx)
        cal.eq_wizard_set_mode('auto')
        with patch('calibration.gevent.sleep'):
            self.assertFalse(cal.eq_wizard_capture())

    def test_sweeping_is_refused_in_manual_mode(self):
        ctx, _, cal = self.context(count=2)
        self.controller(ctx)
        cal.eq_wizard_set_mode('manual')
        cal._eq_captured = {'noise': [90, 90]}
        cal._eq_note_capture_session()
        with patch('calibration.gevent.sleep'):
            self.assertFalse(cal.eq_sweep_level('high'))

    def test_back_from_the_first_step_returns_to_the_choice(self):
        ctx, _, cal = self.context(count=2)
        self.controller(ctx)
        cal.eq_wizard_set_mode('manual')
        self.assertTrue(cal.eq_wizard_back())
        self.assertIsNone(cal.eq_wizard_mode())
        self.assertEqual(cal.eq_wizard_state()['state'], 'choosing')

    def test_sweep_stage_advances_with_what_has_been_captured(self):
        ctx, _, cal = self.context(count=2)
        self.controller(ctx)
        self.assertEqual(cal.eq_sweep_state()['stage'], 'noise')

        cal._eq_captured = {'noise': [90, 90]}
        cal._eq_note_capture_session()
        self.assertEqual(cal.eq_sweep_state()['stage'], 'high')

        cal._eq_captured['high:R1'] = [180, 90]
        cal._eq_captured['high:R2'] = [90, 175]
        self.assertEqual(cal.eq_sweep_state()['stage'], 'low')

        cal._eq_captured['low:R1'] = [140, 90]
        cal._eq_captured['low:R2'] = [90, 135]
        self.assertEqual(cal.eq_sweep_state()['stage'], 'ready')


if __name__ == '__main__':
    unittest.main()
