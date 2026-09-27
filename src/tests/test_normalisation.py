"""Regression tests for per-node RSSI normalisation."""
import gevent.event
import gevent.lock
import importlib.util
import json
from pathlib import Path
import sqlite3
import shutil
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

SRC = Path(__file__).resolve().parents[1]
for folder in ('interface', 'server'):
    sys.path.insert(0, str(SRC / folder))

from Node import Node
from calibration import Calibration

spec = importlib.util.spec_from_file_location(
    'norm_migration', SRC / 'server/util/add_normalisation_columns.py')
migration = importlib.util.module_from_spec(spec)
spec.loader.exec_module(migration)


def cal_mod_full_scale():
    import calibration
    return calibration.NORM_FULL_SCALE


class NormalisationTest(unittest.TestCase):
    # The R band frequencies, so a fixture's channels resolve the way the sweep
    #  resolves them.
    R_FREQS = (5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917)

    def context(self, count=1, scope='current'):
        nodes = []
        for _ in range(count):
            node = Node()
            node.api_level = 37
            node.init()
            nodes.append(node)
        profile = SimpleNamespace(
            id=1, norm_per_freq=None,
            frequencies=json.dumps({'b': ['R'] * count,
                                    'c': list(range(1, count + 1)),
                                    'f': list(self.R_FREQS[:count])}))
        ctx = SimpleNamespace(race=SimpleNamespace(profile=profile, num_nodes=count),
                              interface=Mock(nodes=nodes), rhui=Mock(), rhdata=Mock(),
                              events=Mock())
        def save(data):
            for key, value in data.items():
                if key != 'profile_id':
                    setattr(profile, key, json.dumps(value))
            return profile
        ctx.rhdata.alter_profile.side_effect = save
        ctx.rhdata.get_profile.return_value = profile
        ctx.interface.set_normalisation.return_value = True
        cal = Calibration(ctx)
        # A run is not armed until a scope is chosen; the tests that care about
        #  the choice itself pass scope=None and make it explicitly.
        if scope:
            cal._norm_scope_sel = scope
            cal._norm_saved_freqs = cal._norm_profile_freqs()
            # Frozen as norm_start freezes it: the sweep rewrites the profile, so
            #  the channel list cannot be re-derived from it mid-run.
            cal._norm_channels = cal._norm_sweep_channels()
        return ctx, nodes, cal

    @classmethod
    def sweep_captures(cls, fleet):
        """Captures as the sweep files them: every node read on every channel.

        Each step tunes the whole fleet to one channel, so a channel's row holds
        a reading for every node, for both levels. A node reads its own `gate` on
        its own channel and a little less elsewhere, which is what a real
        receiver does.
        """
        out = {}
        for ch in range(len(fleet)):
            label = 'R{0}'.format(ch + 1)
            out['noise:' + label] = [f for f, _ in fleet]
            out['high:' + label] = [
                gate if node == ch else max(floor + 31, gate - 6)
                for node, (floor, gate) in enumerate(fleet)]
        return out

    @staticmethod
    def corrected(raw, pivot, offset, scale):
        """The node's own arithmetic, so a test asserts what hardware does."""
        if raw >= pivot:
            adj = raw - offset
        else:
            adj = (pivot - offset) + (((raw - pivot) * scale) >> 8)
        return max(0, adj)

    def test_the_gate_lands_on_the_target(self):
        """Every node's gate capture comes out on the same value."""
        fleet = [(18, 52), (22, 61), (15, 88), (20, 58),
                 (25, 95), (19, 90), (17, 70), (21, 80)]
        with patch('calibration.gevent.sleep'):
            ctx, _, cal = self.context(count=len(fleet))
            cal._norm_captured = self.sweep_captures(fleet)
            cal._norm_note_capture_session()
            self.assertTrue(cal.norm_wizard_apply())

            target = max(h for _, h in fleet)
            for call in ctx.interface.set_normalisation.call_args_list:
                idx, pivot, offset, scale = call.args
                self.assertEqual(
                    self.corrected(fleet[idx][1], pivot, offset, scale), target)

    def test_the_gate_region_carries_no_gain(self):
        """Above the pivot the correction is a translation and nothing more.

        This is the property the whole design exists for: the region that
        decides a lap must keep the raw curve's amplitude, slope and timing, so
        a fluctuation of n raw counts has to come out as n counts on every node.
        The superseded fit amplified it by 1.0 to 2.85x, differently per seat.
        """
        fleet = [(18, 52), (22, 61), (15, 88), (25, 95)]
        with patch('calibration.gevent.sleep'):
            ctx, _, cal = self.context(count=len(fleet))
            cal._norm_captured = self.sweep_captures(fleet)
            cal._norm_note_capture_session()
            self.assertTrue(cal.norm_wizard_apply())

            for call in ctx.interface.set_normalisation.call_args_list:
                idx, pivot, offset, scale = call.args
                gate = fleet[idx][1]
                for delta in (1, 5, 10):
                    out = (self.corrected(gate + delta, pivot, offset, scale)
                           - self.corrected(gate - delta, pivot, offset, scale))
                    self.assertEqual(out, 2 * delta)

    def test_the_floors_converge(self):
        """Every node's noise floor comes out on one value, within a count.

        Exactly equal is not reachable: the lower segment is a Q8 multiply, and
        the truncation differs per node. One count is well inside the noise the
        readings carry anyway.

        They converge on the reference node's own floor, which leaves headroom
        underneath: a signal below a node's floor still reads as a number rather
        than clamping at zero.
        """
        fleet = [(18, 52), (22, 61), (15, 88), (20, 58),
                 (25, 95), (19, 90), (17, 70), (21, 80)]
        with patch('calibration.gevent.sleep'):
            ctx, _, cal = self.context(count=len(fleet))
            cal._norm_captured = self.sweep_captures(fleet)
            cal._norm_note_capture_session()
            self.assertTrue(cal.norm_wizard_apply())

            floors = []
            for call in ctx.interface.set_normalisation.call_args_list:
                idx, pivot, offset, scale = call.args
                floors.append(self.corrected(fleet[idx][0], pivot, offset, scale))
            self.assertLessEqual(max(floors) - min(floors), 1)
            # on the reference node's floor: the one with the highest gate
            ref_floor = max(fleet, key=lambda fg: fg[1])[0]
            self.assertLessEqual(abs(min(floors) - ref_floor), 1)

    def test_the_reference_node_keeps_its_own_curve(self):
        """The node with the highest gate takes offset 0 and scale x1.00.

        Its curve is then a pure translation end to end - in fact no change at
        all - and every other node is bent onto it. Pinning the floors to a
        small fixed value instead forced x2.5 to x5.2 across the fleet, which
        amplified each node's own noise and clamped quiet signals to zero.
        """
        fleet = [(89, 187), (94, 149), (67, 159), (109, 191),
                 (91, 169), (106, 186), (83, 183), (87, 140)]
        with patch('calibration.gevent.sleep'):
            ctx, _, cal = self.context(count=len(fleet))
            cal._norm_captured = self.sweep_captures(fleet)
            cal._norm_note_capture_session()
            self.assertTrue(cal.norm_wizard_apply())

            ref = max(range(len(fleet)), key=lambda i: fleet[i][1])
            sent = {c.args[0]: c.args for c in
                    ctx.interface.set_normalisation.call_args_list}
            _, _, offset, scale = sent[ref]
            self.assertEqual(offset, 0)
            self.assertEqual(scale, 256)
            # and no node is stretched anywhere near what a fixed floor forced
            worst = max(sent[i][3] for i in range(len(fleet)))
            self.assertLess(worst, 2 * 256)

    def test_a_quiet_signal_does_not_clamp_to_zero(self):
        """Below a node's floor there must still be headroom, not the clamp."""
        fleet = [(89, 187), (94, 149), (67, 159), (109, 191)]
        with patch('calibration.gevent.sleep'):
            ctx, _, cal = self.context(count=len(fleet))
            cal._norm_captured = self.sweep_captures(fleet)
            cal._norm_note_capture_session()
            self.assertTrue(cal.norm_wizard_apply())

            sent = {c.args[0]: c.args for c in
                    ctx.interface.set_normalisation.call_args_list}
            for raw in (80, 100):
                out = [self.corrected(raw, *sent[i][1:]) for i in range(len(fleet))]
                self.assertTrue(all(v > 0 for v in out),
                                'raw {0} clamped somewhere: {1}'.format(raw, out))

    def fitted(self, fleet):
        """Apply a real fit for `fleet` and return the context."""
        ctx, _, cal = self.context(count=len(fleet))
        cal._norm_captured = self.sweep_captures(fleet)
        cal._norm_note_capture_session()
        with patch('calibration.gevent.sleep'):
            self.assertTrue(cal.norm_wizard_apply())
        return ctx, cal

    def test_suggested_enter_at_is_the_highest_pivot_not_the_mean(self):
        """Below its own pivot a node is scaled, so the mean is not safe.

        The mean sits under the pivot of every node above average - on the
        measured fleet that was three of eight - which would put their trigger in
        the region where the nodes do not agree.
        """
        fleet = [(89, 187), (94, 149), (67, 159), (109, 191),
                 (91, 169), (106, 186), (83, 183), (87, 140)]
        ctx, cal = self.fitted(fleet)
        enter_at, exit_at = cal.norm_suggested_thresholds()

        sent = {c.args[0]: c.args for c in
                ctx.interface.set_normalisation.call_args_list}
        pivots = [self.corrected(sent[i][1], *sent[i][1:])
                  for i in range(len(fleet))]
        self.assertEqual(enter_at, max(pivots))
        # every node triggers at or above its own pivot, so at unity gain
        for p in pivots:
            self.assertGreaterEqual(enter_at, p)
        self.assertLess(sum(pivots) / len(pivots), enter_at,
                        'the mean would be lower - that is the point')

    def test_suggested_exit_at_is_a_tenth_below_enter_at(self):
        import calibration as cal_mod
        fleet = [(89, 187), (94, 149), (67, 159), (109, 191)]
        ctx, cal = self.fitted(fleet)
        enter_at, exit_at = cal.norm_suggested_thresholds()
        self.assertEqual(
            exit_at,
            enter_at - round(cal_mod.NORM_HYSTERESIS_FRACTION * enter_at))

    def test_suggested_exit_at_stays_above_the_floor(self):
        """An ExitAt at or below the floor means a pass that never ends."""
        fleet = [(89, 187), (94, 149), (67, 159), (109, 191)]
        ctx, cal = self.fitted(fleet)
        enter_at, exit_at = cal.norm_suggested_thresholds()

        sent = {c.args[0]: c.args for c in
                ctx.interface.set_normalisation.call_args_list}
        floors = [self.corrected(fleet[i][0], *sent[i][1:])
                  for i in range(len(fleet))]
        self.assertGreater(exit_at, max(floors))
        self.assertLess(exit_at, enter_at)

    def test_no_suggestion_without_a_fit(self):
        _, _, cal = self.context(count=2)
        self.assertIsNone(cal.norm_suggested_thresholds())

    def test_applying_thresholds_writes_every_node(self):
        fleet = [(89, 187), (94, 149), (67, 159), (109, 191)]
        ctx, cal = self.fitted(fleet)
        ctx.race.profile.enter_ats = json.dumps({'v': [0] * len(fleet)})
        ctx.race.profile.exit_ats = json.dumps({'v': [0] * len(fleet)})
        enter_at, exit_at = cal.norm_suggested_thresholds()
        self.assertTrue(cal.norm_apply_thresholds(enter_at, exit_at))
        for idx in range(len(fleet)):
            self.assertEqual(
                ctx.interface.set_enter_at_level.call_args_list[idx].args[:2],
                (idx, enter_at))
            self.assertEqual(
                ctx.interface.set_exit_at_level.call_args_list[idx].args[:2],
                (idx, exit_at))

    def test_thresholds_out_of_order_are_refused(self):
        fleet = [(89, 187), (109, 191)]
        ctx, cal = self.fitted(fleet)
        ctx.race.profile.enter_ats = json.dumps({'v': [0] * len(fleet)})
        ctx.race.profile.exit_ats = json.dumps({'v': [0] * len(fleet)})
        ctx.interface.set_enter_at_level.reset_mock()
        for en, ex in ((120, 120), (120, 130), (0, 0), (300, 100)):
            self.assertFalse(cal.norm_apply_thresholds(en, ex))
        self.assertFalse(cal.norm_apply_thresholds('abc', 100))
        ctx.interface.set_enter_at_level.assert_not_called()

    def test_a_scope_must_be_chosen_before_anything_is_armed(self):
        """The sweep retunes the whole fleet, so which channels it visits first."""
        _, _, cal = self.context(count=2, scope=None)
        self.assertEqual(cal.norm_wizard_state()['state'], 'choosing')
        self.assertFalse(cal.norm_start('sideways'))
        self.assertEqual(cal.norm_wizard_state()['state'], 'choosing')
        self.assertTrue(cal.norm_start('R'))
        self.assertEqual(cal.norm_wizard_state()['state'], 'capturing')

    def test_the_band_scopes_sweep_the_whole_band(self):
        """Two passes - noise then gate - over every channel in the band."""
        import calibration as cal_mod
        _, _, cal = self.context(count=2, scope=None)
        self.assertTrue(cal.norm_start('R'))
        steps = cal._norm_steps()
        self.assertEqual(len(steps), 16)
        r = ['R{0}'.format(i) for i in range(1, 9)]
        self.assertEqual(steps[:8], [('noise', c) for c in r])
        self.assertEqual(steps[8:], [('high', c) for c in r])

        _, _, cal = self.context(count=2, scope=None)
        self.assertTrue(cal.norm_start('RL'))
        steps = cal._norm_steps()
        self.assertEqual(len(steps), 32)
        rl = r + ['L{0}'.format(i) for i in range(1, 9)]
        self.assertEqual(steps[:16], [('noise', c) for c in rl])
        self.assertEqual(steps[16:], [('high', c) for c in rl])
        # and the frequencies come from the band table, not the profile
        freqs = [f for _, _, f in cal._norm_sweep_channels()]
        self.assertIn(cal_mod.NORM_BANDS['L'][0][1], freqs)

    def test_the_current_scope_sweeps_only_the_channels_in_use(self):
        _, _, cal = self.context(count=3, scope=None)
        self.assertTrue(cal.norm_start('current'))
        self.assertEqual(cal._norm_steps(),
                         [('noise', c) for c in ('R1', 'R2', 'R3')]
                         + [('high', c) for c in ('R1', 'R2', 'R3')])

    def test_a_capture_tunes_every_node_to_the_step_channel(self):
        """One channel per step, all nodes on it - that is what makes the fit
        per [node, frequency] instead of per node."""
        ctx, nodes, cal = self.context(count=3)
        for node in nodes:
            node.node_nadir_rssi = 90
            node.node_peak_rssi = 150
        with patch('calibration.gevent.sleep'):
            self.assertTrue(cal.norm_wizard_capture())   # noise
            ctx.interface.set_frequency.reset_mock()
            self.assertTrue(cal.norm_wizard_capture())   # first gate channel
        tuned = [c.args for c in ctx.interface.set_frequency.call_args_list]
        self.assertEqual(len(tuned), 3)
        # every node on the same frequency
        self.assertEqual(len({a[1] for a in tuned}), 1)
        self.assertEqual(sorted(a[0] for a in tuned), [0, 1, 2])

    def test_tuning_updates_the_profile_not_only_the_hardware(self):
        """The page reads the channel from the profile, and so does the restore.

        Writing only the hardware showed the operator their race assignment while
        the nodes were elsewhere, and left the restore looking every node's fit up
        under the wrong frequency.
        """
        ctx, _, cal = self.context(count=3, scope=None)
        self.assertTrue(cal.norm_start('R'))
        freqs = json.loads(ctx.race.profile.frequencies)
        self.assertEqual(freqs['c'][:3], [1, 1, 1])
        self.assertEqual(freqs['f'][:3], [self.R_FREQS[0]] * 3)
        # and what the run has to put back is the assignment it started from
        self.assertEqual(cal._norm_saved_freqs['f'][:3], list(self.R_FREQS[:3]))

    def test_restoring_puts_the_profile_back(self):
        fleet = [(89, 187), (94, 149), (67, 159)]
        ctx, cal = self.fitted(fleet)
        freqs = json.loads(ctx.race.profile.frequencies)
        self.assertEqual(freqs['f'][:3], list(self.R_FREQS[:3]))
        self.assertEqual(freqs['c'][:3], [1, 2, 3])

    def test_a_pass_captures_every_channel_by_itself(self):
        """One click per level: the operator sets the quad up once."""
        ctx, nodes, cal = self.context(count=3, scope=None)
        self.assertTrue(cal.norm_start('R'))
        for node in nodes:
            node.node_nadir_rssi = 90
            node.node_peak_rssi = 180
        with patch('calibration.gevent.sleep'):
            self.assertTrue(cal.norm_capture_pass())
        # the whole noise pass is in, and the gate pass is next
        state = cal.norm_wizard_state()
        self.assertEqual(state['level'], 'high')
        self.assertEqual(state['channel'], 'R1')
        self.assertEqual(sorted(cal._norm_captured),
                         ['noise:R{0}'.format(i) for i in range(1, 9)])
        # every channel was tuned, each to one frequency for the whole fleet
        tuned = [c.args[1] for c in ctx.interface.set_frequency.call_args_list]
        self.assertEqual(sorted(set(tuned)), sorted(self.R_FREQS))

    def test_a_pass_can_be_cancelled_between_channels(self):
        """Stopping keeps what was captured, never a half-read channel."""
        ctx, nodes, cal = self.context(count=3, scope=None)
        self.assertTrue(cal.norm_start('R'))
        for node in nodes:
            node.node_nadir_rssi = 90

        # cancel from inside the loop, after the first channel is filed
        real = cal.norm_wizard_capture
        def capture_then_cancel():
            ok = real()
            cal.norm_cancel_pass()
            return ok
        with patch('calibration.gevent.sleep'):
            with patch.object(cal, 'norm_wizard_capture', capture_then_cancel):
                self.assertFalse(cal.norm_capture_pass())

        self.assertIn('noise:R1', cal._norm_captured)
        self.assertNotIn('noise:R3', cal._norm_captured)
        # and the flag is cleared, so the next click runs again
        self.assertFalse(getattr(cal, '_norm_pass_running', False))
        with patch('calibration.gevent.sleep'):
            self.assertTrue(cal.norm_capture_pass())
        self.assertIn('noise:R8', cal._norm_captured)

    def test_cancel_does_nothing_when_no_pass_is_running(self):
        _, _, cal = self.context(count=2)
        self.assertFalse(cal.norm_cancel_pass())

    def test_retuning_waits_for_the_receivers_to_settle(self):
        """A reading taken mid-switch is neither channel."""
        import calibration as cal_mod
        ctx, nodes, cal = self.context(count=2, scope=None)
        for node in nodes:
            node.node_nadir_rssi = 90
        with patch('calibration.gevent.sleep') as slept:
            self.assertTrue(cal.norm_start('R'))
            self.assertTrue(cal.norm_wizard_capture())
        waits = [c.args[0] for c in slept.call_args_list]
        self.assertIn(cal_mod.NORM_RETUNE_SECONDS, waits)
        self.assertIn(cal_mod.NORM_SETTLE_SECONDS, waits)
        # the settle has to come after the retune wait, not before
        self.assertLess(waits.index(cal_mod.NORM_RETUNE_SECONDS),
                        len(waits) - 1)

    def test_a_pass_stops_at_a_channel_that_will_not_capture(self):
        """A rejected reading names its channel instead of being swept past."""
        ctx, nodes, cal = self.context(count=2, scope=None)
        self.assertTrue(cal.norm_start('R'))
        for node in nodes:
            node.node_nadir_rssi = 90
        with patch('calibration.gevent.sleep'):
            self.assertTrue(cal.norm_capture_pass())
            # gate pass: a gate that never rises above the floor is refused
            for node in nodes:
                node.node_peak_rssi = 92
            self.assertFalse(cal.norm_capture_pass())
        self.assertEqual(cal.norm_wizard_state()['level'], 'high')
        self.assertNotIn('high:R1', cal._norm_captured)

    def test_a_fit_is_stored_for_every_channel_swept(self):
        fleet = [(89, 187), (94, 149), (67, 159)]
        ctx, cal = self.fitted(fleet)
        table = json.loads(ctx.race.profile.norm_per_freq)
        # one row per channel in scope, each carrying a fit for every node
        self.assertEqual(len(table), len(fleet))
        for freq, row in table.items():
            self.assertEqual(len(row), len(fleet))
            for fit in row:
                self.assertEqual(len(fit), 3)
                self.assertTrue(fit[0], 'pivot 0 would disable the correction')

    def test_a_retune_sends_that_channels_own_fit(self):
        fleet = [(89, 187), (94, 149), (67, 159)]
        ctx, cal = self.fitted(fleet)
        table = json.loads(ctx.race.profile.norm_per_freq)
        other = sorted(table)[1]                 # a channel node 0 is not on
        ctx.interface.set_normalisation.reset_mock()

        self.assertTrue(cal.norm_apply_for_frequency(0, int(other)))
        idx, pivot, offset, scale = ctx.interface.set_normalisation.call_args.args
        self.assertEqual([idx, pivot, offset, scale],
                         [0] + list(table[other][0]))

    def test_an_uncalibrated_channel_leaves_the_node_uncorrected(self):
        """A wrong fit is worse than none: pivot 0 turns the correction off."""
        fleet = [(89, 187), (94, 149)]
        ctx, cal = self.fitted(fleet)
        ctx.interface.set_normalisation.reset_mock()

        self.assertFalse(cal.norm_apply_for_frequency(0, 5362))   # L1, never swept
        idx, pivot, offset, scale = ctx.interface.set_normalisation.call_args.args
        self.assertEqual((idx, pivot, offset, scale), (0, 0, 0, 256))

    def test_the_channel_assignment_comes_back_after_a_run(self):
        """The sweep parks the fleet on capture channels; that is not a race."""
        fleet = [(89, 187), (94, 149), (67, 159)]
        ctx, cal = self.fitted(fleet)
        restored = [c.args for c in ctx.interface.set_frequency.call_args_list[-3:]]
        self.assertEqual(sorted(a[0] for a in restored), [0, 1, 2])
        self.assertEqual([a[1] for a in restored], list(self.R_FREQS[:3]))

    def test_the_pivot_sits_at_the_ratio_of_the_captured_range(self):
        import calibration as cal_mod
        _, _, cal = self.context()
        pivot, offset, scale = cal._norm_fit(20, 120, 120, 20)
        self.assertEqual(pivot, 20 + round((120 - 20) * cal_mod.NORM_PIVOT_RATIO))
        self.assertEqual(offset, 0)   # this node defines the target
        self.assertGreater(scale, 0)

    def test_the_transfer_function_is_monotonic_and_has_no_step(self):
        """A jump at the pivot would read as movement that never happened."""
        import calibration as cal_mod
        _, _, cal = self.context()
        for floor, gate in ((18, 52), (15, 88), (25, 95), (22, 61)):
            pivot, offset, scale = cal._norm_fit(floor, gate, 95, 25)
            seq = [self.corrected(r, pivot, offset, scale)
                   for r in range(cal_mod.NORM_FULL_SCALE + 1)]
            self.assertEqual(seq, sorted(seq))
            step = (self.corrected(pivot, pivot, offset, scale)
                    - self.corrected(pivot - 1, pivot, offset, scale))
            # one count of the lower gain, which at these spans is about 4
            self.assertLessEqual(step, 1 + (scale + 255) // 256)

    def test_a_fit_needs_only_two_captures(self):
        """Floor and gate. The mid-power level the old fit needed is gone."""
        with patch('calibration.gevent.sleep'):
            ctx, _, cal = self.context()
            cal._norm_captured = {'noise:R1': [20], 'high:R1': [120]}
            cal._norm_note_capture_session()
            self.assertEqual(cal.norm_wizard_state()['state'], 'ready')
            self.assertTrue(cal.norm_wizard_apply())
            self.assertEqual(cal.norm_wizard_state()['state'], 'applied')

    def test_disabled_node_does_not_take_part(self):
        """A node with no frequency raises no step and gets no fit."""
        ctx, _, cal = self.context(count=2)
        ctx.race.profile.frequencies = json.dumps(
            {'b': ['R', 'R'], 'c': [1, 2], 'f': [5658, 0]})
        # the run's assignment is re-read, as norm_start would take it
        cal._norm_saved_freqs = None
        cal._norm_channels = None
        self.assertEqual(cal._norm_participants(), [0])
        self.assertIsNone(cal._norm_node_channels()[1])
        # only the enabled node's channel raises a gate step
        labels = [chan for _, chan in cal._norm_steps() if chan]
        self.assertEqual(sorted(set(labels)), ['R1'])

    def test_level_is_the_outer_loop(self):
        """The quad is powered off once for the noise pass, on once for the gate.

        Switching per channel instead would mean powering the VTX off and on
        sixteen times for an R+L run.
        """
        _, _, cal = self.context(count=3)
        levels = [level for level, _ in cal._norm_steps()]
        self.assertEqual(levels, ['noise'] * 3 + ['high'] * 3)

    def test_captures_are_keyed_not_positional(self):
        """The captures are keyed by level and channel, not by capture order."""
        with patch('calibration.gevent.sleep'):
            ctx, _, cal = self.context(count=2)
            # filed out of order: the second channel, then noise, then the first
            cal._norm_captured = {'high:R2': [114, 140],
                                  'noise:R2': [20, 22],
                                  'noise:R1': [20, 22],
                                  'high:R1': [120, 134]}
            cal._norm_note_capture_session()
            self.assertEqual(cal.norm_wizard_state()['state'], 'ready')
            self.assertTrue(cal.norm_wizard_apply())

            # node 1 sits on R1 and node 2 on R2, so each is sent its own
            #  channel's fit and both gates land on the sweep's highest, 140.
            sent = {c.args[0]: c.args for c in
                    ctx.interface.set_normalisation.call_args_list}
            for idx, gate in ((0, 120), (1, 140)):
                _, pivot, offset, scale = sent[idx]
                self.assertEqual(self.corrected(gate, pivot, offset, scale), 140)

    def test_back_discards_the_step_most_recently_captured(self):
        """Back drops the last gate captured and re-arms that step."""
        _, _, cal = self.context(count=2)
        cal._norm_captured = {'noise:R1': [1, 1], 'noise:R2': [1, 1],
                              'high:R1': [2, 2], 'high:R2': [3, 3]}
        cal._norm_note_capture_session()
        self.assertTrue(cal.norm_wizard_back())
        self.assertNotIn('high:R2', cal._norm_captured)
        self.assertIn('high:R1', cal._norm_captured)
        self.assertEqual(cal.norm_wizard_state()['level'], 'high')
        self.assertEqual(cal.norm_wizard_state()['channel'], 'R2')

    def noise_pass(self, cal, nodes, floors):
        """Run every step of the noise pass, each node reading `floors`."""
        for level, _ in cal._norm_steps():
            if level != 'noise':
                break
            if not self.capture(cal, nodes, 'noise', floors):
                return False
        return True

    def capture(self, cal, nodes, level, peaks):
        """Run one wizard capture with each node reading `peaks`."""
        for node, value in zip(nodes, peaks):
            if level == 'noise':
                node.node_nadir_rssi = value
            else:
                node.node_peak_rssi = value
        with patch('calibration.gevent.sleep'):
            return cal.norm_wizard_capture()

    def test_a_gate_too_near_the_floor_restarts_the_whole_pass(self):
        """One placement of the quad serves the pass, so all of it is suspect.

        The channels already captured were measured from the same wrong
        distance, so keeping them would bury the error until Apply.
        """
        _, nodes, cal = self.context(count=2)
        self.assertTrue(self.noise_pass(cal, nodes, [90, 95]))
        self.assertTrue(self.capture(cal, nodes, 'high', [187, 100]))   # R1 ok
        self.assertIn('high:R1', cal._norm_captured)
        # R2's gate sits 6 counts over its floor: the quad never arrived
        self.assertFalse(self.capture(cal, nodes, 'high', [100, 101]))
        # the good R1 gate goes too - it shared the bad placement
        self.assertNotIn('high:R1', cal._norm_captured)
        self.assertNotIn('high:R2', cal._norm_captured)
        # the noise pass was measured with no quad at all and survives
        self.assertIn('noise:R1', cal._norm_captured)
        self.assertIn('noise:R2', cal._norm_captured)
        state = cal.norm_wizard_state()
        self.assertEqual((state['level'], state['channel']), ('high', 'R1'))

    def test_a_healthy_gate_is_accepted(self):
        """The guard must not fire on a pass that is merely close to the limit."""
        _, nodes, cal = self.context(count=1)
        self.assertTrue(self.noise_pass(cal, nodes, [90]))
        gap = cal._norm_min_gap()
        self.assertTrue(self.capture(cal, nodes, 'high', [90 + gap]))  # exactly the limit
        self.assertIn('high:R1', cal._norm_captured)

    def test_only_the_node_on_that_channel_is_judged(self):
        """Off-channel nodes read bleed, so their tiny rise means nothing."""
        _, nodes, cal = self.context(count=2)
        self.assertTrue(self.noise_pass(cal, nodes, [90, 95]))
        # on R1 only node 1 is judged; node 2 reads bleed and barely moves
        self.assertTrue(self.capture(cal, nodes, 'high', [187, 96]))
        self.assertIn('high:R1', cal._norm_captured)

    def applied(self, count=1, pivot=150, offset=50, scale=300):
        """A context with a fit already stored and on the nodes."""
        ctx, nodes, cal = self.context(count=count)
        prof = ctx.race.profile
        prof.norm_pivots = json.dumps({'v': [pivot] * count})
        prof.norm_offsets = json.dumps({'v': [offset] * count})
        prof.norm_scales = json.dumps({'v': [scale] * count})
        return ctx, nodes, cal

    def test_the_coefficients_are_always_offered_even_with_no_fit(self):
        """"No correction" is an editable state, not missing data."""
        import calibration as cal_mod
        _, _, cal = self.context(count=2)
        for row in cal.norm_captured_table():
            self.assertEqual(row['offset'], 0)
            self.assertEqual(row['scale'], cal_mod.NORM_UNITY_SLOPE)

    def test_editing_the_offset_sends_it_to_the_node(self):
        ctx, _, cal = self.applied()
        self.assertTrue(cal.norm_wizard_set_coefficient(0, 'offset', -20))
        idx, pivot, offset, scale = ctx.interface.set_normalisation.call_args.args
        self.assertEqual((idx, pivot, offset, scale), (0, 150, -20, 300))
        self.assertEqual(cal.norm_captured_table()[0]['offset'], -20)

    def test_editing_the_scale_sends_it_to_the_node(self):
        ctx, _, cal = self.applied()
        self.assertTrue(cal.norm_wizard_set_coefficient(0, 'scale', 320))
        idx, pivot, offset, scale = ctx.interface.set_normalisation.call_args.args
        self.assertEqual((idx, pivot, offset, scale), (0, 150, 50, 320))
        self.assertEqual(cal.norm_captured_table()[0]['scale'], 320)

    def test_editing_the_pivot_sends_it_to_the_node(self):
        ctx, _, cal = self.applied()
        self.assertTrue(cal.norm_wizard_set_coefficient(0, 'pivot', 180))
        idx, pivot, offset, scale = ctx.interface.set_normalisation.call_args.args
        self.assertEqual((idx, pivot, offset, scale), (0, 180, 50, 300))
        self.assertEqual(cal.norm_captured_table()[0]['pivot'], 180)

    def test_raising_the_pivot_widens_the_unity_gain_region(self):
        """The point of the field: more of the curve kept at gain 1."""
        ctx, _, cal = self.applied(pivot=150, offset=50, scale=300)
        # at 160 the old pivot already gave unity gain; at 140 it did not
        self.assertTrue(cal.norm_wizard_set_coefficient(0, 'pivot', 120))
        _, pivot, offset, scale = ctx.interface.set_normalisation.call_args.args
        for raw in (130, 140, 149):
            gain = (self.corrected(raw + 5, pivot, offset, scale)
                    - self.corrected(raw - 5, pivot, offset, scale))
            self.assertEqual(gain, 10)

    def test_pivot_zero_is_refused(self):
        """0 is the node's correction-off sentinel; Reset is how you get there."""
        ctx, _, cal = self.applied()
        self.assertFalse(cal.norm_wizard_set_coefficient(0, 'pivot', 0))
        self.assertFalse(cal.norm_wizard_set_coefficient(
            0, 'pivot', cal_mod_full_scale() + 1))
        ctx.interface.set_normalisation.assert_not_called()
        self.assertTrue(ctx.rhui.emit_priority_message.called)

    def test_an_offset_edit_moves_the_gate_by_what_was_typed(self):
        """The offset is in counts, so the curve must move by exactly that."""
        ctx, _, cal = self.applied()
        _, pivot, offset, scale = (0, 150, 50, 300)
        before = self.corrected(200, pivot, offset, scale)
        self.assertTrue(cal.norm_wizard_set_coefficient(0, 'offset', offset - 10))
        _, pivot, offset, scale = ctx.interface.set_normalisation.call_args.args
        self.assertEqual(self.corrected(200, pivot, offset, scale), before + 10)

    def test_a_scale_edit_leaves_the_gate_region_alone(self):
        """The scale is the lower segment only; above the pivot nothing moves."""
        ctx, _, cal = self.applied()
        before = self.corrected(200, 150, 50, 300)
        self.assertTrue(cal.norm_wizard_set_coefficient(0, 'scale', 1024))
        _, pivot, offset, scale = ctx.interface.set_normalisation.call_args.args
        self.assertEqual(self.corrected(200, pivot, offset, scale), before)

    def test_unity_undoes_the_lower_correction_by_hand(self):
        import calibration as cal_mod
        ctx, _, cal = self.applied(scale=973)
        self.assertTrue(
            cal.norm_wizard_set_coefficient(0, 'scale', cal_mod.NORM_UNITY_SLOPE))
        self.assertEqual(cal.norm_captured_table()[0]['scale'], 256)

    def test_a_coefficient_outside_the_range_is_refused(self):
        import calibration as cal_mod
        ctx, _, cal = self.applied()
        for bad in (cal_mod.NORM_SLOPE_MIN - 1, cal_mod.NORM_SLOPE_MAX + 1):
            self.assertFalse(cal.norm_wizard_set_coefficient(0, 'scale', bad))
        for bad in (-cal_mod.NORM_FULL_SCALE - 1, cal_mod.NORM_FULL_SCALE + 1):
            self.assertFalse(cal.norm_wizard_set_coefficient(0, 'offset', bad))
        ctx.interface.set_normalisation.assert_not_called()
        self.assertTrue(ctx.rhui.emit_priority_message.called)

    def test_there_is_no_field_for_a_gain_above_the_pivot(self):
        """The gate region is a translation by design; the node has no such coefficient."""
        ctx, _, cal = self.applied()
        self.assertFalse(cal.norm_wizard_set_coefficient(0, 'slope_up', 512))
        self.assertFalse(cal.norm_wizard_set_coefficient(0, 'up', 512))
        ctx.interface.set_normalisation.assert_not_called()

    def test_junk_and_an_unfitted_node_are_refused(self):
        ctx, _, cal = self.applied()
        self.assertFalse(cal.norm_wizard_set_coefficient(0, 'sideways', 300))
        self.assertFalse(cal.norm_wizard_set_coefficient(0, 'scale', 'abc'))
        self.assertFalse(cal.norm_wizard_set_coefficient(9, 'scale', 300))
        ctx.interface.set_normalisation.assert_not_called()

        _, _, fresh = self.context(count=1)   # pivot 0: nothing to adjust
        self.assertFalse(fresh.norm_wizard_set_coefficient(0, 'scale', 300))

    def test_a_coefficient_the_node_refuses_is_not_stored(self):
        ctx, _, cal = self.applied()
        ctx.interface.set_normalisation.return_value = False
        self.assertFalse(cal.norm_wizard_set_coefficient(0, 'scale', 320))
        self.assertEqual(cal.norm_captured_table()[0]['scale'], 300)

    def test_noise_alone_levels_the_floors(self):
        """No quad in the air, so no slope - the offsets do all the work."""
        import calibration as cal_mod
        ctx, nodes, cal = self.context(count=3)
        self.noise_pass(cal, nodes, [90, 95, 68])
        self.assertTrue(cal.norm_wizard_state()['noise_ready'])
        self.assertTrue(cal.norm_wizard_apply_noise())

        sent = {c.args[0]: c.args for c in ctx.interface.set_normalisation.call_args_list}
        self.assertEqual(sorted(sent), [0, 1, 2])
        for idx, floor in enumerate((90, 95, 68)):
            _, pivot, offset, scale = sent[idx]
            # unity scale: one measured point supports subtraction, not gain
            self.assertEqual(scale, cal_mod.NORM_UNITY_SLOPE)
            # every floor lands on the quietest node's floor
            self.assertEqual(floor - offset, 68)
            self.assertTrue(pivot, 'pivot 0 would disable the correction')

    def test_the_noise_button_is_offered_only_once_the_pass_is_done(self):
        """A floor belongs to a channel, so levelling waits for the whole pass."""
        _, nodes, cal = self.context(count=2)
        self.assertFalse(cal.norm_wizard_state()['noise_ready'])
        self.assertFalse(cal.norm_wizard_apply_noise())
        ctx_calls = cal._racecontext.interface.set_normalisation.call_count
        self.assertEqual(ctx_calls, 0)
        # one channel of the pass is not enough
        self.capture(cal, nodes, 'noise', [90, 95])
        self.assertFalse(cal.norm_wizard_state()['noise_ready'])
        self.assertTrue(self.noise_pass(cal, nodes, [90, 95]))
        self.assertTrue(cal.norm_wizard_state()['noise_ready'])

    def test_noise_levelling_is_not_stored_when_a_node_refuses(self):
        ctx, nodes, cal = self.context(count=2)
        self.capture(cal, nodes, 'noise', [90, 95])
        ctx.interface.set_normalisation.return_value = False
        self.assertFalse(cal.norm_wizard_apply_noise())
        self.assertTrue(cal.norm_state_is_unresolved())
        # nothing written to the profile
        self.assertFalse(any(cal._norm_stored('norm_pivots', 0)))

    def test_levelling_discards_the_captures_it_was_made_from(self):
        """Readings taken after levelling sit on a different axis.

        The nodes now subtract an offset, so a capture from before the
        levelling cannot be compared with one from after it: the difference
        between them measures the levelling, not the receiver. This bit in
        practice - a noise floor captured at 94 against a low captured at 103
        once the floors had moved to 67 looked like a 9-count span and was
        refused, when the real span was 36.
        """
        _, nodes, cal = self.context(count=2)
        self.capture(cal, nodes, 'noise', [90, 95])
        self.assertTrue(cal.norm_wizard_apply_noise())
        self.assertEqual(cal._norm_captured, {})
        state = cal.norm_wizard_state()
        self.assertEqual((state['state'], state['level']), ('capturing', 'noise'))

    def test_a_full_sweep_still_overrides_levelled_floors(self):
        """Levelling is a starting point, not a substitute for the fit."""
        ctx, nodes, cal = self.context(count=1)
        self.noise_pass(cal, nodes, [90])
        self.assertTrue(cal.norm_wizard_apply_noise())
        # the sweep restarts against the levelled nodes, which now read lower
        self.capture(cal, nodes, 'noise', [67])
        self.capture(cal, nodes, 'high', [210])
        with patch('calibration.gevent.sleep'):
            self.assertTrue(cal.norm_wizard_apply())
        _, pivot, offset, scale = ctx.interface.set_normalisation.call_args.args
        # derived from the new captures, not the levelling pivot of 1
        import calibration as cal_mod
        self.assertEqual(pivot, 67 + round((210 - 67) * cal_mod.NORM_PIVOT_RATIO))

    def test_unconfirmed_write_is_not_recorded(self):
        """A coefficient write the node never acknowledged is not success."""
        import RHInterface
        _, nodes, _ = self.context()
        interface = RHInterface.RHInterface.__new__(RHInterface.RHInterface)
        interface.nodes = nodes
        interface.log = lambda *a, **k: None
        nodes[0].norm_pivot = 0
        with patch.object(RHInterface.RHInterface, 'set_and_validate_value_16',
                          return_value=120), \
             patch.object(RHInterface.RHInterface, 'get_value_16', return_value=None):
            ok = RHInterface.RHInterface.set_normalisation(
                interface, 0, 120, 89, 256)
        self.assertFalse(ok)
        self.assertEqual(nodes[0].norm_pivot, 0)

    def test_capture_is_discarded_when_state_changes(self):
        """A reset landing during the settle invalidates the reading."""
        ctx, _, cal = self.context()
        before = cal._norm_session()
        cal._norm_invalidate_session()
        self.assertNotEqual(cal._norm_session(), before)

    def test_apply_leaves_the_thresholds_alone(self):
        """EnterAt/ExitAt are the operator's, and applying a fit must not move them.

        The correction does change what a given number means, but a tuned
        threshold is a judgement about what the gate should trigger on, and
        rewriting it silently is worse than leaving it for the operator.
        """
        with patch('calibration.gevent.sleep'):
            ctx, _, cal = self.context()
            ctx.race.profile.enter_ats = json.dumps({'v': [169]})
            ctx.race.profile.exit_ats = json.dumps({'v': [160]})
            cal._norm_captured = dict(zip(('noise:R1', 'high:R1'),
                                        ([v] for v in (90, 210))))
            cal._norm_note_capture_session()
            self.assertTrue(cal.norm_wizard_apply())
            self.assertEqual(json.loads(ctx.race.profile.enter_ats)['v'], [169])
            self.assertEqual(json.loads(ctx.race.profile.exit_ats)['v'], [160])

    def test_apply_refuses_when_a_node_does_not_confirm(self):
        """An unconfirmed write leaves the axis unknown, not merely changed."""
        with patch('calibration.gevent.sleep'):
            ctx, _, cal = self.context()
            ctx.interface.set_normalisation.return_value = False
            cal._norm_captured = dict(zip(('noise:R1', 'high:R1'),
                                        ([v] for v in (90, 210))))
            cal._norm_note_capture_session()
            self.assertFalse(cal.norm_wizard_apply())

    def test_captures_do_not_survive_a_profile_change(self):
        """A complete capture set belongs to the configuration that made it."""
        ctx, _, cal = self.context()
        cal._norm_captured = {'noise:R1': [90], 'high:R1': [210]}
        cal._norm_note_capture_session()
        self.assertTrue(cal._norm_captures_are_current())
        ctx.race.profile.id = 2
        self.assertFalse(cal._norm_captures_are_current())

    def test_reset_refuses_when_a_node_does_not_confirm(self):
        """An unconfirmed reset leaves the correction unknown."""
        ctx, _, cal = self.context()
        ctx.race.profile.enter_ats = json.dumps({'v': [80], 'eq': [[150, 89, 256, 89, 256]]})
        ctx.interface.set_normalisation.return_value = False
        self.assertFalse(cal.norm_wizard_reset())
        # thresholds must not move as though correction were off
        self.assertEqual(json.loads(ctx.race.profile.enter_ats)['v'], [80])
        self.assertTrue(cal.norm_state_is_unresolved())

    def test_failed_apply_blocks_racing(self):
        """Unresolved hardware state has to stop a race starting."""
        with patch('calibration.gevent.sleep'):
            ctx, _, cal = self.context()
            ctx.interface.set_normalisation.return_value = False
            cal._norm_captured = dict(zip(('noise:R1', 'high:R1'),
                                        ([v] for v in (90, 210))))
            cal._norm_note_capture_session()
            self.assertFalse(cal.norm_wizard_apply())
            self.assertTrue(cal.norm_state_is_unresolved())
            self.assertEqual(cal.norm_unresolved_nodes(), [1])

    def test_apply_writes_no_thresholds_at_all(self):
        """Applying a fit touches the node constants and nothing else."""
        with patch('calibration.gevent.sleep'):
            ctx, _, cal = self.context()
            ctx.race.profile.enter_ats = json.dumps({'v': [169]})
            ctx.race.profile.exit_ats = json.dumps({'v': [160]})
            cal._norm_captured = dict(zip(('noise:R1', 'high:R1'),
                                        ([v] for v in (90, 210))))
            cal._norm_note_capture_session()
            self.assertTrue(cal.norm_wizard_apply())
        ctx.interface.set_enter_at_level.assert_not_called()
        ctx.interface.set_exit_at_level.assert_not_called()

    def test_back_keeps_the_earlier_captures(self):
        """Stepping back cancels a pending capture, not the retained ones."""
        ctx, _, cal = self.context()
        cal._norm_captured = {'noise:R1': [90], 'high:R1': [210]}
        cal._norm_note_capture_session()
        self.assertTrue(cal.norm_wizard_back())
        self.assertTrue(cal._norm_captures_are_current())
        # the state query must not discard what Back kept
        cal.norm_wizard_state()
        # the gate was the last step, so the noise floor is what survives
        self.assertEqual(sorted(cal._norm_captured), ['noise:R1'])

    def test_history_ignores_races_under_another_correction(self):
        """Adaptive calibration must not restore thresholds from another axis."""
        ctx, _, cal = self.context()
        ctx.race.profile.norm_pivots = json.dumps({'v': [150]})
        ctx.race.profile.norm_offsets = json.dumps({'v': [89]})
        ctx.race.profile.norm_scales = json.dumps({'v': [256]})
        live = cal._norm_signature()
        race = object()
        values = {'norm_signature': json.dumps(live)}
        ctx.rhdata.get_savedrace_attribute_value.side_effect = \
            lambda r, name, default=None: values.get(name, default)
        self.assertTrue(cal._race_matches_correction(race))
        values['norm_signature'] = json.dumps([[151, 89, 256, 89, 256]])
        self.assertFalse(cal._race_matches_correction(race))
        # an untagged race predates equalisation: uncorrected
        values.clear()
        self.assertFalse(cal._race_matches_correction(race))

    def test_a_failed_apply_then_a_retry_leaves_the_thresholds_alone(self):
        """Neither the failure nor the retry may touch EnterAt/ExitAt."""
        with patch('calibration.gevent.sleep'):
            ctx, _, cal = self.context()
            ctx.race.profile.enter_ats = json.dumps({'v': [169]})
            ctx.race.profile.exit_ats = json.dumps({'v': [160]})
            captures = dict(zip(('noise:R1', 'high:R1'),
                                ([v] for v in (90, 210))))

            ctx.interface.set_normalisation.return_value = False
            cal._norm_captured = dict(captures)
            cal._norm_note_capture_session()
            self.assertFalse(cal.norm_wizard_apply())
            self.assertEqual(json.loads(ctx.race.profile.enter_ats)['v'], [169])

            ctx.interface.set_normalisation.return_value = True
            cal._norm_captured = dict(captures)
            cal._norm_note_capture_session()
            self.assertTrue(cal.norm_wizard_apply())
            self.assertEqual(json.loads(ctx.race.profile.enter_ats)['v'], [169])
            self.assertFalse(cal.norm_state_is_unresolved())

    def test_reset_leaves_the_thresholds_alone(self):
        """Clearing the correction must not rewrite them either."""
        ctx, _, cal = self.context()
        ctx.race.profile.enter_ats = json.dumps({'v': [80]})
        ctx.race.profile.exit_ats = json.dumps({'v': [75]})
        ctx.race.profile.norm_pivots = json.dumps({'v': [150]})

        ctx.interface.set_normalisation.return_value = False
        self.assertFalse(cal.norm_wizard_reset())
        self.assertEqual(json.loads(ctx.race.profile.enter_ats)['v'], [80])

        ctx.interface.set_normalisation.return_value = True
        self.assertTrue(cal.norm_wizard_reset())
        self.assertEqual(json.loads(ctx.race.profile.enter_ats)['v'], [80])
        self.assertEqual(json.loads(ctx.race.profile.exit_ats)['v'], [75])
        self.assertFalse(cal.norm_state_is_unresolved())

    def test_reset_holds_the_guard_through_its_tracking_reset(self):
        """The final tracking reset is a hardware mutation too."""
        seen = []
        ctx, _, cal = self.context()
        ctx.interface.reset_node_extremums.side_effect = \
            lambda *a, **k: seen.append(cal._norm_busy)
        self.assertTrue(cal.norm_wizard_reset())
        self.assertTrue(seen, 'tracking was never reset')
        self.assertTrue(all(seen), 'guard released before the tracking reset')

    def test_capture_rejects_noise_only_signal(self):
        ctx, _, cal = self.context()
        cal._norm_captured = {'noise:R1': [700], 'high:R1': [704]}
        self.assertFalse(cal.norm_wizard_apply())
        ctx.rhdata.alter_profile.assert_not_called()

    def test_opcodes_are_unique(self):
        import re
        header = (SRC / 'node/commands.h').read_text()
        names = re.findall(
            r'^#define ((?:READ_|WRITE_|RESET_NODE_)\w+) (0x[0-9A-F]+)',
            header, re.M)
        values = [int(value, 16) for _, value in names]
        self.assertEqual(len(values), len(set(values)))
        import RHInterface
        for name, value in names:
            if hasattr(RHInterface, name):
                self.assertEqual(getattr(RHInterface, name), int(value, 16), name)

    def test_migration_adds_columns_once(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'db.sqlite'
            db = sqlite3.connect(path)
            db.execute('CREATE TABLE profiles (id INTEGER PRIMARY KEY, '
                       'enter_ats TEXT, exit_ats TEXT)')
            db.execute("INSERT INTO profiles VALUES (1, '{}', '{}')")
            db.commit()
            db.close()
            self.assertEqual(migration.main(str(path)), 0)
            self.assertEqual(migration.main(str(path)), 0)  # second run is a no-op
            db = sqlite3.connect(path)
            columns = {row[1] for row in db.execute('PRAGMA table_info(profiles)')}
            db.close()
            for column, _ in migration.COLUMNS:
                self.assertIn(column, columns)


if __name__ == '__main__':
    unittest.main()
