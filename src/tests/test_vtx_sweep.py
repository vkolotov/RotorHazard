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
import calibration
from calibration import Calibration
import vtx_control


class SweepTest(unittest.TestCase):
    """Fixtures: a fleet of nodes on R1..Rn with settable peaks."""

    def setUp(self):
        # Drive polling/settling deterministically, including tests that patch
        # sleep themselves. No wall-clock waiting or RF hardware is involved.
        self.now = 0.0
        def clock():
            self.now += 0.01
            return self.now
        def advance(seconds):
            self.now += seconds
        clock_patch = patch('vtx_control.time.monotonic', side_effect=clock)
        sleep_patch = patch('vtx_control.gevent.sleep', side_effect=advance)
        clock_patch.start()
        sleep_patch.start()
        self.addCleanup(clock_patch.stop)
        self.addCleanup(sleep_patch.stop)

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

    def test_the_default_scope_is_what_the_nodes_are_tuned_to(self):
        ctx, _, cal = self.context(count=3, bands=['R', 'F', 'L'])
        self.assertEqual(cal.eq_sweep_channels(), ['R1', 'F2', 'L3'])

    def test_sweep_skips_duplicate_channels(self):
        ctx, _, cal = self.context(count=2)
        ctx.race.profile.frequencies = json.dumps(
            {'b': ['R', 'R'], 'c': [4, 4], 'f': [5769, 5769]})
        self.assertEqual(cal.eq_sweep_channels(), ['R4'])

    def test_a_band_scope_covers_every_channel_of_that_band(self):
        ctx, _, cal = self.context(count=2)
        cal.eq_sweep_set_scope('r')
        self.assertEqual(cal.eq_sweep_channels(),
                         ['R' + str(n) for n in range(1, 9)])
        cal._eq_scope = 'rl'
        self.assertEqual(len(cal.eq_sweep_channels()), 16)

    def test_the_scope_cannot_change_part_way_through_a_run(self):
        ctx, _, cal = self.context(count=2)
        cal.eq_sweep_set_scope('r')
        cal._eq_captured = {'noise': [90, 90]}
        cal._eq_note_capture_session()
        self.assertFalse(cal.eq_sweep_set_scope('rl'))
        self.assertEqual(cal.eq_sweep_scope(), 'r')

    def test_the_manual_steps_follow_the_scope_too(self):
        """Both ways cover the same channels, so both read the same scope."""
        ctx, _, cal = self.context(count=2)
        cal.eq_wizard_set_mode('manual')
        cal.eq_sweep_set_scope('r')
        # one noise step, then a low and a high for each of eight channels
        self.assertEqual(cal.eq_wizard_state()['total'], 17)

    #
    # Confirmation
    #

    def test_confirmation_compares_excess_not_raw_rssi(self):
        """A high-floor idle node must not outrank a lit low-floor one.

        Node 1 idles at 116 while node 2 idles at 70. On raw values node 1 wins
        outright; on excess over its own floor node 2 is the one carrying signal.
        """
        ctx, nodes, cal = self.context(count=2)
        nodes[0].current_rssi = 118
        nodes[1].current_rssi = 160
        vtx = vtx_control.VtxController(ctx)
        with patch('vtx_control.gevent.sleep'):
            label, margin, _ = vtx.observed_channel([116, 70], ['R1', 'R2'])
        self.assertEqual(label, 'R2')
        self.assertEqual(margin, 90)

    def test_reading_the_nodes_does_not_clear_their_extremes(self):
        """Confirmation must not reset peak/nadir tracking.

        Clearing the extremes is a write to every node on the bus, and the
        confirmation polls several times a second; doing it per read wipes the
        peaks and nadirs the rest of the system displays. It also reads badly,
        since a peak cleared moments ago holds whatever arrived since, which
        during a change is as likely to be the channel being left behind.
        """
        ctx, nodes, cal = self.context(count=2)
        nodes[0].current_rssi = 150
        nodes[1].current_rssi = 95
        vtx = vtx_control.VtxController(ctx)
        with patch('vtx_control.gevent.sleep'):
            vtx.observed_channel([90, 90], ['R1', 'R2'])
        ctx.interface.reset_node_extremums.assert_not_called()

    def test_confirmation_reads_the_live_value(self):
        ctx, nodes, cal = self.context(count=2)
        nodes[0].current_rssi = 96
        nodes[1].current_rssi = 175
        vtx = vtx_control.VtxController(ctx)
        with patch('vtx_control.gevent.sleep'):
            label, margin, _ = vtx.observed_channel([90, 90], ['R1', 'R2'])
        self.assertEqual(label, 'R2')
        self.assertEqual(margin, 85)

    def test_confirmation_refuses_an_ambiguous_read(self):
        """Adjacent-channel bleed must not be mistaken for the commanded channel."""
        ctx, nodes, cal = self.context(count=2)
        nodes[0].current_rssi = 150  # 60 over its floor
        nodes[1].current_rssi = 145  # 55 over its floor: no clear winner
        vtx = vtx_control.VtxController(ctx)
        with patch('vtx_control.gevent.sleep'):
            label, _, separation = vtx.observed_channel([90, 90], ['R1', 'R2'])
        self.assertIsNone(label)
        self.assertLess(separation, 255 * vtx_control.VTX_CONFIRM_SEPARATION_FRACTION)

    def test_confirmation_refuses_a_silent_vtx(self):
        """Nothing above the floor means the VTX is off, in pit mode, or away."""
        ctx, nodes, cal = self.context(count=2)
        nodes[0].current_rssi = 92
        nodes[1].current_rssi = 91
        vtx = vtx_control.VtxController(ctx)
        with patch('vtx_control.gevent.sleep'):
            label, _, _ = vtx.observed_channel([90, 90], ['R1', 'R2'])
        self.assertIsNone(label)

    LABELS = ['R{0}'.format(n + 1) for n in range(8)]

    def _levels(self, ctx, nodes, values):
        for idx, node in enumerate(nodes):
            node.current_rssi = values[idx]
        vtx = vtx_control.VtxController(ctx)
        with patch('vtx_control.gevent.sleep'):
            return vtx, vtx.read_levels()

    def test_the_largest_rise_names_the_new_channel(self):
        """Measured across a real change: R8 fell 96, R4 rose 88."""
        ctx, nodes, cal = self.context(count=8, bands=['R'] * 8)
        before = [87, 95, 69, 113, 99, 118, 145, 188]
        after = [93, 111, 115, 202, 137, 125, 97, 90]
        vtx = vtx_control.VtxController(ctx)
        label, rise, _ = vtx.find_switch(before, after, self.LABELS)
        self.assertEqual(label, 'R4')
        self.assertEqual(rise, 89)

    def test_bleed_rises_less_than_the_channel_switched_to(self):
        """R7 climbs when the quad lands on R6, but never as much."""
        ctx, nodes, cal = self.context(count=8, bands=['R'] * 8)
        before = [90, 95, 70, 116, 95, 109, 88, 89]
        after = [90, 95, 75, 125, 141, 208, 152, 89]
        vtx = vtx_control.VtxController(ctx)
        label, _, _ = vtx.find_switch(before, after, self.LABELS)
        self.assertEqual(label, 'R6')

    def test_nothing_moving_is_not_a_switch(self):
        ctx, nodes, cal = self.context(count=8, bands=['R'] * 8)
        before = [90, 95, 70, 116, 95, 109, 88, 189]
        after = [91, 94, 70, 117, 96, 108, 89, 188]
        vtx = vtx_control.VtxController(ctx)
        label, _, _ = vtx.find_switch(before, after, self.LABELS)
        self.assertIsNone(label)

    def test_an_insensitive_node_still_registers_a_switch(self):
        """Node 4 gains least in the fleet and must still be detected."""
        ctx, nodes, cal = self.context(count=8, bands=['R'] * 8)
        before = [90, 95, 70, 116, 95, 109, 88, 189]
        after = [90, 95, 70, 146, 95, 109, 88, 95]
        vtx = vtx_control.VtxController(ctx)
        label, rise, _ = vtx.find_switch(before, after, self.LABELS)
        self.assertEqual(label, 'R4')
        self.assertEqual(rise, 30)

    def test_detection_needs_no_noise_floor(self):
        """A floor captured while transmitting used to break detection.

        The node for the occupied channel had the signal in its floor, so it
        read as quiet, and a bleeding neighbour with an honest floor read as
        loud. Comparing a node against itself cannot go wrong that way.
        """
        ctx, nodes, cal = self.context(count=8, bands=['R'] * 8)
        before = [113, 141, 175, 170, 111, 116, 89, 144]
        after = [113, 226, 175, 170, 111, 116, 120, 144]
        vtx = vtx_control.VtxController(ctx)
        label, _, _ = vtx.find_switch(before, after, self.LABELS)
        self.assertEqual(label, 'R2')

    def test_confirmation_waits_for_the_rise(self):
        ctx, nodes, cal = self.context(count=8, bands=['R'] * 8)
        before = [90, 95, 70, 116, 95, 109, 88, 189]
        vtx = vtx_control.VtxController(ctx)
        reads = iter([
            [90, 95, 70, 116, 95, 109, 88, 189],   # nothing yet
            [90, 190, 70, 116, 95, 109, 88, 95],   # R2 arrives
            [90, 190, 70, 116, 95, 109, 88, 95],
        ])
        with patch.object(vtx, 'read_levels', side_effect=lambda *a, **k: next(reads)), \
                patch('vtx_control.gevent.sleep'):
            confirmed, detail = vtx.confirm_channel('R2', self.LABELS, before)
        self.assertTrue(confirmed, detail)

    #
    # The sweep itself
    #

    def test_each_capture_is_of_its_own_channel(self):
        """The peaks are cleared between channels.

        A peak only ever rises, so without clearing, every capture carries the
        highest reading from every channel before it - a measured run recorded
        the same 191 on node 1 for all three channels, which was what it saw
        while the quad was on the first of them.
        """
        ctx, nodes, cal = self.context(count=2)
        self.controller(ctx)
        cal._eq_mode = 'auto'
        cal._eq_captured = {'noise': [90, 90]}
        cal._eq_note_capture_session()

        vtx = cal._vtx()
        with patch.object(vtx, 'confirm_channel', return_value=(True, 'on air')), \
                patch.object(vtx, 'read_levels', return_value=[90, 90]), \
                patch('calibration.gevent.sleep'):
            nodes[0].node_peak_rssi = 180
            nodes[1].node_peak_rssi = 175
            cal.eq_sweep_level('high')

        # Once per channel, so neither capture inherits the other's peak.
        self.assertEqual(ctx.interface.reset_node_extremums.call_count, 4)

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
            nodes[0].current_rssi = 180
            nodes[1].current_rssi = 175
            self.assertTrue(cal.eq_sweep_level('high'))

        self.assertEqual(sent, ['R2', 'R1', 'R2'])
        self.assertIn('high:R1', cal._eq_captured)
        self.assertIn('high:R2', cal._eq_captured)

    def test_the_command_is_repeated_while_waiting(self):
        """A lost command is replaced during the wait, not after it.

        Waiting out the whole limit and then starting again costs a timeout per
        attempt; re-sending inside the wait replaces a lost command within a few
        seconds and still costs only one timeout when the channel is never going
        to switch.
        """
        ctx, nodes, cal = self.context(count=2)
        sent = self.controller(ctx)
        cal._eq_mode = 'auto'
        cal._eq_captured = {'noise': [90, 90]}
        cal._eq_note_capture_session()

        def confirm(label, floors, channels, before=None, cancelled=None,
                    resend=None, **kwargs):
            # Two resends inside one wait, then the change appears.
            resend()
            resend()
            return (True, 'confirmed')

        with patch.object(cal._vtx(), 'confirm_channel', side_effect=confirm), \
                patch('calibration.gevent.sleep'):
            nodes[0].current_rssi = 180
            nodes[1].current_rssi = 175
            self.assertTrue(cal.eq_sweep_level('high'))

        # Three sends for the first channel: the original plus two resends.
        self.assertEqual(sent, ['R2', 'R1', 'R1', 'R1', 'R2', 'R2', 'R2'])
        self.assertIn('high:R1', cal._eq_captured)
        self.assertIn('high:R2', cal._eq_captured)

    def test_a_channel_that_never_switches_costs_one_wait(self):
        ctx, nodes, cal = self.context(count=2)
        sent = self.controller(ctx)
        cal._eq_mode = 'auto'
        cal._eq_captured = {'noise': [90, 90]}
        cal._eq_note_capture_session()

        calls = []

        def confirm(label, floors, channels, before=None, cancelled=None,
                    resend=None, **kwargs):
            calls.append(label)
            return (False, 'no change')

        with patch.object(cal._vtx(), 'confirm_channel', side_effect=confirm), \
                patch('calibration.gevent.sleep'):
            self.assertFalse(cal.eq_sweep_level('high'))

        # One wait for the first channel, then the sweep stops.
        self.assertEqual(calls, ['R1'])
        self.assertEqual(sent, ['R2', 'R1'])

    def test_cancel_during_failed_confirmation_does_not_retry(self):
        ctx, nodes, cal = self.context(count=2)
        sent = self.controller(ctx)
        cal._eq_mode = 'auto'
        cal._eq_captured = {'noise': [90, 90]}
        cal._eq_note_capture_session()
        def cancel(*args, **kwargs):
            cal._eq_cancelled = True
            return False, 'cancelled'
        with patch.object(cal._vtx(), 'confirm_channel', side_effect=cancel), \
                patch('calibration.gevent.sleep'):
            self.assertFalse(cal.eq_sweep_level('high'))
        self.assertEqual(sent, ['R2', 'R1'])
        self.assertNotIn('high:R1', cal._eq_captured)

    def test_an_unconfirmed_channel_is_never_captured(self):
        """The whole point: a reading is only kept where the channel was proven.

        A reading taken while the VTX sat elsewhere would fit a plausible
        correction to the wrong channel, which is worse than none at all.
        """
        ctx, nodes, cal = self.context(count=2)
        self.controller(ctx)
        cal._eq_mode = 'auto'
        cal._eq_captured = {'noise': [90, 90]}
        cal._eq_note_capture_session()

        answers = iter([(True, 'margin 90')] + [(False, 'reading R1, not R2')] * 4)
        vtx = cal._vtx()
        with patch.object(vtx, 'confirm_channel', side_effect=lambda *a, **k: next(answers)), \
                patch('calibration.gevent.sleep'):
            nodes[0].node_peak_rssi = 180
            nodes[1].node_peak_rssi = 175
            nodes[0].current_rssi = 180
            nodes[1].current_rssi = 175
            self.assertFalse(cal.eq_sweep_level('high'))

        self.assertIn('high:R1', cal._eq_captured)
        self.assertNotIn('high:R2', cal._eq_captured)
        self.assertEqual(len(cal.eq_sweep_state()['skipped']), 1)

    def test_sweep_stops_at_the_first_channel_it_cannot_confirm(self):
        """A VTX that is not listening fails every channel the same way.

        Working through the rest makes the operator wait out a timeout per
        channel to be told what the first one already said.
        """
        ctx, nodes, cal = self.context(count=2)
        sent = self.controller(ctx)
        cal._eq_mode = 'auto'
        cal._eq_captured = {'noise': [90, 90]}
        cal._eq_note_capture_session()

        vtx = cal._vtx()
        with patch.object(vtx, 'confirm_channel',
                          return_value=(False, 'no channel stands out')), \
                patch('calibration.gevent.sleep'):
            self.assertFalse(cal.eq_sweep_level('high'))

        self.assertEqual(sent, ['R2', 'R1'])
        self.assertNotIn('high:R1', cal._eq_captured)
        self.assertEqual(len(cal.eq_sweep_state()['skipped']), 1)

    def test_cancel_stops_a_sweep_between_channels(self):
        ctx, nodes, cal = self.context(count=2)
        sent = self.controller(ctx)
        cal._eq_mode = 'auto'
        cal._eq_captured = {'noise': [90, 90]}
        cal._eq_note_capture_session()

        vtx = cal._vtx()

        def confirm(*args, **kwargs):
            cal._eq_cancelled = True
            return (True, 'margin 90')

        with patch.object(vtx, 'confirm_channel', side_effect=confirm), \
                patch('calibration.gevent.sleep'):
            nodes[0].node_peak_rssi = 180
            nodes[1].node_peak_rssi = 175
            nodes[0].current_rssi = 180
            nodes[1].current_rssi = 175
            self.assertFalse(cal.eq_sweep_level('high'))

        self.assertEqual(sent, ['R2', 'R1'])

    def test_cancel_drops_the_run_but_not_the_applied_calibration(self):
        ctx, _, cal = self.context(count=2)
        self.controller(ctx)
        cal.eq_wizard_set_mode('auto')
        cal.eq_sweep_set_scope('r')
        cal._eq_captured = {'noise': [90, 90]}
        cal._eq_note_capture_session()

        self.assertTrue(cal.eq_wizard_cancel())
        self.assertEqual(cal._eq_captured, {})
        self.assertIsNone(cal.eq_wizard_mode())
        self.assertIsNone(cal.eq_sweep_scope())
        # Cancelling a run must not push identity constants to the nodes.
        ctx.interface.set_equalisation.assert_not_called()

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

    def simulated_quad(self, start='R1', dropped=None, weak=False):
        ctx, nodes, cal = self.context(count=8)
        labels = self.LABELS
        sent = []
        floors = [90, 95, 70, 116, 95, 109, 88, 89]
        dropped = dict(dropped or {})
        ctx.signal_gain = 10 if weak else 45
        def tune(label):
            for idx, node in enumerate(nodes):
                gain = ctx.signal_gain if labels[idx] == label else 1
                node.current_rssi = floors[idx] + gain
                node.node_peak_rssi = node.current_rssi
        tune(start)
        def send(band, channel):
            label = band + str(channel)
            sent.append(label)
            if dropped.get(label, 0):
                dropped[label] -= 1
            else:
                tune(label)
        ctx.vrx_manager = SimpleNamespace(controllers={'radio': SimpleNamespace(
            send_set_vtx_config=send)})
        ctx.interface.reset_node_extremums.side_effect = \
            lambda idx: setattr(nodes[idx], 'node_peak_rssi', nodes[idx].current_rssi)
        cal._eq_mode = 'auto'
        cal._eq_captured = {'noise': floors}
        cal._eq_note_capture_session()
        return ctx, nodes, cal, sent

    def test_real_detector_completes_from_any_starting_channel(self):
        for start in self.LABELS:
            with self.subTest(start=start):
                ctx, nodes, cal, sent = self.simulated_quad(start)
                self.assertTrue(cal.eq_sweep_level('high'))
                self.assertEqual(sent, ['R8'] + self.LABELS)
                for idx, label in enumerate(self.LABELS):
                    values = cal._eq_captured['high:' + label]
                    self.assertEqual(values[idx] - cal._eq_captured['noise'][idx], 45)

    def test_real_detector_retries_dropped_commands(self):
        ctx, nodes, cal, sent = self.simulated_quad(dropped={'R1': 2})
        self.assertTrue(cal.eq_sweep_level('high'))
        self.assertEqual(sent[:4], ['R8', 'R1', 'R1', 'R1'])

    def test_real_detector_never_captures_a_channel_that_did_not_switch(self):
        ctx, nodes, cal, sent = self.simulated_quad(dropped={'R2': 99})
        self.assertFalse(cal.eq_sweep_level('high'))
        self.assertIn('high:R1', cal._eq_captured)
        self.assertNotIn('high:R2', cal._eq_captured)
        self.assertEqual(sent.count('R2'), 4)
        self.assertNotIn('R3', sent)

    def test_real_detector_handles_weak_far_position(self):
        ctx, nodes, cal, sent = self.simulated_quad(start='R1', weak=True)
        self.assertTrue(cal.eq_sweep_level('low'))
        self.assertEqual(cal.eq_sweep_state()['captured']['low'], self.LABELS)

    def test_capture_both_levels_then_apply(self):
        ctx, nodes, cal, sent = self.simulated_quad()
        profile = ctx.race.profile
        profile.enter_ats = json.dumps({'v': [160] * 8})
        profile.exit_ats = json.dumps({'v': [150] * 8})
        def save(data):
            for key, value in data.items():
                if key != 'profile_id':
                    setattr(profile, key, json.dumps(value))
            return profile
        ctx.rhdata.alter_profile.side_effect = save
        ctx.rhdata.get_profile.return_value = profile
        ctx.interface.set_equalisation.return_value = True
        self.assertTrue(cal.eq_sweep_level('high'))
        ctx.signal_gain = 10
        self.assertTrue(cal.eq_sweep_level('low'))
        self.assertEqual(cal.eq_sweep_state()['stage'], 'ready')
        captures = dict(cal._eq_captured)
        self.assertTrue(cal.eq_wizard_apply())
        self.assertEqual(ctx.interface.set_equalisation.call_count, 8)
        targets = cal._eq_destination([(10, 35)] * 8)
        for call in ctx.interface.set_equalisation.call_args_list:
            idx, pivot, ou, su, ol, sl = call.args
            levels = [captures['noise'][idx], captures['low:' + self.LABELS[idx]][idx],
                      captures['high:' + self.LABELS[idx]][idx]]
            for raw, expected in zip(levels, targets):
                value = ((raw - ou) * su if raw >= pivot else (raw - ol) * sl) >> 8
                self.assertLessEqual(abs(value - expected), 2)

    def test_nearly_equal_rises_are_ambiguous(self):
        ctx, nodes, cal = self.context(count=2)
        self.assertIsNone(cal._vtx().find_switch([90, 95], [120, 124], ['R1', 'R2'])[0])

    def test_unwatched_channels_are_rejected_before_sending(self):
        ctx, nodes, cal, sent = self.simulated_quad()
        cal._eq_scope = 'rl'
        self.assertFalse(cal.eq_sweep_level('high'))
        self.assertEqual(sent, [])

    def test_cancel_during_parking_sends_no_target(self):
        ctx, nodes, cal, sent = self.simulated_quad()
        def cancel(seconds):
            cal._eq_cancelled = True
            self.now += seconds
        with patch('calibration.gevent.sleep', side_effect=cancel):
            self.assertFalse(cal.eq_sweep_level('high'))
        self.assertEqual(sent, ['R8'])
        self.assertEqual(list(cal._eq_captured), ['noise'])

    #
    # Stepping the VTX by hand
    #

    def test_stepping_walks_the_band_one_channel_per_call(self):
        ctx, _, cal = self.context(count=2)
        sent = self.controller(ctx)
        cal.eq_sweep_set_scope('r')
        with patch('vtx_control.gevent.sleep'):
            for _ in range(3):
                cal.eq_vtx_test()
        self.assertEqual(sent, ['R1', 'R2', 'R3'])

    def test_stepping_wraps_at_the_end_of_the_band(self):
        ctx, _, cal = self.context(count=2)
        sent = self.controller(ctx)
        with patch('vtx_control.gevent.sleep'):
            for _ in range(3):
                cal.eq_vtx_test()
        # The default scope is the two channels the nodes are tuned to.
        self.assertEqual(sent, ['R1', 'R2', 'R1'])

    def test_stepping_takes_an_explicit_channel(self):
        ctx, _, cal = self.context(count=2)
        sent = self.controller(ctx)
        with patch('vtx_control.gevent.sleep'):
            cal.eq_vtx_test('R7')
        self.assertEqual(sent, ['R7'])

    def test_stepping_refuses_without_a_calibration_pilot(self):
        ctx, _, cal = self.context(count=2)
        sent = self.controller(ctx)
        ctx.rhdata.get_optionInt.return_value = 0
        self.assertFalse(cal.eq_vtx_test())
        self.assertEqual(sent, [])

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
