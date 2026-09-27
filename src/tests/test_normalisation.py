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
    def context(self, count=1):
        nodes = []
        for _ in range(count):
            node = Node()
            node.api_level = 37
            node.init()
            nodes.append(node)
        profile = SimpleNamespace(
            id=1, frequencies=json.dumps({'b': ['R'] * count,
                                          'c': list(range(1, count + 1))}))
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
        return ctx, nodes, Calibration(ctx)

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
            cal._norm_captured = {'noise': [f for f, _ in fleet]}
            for i, (_, h) in enumerate(fleet):
                cal._norm_captured['high:R{0}'.format(i + 1)] = [
                    h if j == i else None for j in range(len(fleet))]
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
            cal._norm_captured = {'noise': [f for f, _ in fleet]}
            for i, (_, h) in enumerate(fleet):
                cal._norm_captured['high:R{0}'.format(i + 1)] = [
                    h if j == i else None for j in range(len(fleet))]
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
        """
        fleet = [(18, 52), (22, 61), (15, 88), (20, 58),
                 (25, 95), (19, 90), (17, 70), (21, 80)]
        with patch('calibration.gevent.sleep'):
            ctx, _, cal = self.context(count=len(fleet))
            cal._norm_captured = {'noise': [f for f, _ in fleet]}
            for i, (_, h) in enumerate(fleet):
                cal._norm_captured['high:R{0}'.format(i + 1)] = [
                    h if j == i else None for j in range(len(fleet))]
            cal._norm_note_capture_session()
            self.assertTrue(cal.norm_wizard_apply())

            floors = []
            for call in ctx.interface.set_normalisation.call_args_list:
                idx, pivot, offset, scale = call.args
                floors.append(self.corrected(fleet[idx][0], pivot, offset, scale))
            self.assertLessEqual(max(floors) - min(floors), 1)
            # and not on zero, which is the node's "no peak recorded" sentinel
            self.assertGreater(min(floors), 0)

    def test_the_pivot_sits_at_the_ratio_of_the_captured_range(self):
        import calibration as cal_mod
        _, _, cal = self.context()
        pivot, offset, scale = cal._norm_fit(20, 120, 120)
        self.assertEqual(pivot, 20 + round((120 - 20) * cal_mod.NORM_PIVOT_RATIO))
        self.assertEqual(offset, 0)   # this node defines the target
        self.assertGreater(scale, 0)

    def test_the_transfer_function_is_monotonic_and_has_no_step(self):
        """A jump at the pivot would read as movement that never happened."""
        import calibration as cal_mod
        _, _, cal = self.context()
        for floor, gate in ((18, 52), (15, 88), (25, 95), (22, 61)):
            pivot, offset, scale = cal._norm_fit(floor, gate, 95)
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
            cal._norm_captured = {'noise': [20], 'high:R1': [120]}
            cal._norm_note_capture_session()
            self.assertEqual(cal.norm_wizard_state()['state'], 'ready')
            self.assertTrue(cal.norm_wizard_apply())
            self.assertEqual(cal.norm_wizard_state()['state'], 'applied')

    def test_disabled_node_does_not_take_part(self):
        """A node with no frequency raises no step and gets no fit."""
        ctx, _, cal = self.context(count=2)
        ctx.race.profile.frequencies = json.dumps(
            {'b': ['R', 'R'], 'c': [1, 2], 'f': [5658, 0]})
        self.assertEqual(cal._norm_participants(), [0])
        self.assertIsNone(cal._norm_node_channels()[1])
        # only the enabled node's channel raises a gate step
        labels = [chan for _, chan in cal._norm_steps() if chan]
        self.assertEqual(sorted(set(labels)), ['R1'])

    def test_the_sweep_is_noise_then_one_gate_per_channel(self):
        """Two captures per node, so the quad is placed once for the whole run.

        The mid-power level is gone: the fit derives the pivot from the floor
        and the gate. For eight nodes on distinct channels that is 9 steps
        against the 17 the three-level sweep took.
        """
        _, _, cal = self.context(count=3)
        steps = cal._norm_steps()
        self.assertEqual(steps[0], ('noise', None))
        self.assertEqual(steps[1:], [('high', 'R1'), ('high', 'R2'), ('high', 'R3')])

        _, _, big = self.context(count=8)
        self.assertEqual(len(big._norm_steps()), 9)

    def test_captures_are_keyed_not_positional(self):
        """The captures are keyed by level and channel, not by capture order."""
        with patch('calibration.gevent.sleep'):
            ctx, _, cal = self.context(count=2)
            cal._norm_captured = {'high:R2': [None, 140],
                                  'noise': [20, 22],
                                  'high:R1': [120, None]}
            cal._norm_note_capture_session()
            self.assertEqual(cal.norm_wizard_state()['state'], 'ready')
            self.assertTrue(cal.norm_wizard_apply())
            sent = {c.args[0]: c.args for c in
                    ctx.interface.set_normalisation.call_args_list}
            target = 140
            for idx, gate in ((0, 120), (1, 140)):
                _, pivot, offset, scale = sent[idx]
                self.assertEqual(self.corrected(gate, pivot, offset, scale), target)

    def test_back_discards_the_step_most_recently_captured(self):
        """Back drops the last gate captured and re-arms that step."""
        _, _, cal = self.context(count=2)
        cal._norm_captured = {'noise': [1, 1], 'high:R1': [2, 2],
                              'high:R2': [3, 3]}
        cal._norm_note_capture_session()
        self.assertTrue(cal.norm_wizard_back())
        self.assertNotIn('high:R2', cal._norm_captured)
        self.assertIn('high:R1', cal._norm_captured)
        self.assertEqual(cal.norm_wizard_state()['level'], 'high')
        self.assertEqual(cal.norm_wizard_state()['channel'], 'R2')

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
        self.assertTrue(self.capture(cal, nodes, 'noise', [90, 95]))
        self.assertTrue(self.capture(cal, nodes, 'high', [187, 100]))   # R1 ok
        self.assertIn('high:R1', cal._norm_captured)
        # R2's gate sits 6 counts over its floor: the quad never arrived
        self.assertFalse(self.capture(cal, nodes, 'high', [100, 101]))
        # the good R1 gate goes too - it shared the bad placement
        self.assertNotIn('high:R1', cal._norm_captured)
        self.assertNotIn('high:R2', cal._norm_captured)
        # noise was measured with no quad at all and survives
        self.assertIn('noise', cal._norm_captured)
        state = cal.norm_wizard_state()
        self.assertEqual((state['level'], state['channel']), ('high', 'R1'))

    def test_a_healthy_gate_is_accepted(self):
        """The guard must not fire on a pass that is merely close to the limit."""
        _, nodes, cal = self.context(count=1)
        self.assertTrue(self.capture(cal, nodes, 'noise', [90]))
        gap = cal._norm_min_gap()
        self.assertTrue(self.capture(cal, nodes, 'high', [90 + gap]))  # exactly the limit
        self.assertIn('high:R1', cal._norm_captured)

    def test_only_the_node_on_that_channel_is_judged(self):
        """Off-channel nodes read bleed, so their tiny rise means nothing."""
        _, nodes, cal = self.context(count=2)
        self.assertTrue(self.capture(cal, nodes, 'noise', [90, 95]))
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
        self.capture(cal, nodes, 'noise', [90, 95, 68])
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

    def test_the_noise_button_is_offered_only_once_noise_is_in(self):
        _, nodes, cal = self.context(count=2)
        self.assertFalse(cal.norm_wizard_state()['noise_ready'])
        self.assertFalse(cal.norm_wizard_apply_noise())
        ctx_calls = cal._racecontext.interface.set_normalisation.call_count
        self.assertEqual(ctx_calls, 0)
        self.capture(cal, nodes, 'noise', [90, 95])
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
        self.capture(cal, nodes, 'noise', [90])
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
            cal._norm_captured = dict(zip(('noise', 'high:R1'),
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
            cal._norm_captured = dict(zip(('noise', 'high:R1'),
                                        ([v] for v in (90, 210))))
            cal._norm_note_capture_session()
            self.assertFalse(cal.norm_wizard_apply())

    def test_captures_do_not_survive_a_profile_change(self):
        """A complete capture set belongs to the configuration that made it."""
        ctx, _, cal = self.context()
        cal._norm_captured = {'noise': [90], 'high:R1': [210]}
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
            cal._norm_captured = dict(zip(('noise', 'high:R1'),
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
            cal._norm_captured = dict(zip(('noise', 'high:R1'),
                                        ([v] for v in (90, 210))))
            cal._norm_note_capture_session()
            self.assertTrue(cal.norm_wizard_apply())
        ctx.interface.set_enter_at_level.assert_not_called()
        ctx.interface.set_exit_at_level.assert_not_called()

    def test_back_keeps_the_earlier_captures(self):
        """Stepping back cancels a pending capture, not the retained ones."""
        ctx, _, cal = self.context()
        cal._norm_captured = {'noise': [90], 'high:R1': [210]}
        cal._norm_note_capture_session()
        self.assertTrue(cal.norm_wizard_back())
        self.assertTrue(cal._norm_captures_are_current())
        # the state query must not discard what Back kept
        cal.norm_wizard_state()
        # the gate was the last step, so the noise floor is what survives
        self.assertEqual(sorted(cal._norm_captured), ['noise'])

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
            captures = dict(zip(('noise', 'high:R1'),
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
        cal._norm_captured = {'noise': [700], 'high:R1': [704]}
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
