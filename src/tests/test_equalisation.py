"""Regression tests for per-node RSSI equalisation."""
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
    'eq_migration', SRC / 'server/util/add_equalisation_columns.py')
migration = importlib.util.module_from_spec(spec)
spec.loader.exec_module(migration)


class EqualisationTest(unittest.TestCase):
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
        ctx.interface.set_equalisation.return_value = True
        return ctx, nodes, Calibration(ctx)

    def test_fit_passes_through_the_captured_levels(self):
        values = [90, 150, 210]
        with patch('calibration.gevent.sleep'):
            ctx, _, cal = self.context()
            cal._eq_captured = dict(zip(('noise', 'low:R1', 'high:R1'),
                                        ([v] for v in values)))
            cal._eq_note_capture_session()
            self.assertTrue(cal.eq_wizard_apply())
            _, pivot, ou, su, ol, sl = ctx.interface.set_equalisation.call_args.args
            targets = cal._eq_destination([(values[1] - values[0],
                                            values[2] - values[1])])
            def corrected(raw):
                return ((raw - ou) * su if raw >= pivot else (raw - ol) * sl) >> 8
            for raw, target in zip(values, targets):
                self.assertLessEqual(abs(corrected(raw) - target), 2)
            self.assertEqual(cal.eq_wizard_state()['state'], 'applied')

    def test_no_node_is_compressed(self):
        """The correction must never shrink a node's captured spans.

        The destination comes from the widest span in the fleet, so the best
        node maps onto itself and every other node is stretched up to meet it.
        It follows the captures rather than any fixed fraction, so it holds for
        whatever receivers a timer happens to have.
        """
        # Measured on an eight-node fleet, scaled to what a byte-wide pipeline
        #  reports: (low_span, band_span).
        spans = [(566 / 8, 464 / 8), (396 / 8, 299 / 8), (260 / 8, 428 / 8),
                 (374 / 8, 412 / 8), (294 / 8, 464 / 8), (442 / 8, 332 / 8),
                 (431 / 8, 365 / 8), (452 / 8, 364 / 8)]
        _, _, cal = self.context()
        t_floor, t_low, t_high = cal._eq_destination(spans)
        scale = cal._eq_scale(0)
        widest = max(l for l, _ in spans) + max(b for _, b in spans)
        shrink = min(1.0, (scale * 0.5) / (widest * 1.01))
        # Targets are whole counts, so allow the half-count the rounding can
        #  take off the widest span.
        for lo_span, band_span in spans:
            self.assertGreaterEqual((t_high - t_low) / band_span,
                                    shrink - 0.5 / band_span)
            self.assertGreaterEqual((t_low - t_floor) / lo_span,
                                    shrink - 0.5 / lo_span)
        bands = sorted(b for _, b in spans)
        self.assertGreater((t_high - t_low) / bands[len(bands) // 2], 1.0)
        # a quad closer than the calibration spot must stay on scale
        self.assertLessEqual(t_high, scale * 0.5)

    def test_disabled_node_does_not_take_part(self):
        """A node with no frequency raises no step and gets no fit."""
        ctx, _, cal = self.context(count=2)
        ctx.race.profile.frequencies = json.dumps(
            {'b': ['R', 'R'], 'c': [1, 2], 'f': [5658, 0]})
        self.assertEqual(cal._eq_participants(), [0])
        self.assertIsNone(cal._eq_node_channels()[1])
        # only the enabled node's channel raises low/high steps
        labels = [chan for _, chan in cal._eq_steps() if chan]
        self.assertEqual(sorted(set(labels)), ['R1'])

    def test_every_channel_is_captured_high_before_any_is_captured_low(self):
        """Level is the outer loop, so the quad is placed twice per run.

        The levels come from where the quad physically is; the channel comes
        from a command. Sweeping the channels within a level means one move
        between "high" and "low" rather than one per channel.
        """
        _, _, cal = self.context(count=3)
        steps = cal._eq_steps()
        self.assertEqual(steps[0], ('noise', None))
        self.assertEqual(steps[1:], [('high', 'R1'), ('high', 'R2'), ('high', 'R3'),
                                     ('low', 'R1'), ('low', 'R2'), ('low', 'R3')])

    def test_reordering_the_steps_still_fits_the_same_constants(self):
        """The captures are keyed by level and channel, not by position."""
        values = [90, 150, 210]
        with patch('calibration.gevent.sleep'):
            ctx, _, cal = self.context()
            # filed in capture order: noise, then high, then low
            cal._eq_captured = {'noise': [values[0]],
                                'high:R1': [values[2]],
                                'low:R1': [values[1]]}
            cal._eq_note_capture_session()
            self.assertEqual(cal.eq_wizard_state()['state'], 'ready')
            self.assertTrue(cal.eq_wizard_apply())
            _, pivot, ou, su, ol, sl = ctx.interface.set_equalisation.call_args.args
            targets = cal._eq_destination([(values[1] - values[0],
                                            values[2] - values[1])])
            def corrected(raw):
                return ((raw - ou) * su if raw >= pivot else (raw - ol) * sl) >> 8
            for raw, target in zip(values, targets):
                self.assertLessEqual(abs(corrected(raw) - target), 2)

    def test_back_discards_the_step_most_recently_captured(self):
        """Back follows the new order, so it drops a low before a high."""
        _, _, cal = self.context(count=2)
        cal._eq_captured = {'noise': [1, 1], 'high:R1': [2, 2],
                            'high:R2': [3, 3], 'low:R1': [4, 4]}
        cal._eq_note_capture_session()
        self.assertTrue(cal.eq_wizard_back())
        self.assertNotIn('low:R1', cal._eq_captured)
        self.assertIn('high:R2', cal._eq_captured)
        self.assertEqual(cal.eq_wizard_state()['level'], 'low')
        self.assertEqual(cal.eq_wizard_state()['channel'], 'R1')

    def capture(self, cal, nodes, level, peaks):
        """Run one wizard capture with each node reading `peaks`."""
        for node, value in zip(nodes, peaks):
            if level == 'noise':
                node.node_nadir_rssi = value
            else:
                node.node_peak_rssi = value
        with patch('calibration.gevent.sleep'):
            return cal.eq_wizard_capture()

    def test_a_low_too_near_the_high_restarts_the_whole_low_pass(self):
        """One placement of the quad serves the pass, so all of it is suspect.

        The channels already captured at this level were measured from the
        same wrong distance, so keeping them would bury the error until Apply.
        """
        _, nodes, cal = self.context(count=2)
        self.assertTrue(self.capture(cal, nodes, 'noise', [90, 95]))
        self.assertTrue(self.capture(cal, nodes, 'high', [187, 176]))   # R1
        self.assertTrue(self.capture(cal, nodes, 'high', [140, 176]))   # R2
        self.assertTrue(self.capture(cal, nodes, 'low', [150, 97]))     # R1 ok
        self.assertIn('low:R1', cal._eq_captured)
        # R2's low sits 6 counts under its high: the quad never moved
        self.assertFalse(self.capture(cal, nodes, 'low', [94, 170]))
        # the good R1 low goes too - it shared the bad placement
        self.assertNotIn('low:R1', cal._eq_captured)
        self.assertNotIn('low:R2', cal._eq_captured)
        # highs and noise were measured elsewhere and survive
        self.assertIn('high:R1', cal._eq_captured)
        self.assertIn('high:R2', cal._eq_captured)
        self.assertIn('noise', cal._eq_captured)
        state = cal.eq_wizard_state()
        self.assertEqual((state['level'], state['channel']), ('low', 'R1'))

    def test_a_healthy_band_is_accepted(self):
        """The guard must not fire on a pass that is merely close to the limit."""
        _, nodes, cal = self.context(count=1)
        self.assertTrue(self.capture(cal, nodes, 'noise', [90]))
        self.assertTrue(self.capture(cal, nodes, 'high', [180]))
        self.assertEqual(cal._eq_min_gap(), 15)
        self.assertTrue(self.capture(cal, nodes, 'low', [165]))  # exactly 15
        self.assertIn('low:R1', cal._eq_captured)

    def test_only_the_node_on_that_channel_is_judged(self):
        """Off-channel nodes read bleed, so their tiny band means nothing."""
        _, nodes, cal = self.context(count=2)
        self.assertTrue(self.capture(cal, nodes, 'noise', [90, 95]))
        self.assertTrue(self.capture(cal, nodes, 'high', [187, 100]))  # R1
        self.assertTrue(self.capture(cal, nodes, 'high', [100, 176]))  # R2
        # on R1 only node 1 is judged; node 2 reads bleed and barely moves
        self.assertTrue(self.capture(cal, nodes, 'low', [150, 99]))
        self.assertIn('low:R1', cal._eq_captured)

    def test_an_edited_level_replaces_the_captured_one(self):
        """A single shadowed node is cheaper to correct than a whole pass."""
        _, nodes, cal = self.context(count=2)
        self.capture(cal, nodes, 'noise', [90, 95])
        self.capture(cal, nodes, 'high', [187, 176])   # R1
        self.capture(cal, nodes, 'high', [140, 176])   # R2
        self.assertTrue(cal.eq_wizard_set_level(1, 'high', 181))
        self.assertEqual(cal._eq_captured['high:R2'], [140, 181])
        # the edit survives a state query, as a capture does
        cal.eq_wizard_state()
        self.assertEqual(cal._eq_captured['high:R2'][1], 181)

    def test_captures_survive_apply_so_a_level_stays_editable(self):
        """Correcting one reading must not mean sweeping the fleet again."""
        values = [90, 150, 210]
        with patch('calibration.gevent.sleep'):
            ctx, _, cal = self.context()
            cal._eq_captured = {'noise': [values[0]], 'high:R1': [values[2]],
                                'low:R1': [values[1]]}
            cal._eq_note_capture_session()
            self.assertTrue(cal.eq_wizard_apply())

        # the fit is applied, and the readings behind it are still there
        state = cal.eq_wizard_state()
        self.assertEqual(state['state'], 'applied')
        rows = cal.eq_captured_table()
        self.assertEqual(rows[0]['mode'], 'applied-capture')
        self.assertEqual((rows[0]['low'], rows[0]['high']), (150, 210))

        # editing one re-arms Apply rather than demanding a new sweep
        self.assertTrue(cal.eq_wizard_set_level(0, 'high', 205))
        self.assertEqual(cal.eq_wizard_state()['state'], 'ready')
        self.assertEqual(cal._eq_captured['high:R1'], [205])
        with patch('calibration.gevent.sleep'):
            self.assertTrue(cal.eq_wizard_apply())
        self.assertEqual(cal.eq_wizard_state()['state'], 'applied')

    def test_a_new_capture_after_apply_starts_a_fresh_run(self):
        """Kept captures are a record, not a run still in progress."""
        values = [90, 150, 210]
        ctx, nodes, cal = self.context()
        with patch('calibration.gevent.sleep'):
            cal._eq_captured = {'noise': [values[0]], 'high:R1': [values[2]],
                                'low:R1': [values[1]]}
            cal._eq_note_capture_session()
            self.assertTrue(cal.eq_wizard_apply())
        self.assertEqual(cal.eq_wizard_state()['state'], 'applied')
        # Reset arms the wizard again and drops the kept set
        cal.eq_wizard_reset()
        self.assertEqual(cal._eq_captured, {})
        state = cal.eq_wizard_state()
        self.assertEqual((state['state'], state['level']), ('capturing', 'noise'))

    def test_noise_is_not_editable_and_junk_is_ignored(self):
        _, nodes, cal = self.context(count=1)
        self.capture(cal, nodes, 'noise', [90])
        self.capture(cal, nodes, 'high', [187])
        self.assertFalse(cal.eq_wizard_set_level(0, 'noise', 50))
        self.assertFalse(cal.eq_wizard_set_level(0, 'high', 'abc'))
        self.assertFalse(cal.eq_wizard_set_level(9, 'high', 100))
        self.assertFalse(cal.eq_wizard_set_level(0, 'low', 100))  # not captured
        self.assertEqual(cal._eq_captured['high:R1'], [187])
        self.assertEqual(cal._eq_captured['noise'], [90])

    def test_an_out_of_range_edit_is_refused(self):
        ctx, nodes, cal = self.context(count=1)
        self.capture(cal, nodes, 'noise', [90])
        self.capture(cal, nodes, 'high', [187])
        self.assertFalse(cal.eq_wizard_set_level(0, 'high', 999))
        self.assertFalse(cal.eq_wizard_set_level(0, 'high', -1))
        self.assertEqual(cal._eq_captured['high:R1'], [187])
        self.assertTrue(ctx.rhui.emit_priority_message.called)

    def test_unconfirmed_write_is_not_recorded(self):
        """A coefficient write the node never acknowledged is not success."""
        import RHInterface
        _, nodes, _ = self.context()
        interface = RHInterface.RHInterface.__new__(RHInterface.RHInterface)
        interface.nodes = nodes
        interface.log = lambda *a, **k: None
        nodes[0].eq_pivot = 0
        with patch.object(RHInterface.RHInterface, 'set_and_validate_value_16',
                          return_value=120), \
             patch.object(RHInterface.RHInterface, 'get_value_16', return_value=None):
            ok = RHInterface.RHInterface.set_equalisation(
                interface, 0, 120, 89, 256, 89, 256)
        self.assertFalse(ok)
        self.assertEqual(nodes[0].eq_pivot, 0)

    def test_capture_is_discarded_when_state_changes(self):
        """A reset landing during the settle invalidates the reading."""
        ctx, _, cal = self.context()
        before = cal._eq_session()
        cal._eq_invalidate_session()
        self.assertNotEqual(cal._eq_session(), before)

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
            cal._eq_captured = dict(zip(('noise', 'low:R1', 'high:R1'),
                                        ([v] for v in (90, 150, 210))))
            cal._eq_note_capture_session()
            self.assertTrue(cal.eq_wizard_apply())
            self.assertEqual(json.loads(ctx.race.profile.enter_ats)['v'], [169])
            self.assertEqual(json.loads(ctx.race.profile.exit_ats)['v'], [160])

    def test_apply_refuses_when_a_node_does_not_confirm(self):
        """An unconfirmed write leaves the axis unknown, not merely changed."""
        with patch('calibration.gevent.sleep'):
            ctx, _, cal = self.context()
            ctx.interface.set_equalisation.return_value = False
            cal._eq_captured = dict(zip(('noise', 'low:R1', 'high:R1'),
                                        ([v] for v in (90, 150, 210))))
            cal._eq_note_capture_session()
            self.assertFalse(cal.eq_wizard_apply())

    def test_captures_do_not_survive_a_profile_change(self):
        """A complete capture set belongs to the configuration that made it."""
        ctx, _, cal = self.context()
        cal._eq_captured = {'noise': [90], 'low:R1': [150], 'high:R1': [210]}
        cal._eq_note_capture_session()
        self.assertTrue(cal._eq_captures_are_current())
        ctx.race.profile.id = 2
        self.assertFalse(cal._eq_captures_are_current())

    def test_reset_refuses_when_a_node_does_not_confirm(self):
        """An unconfirmed reset leaves the correction unknown."""
        ctx, _, cal = self.context()
        ctx.race.profile.enter_ats = json.dumps({'v': [80], 'eq': [[150, 89, 256, 89, 256]]})
        ctx.interface.set_equalisation.return_value = False
        self.assertFalse(cal.eq_wizard_reset())
        # thresholds must not move as though correction were off
        self.assertEqual(json.loads(ctx.race.profile.enter_ats)['v'], [80])
        self.assertTrue(cal.eq_state_is_unresolved())

    def test_failed_apply_blocks_racing(self):
        """Unresolved hardware state has to stop a race starting."""
        with patch('calibration.gevent.sleep'):
            ctx, _, cal = self.context()
            ctx.interface.set_equalisation.return_value = False
            cal._eq_captured = dict(zip(('noise', 'low:R1', 'high:R1'),
                                        ([v] for v in (90, 150, 210))))
            cal._eq_note_capture_session()
            self.assertFalse(cal.eq_wizard_apply())
            self.assertTrue(cal.eq_state_is_unresolved())
            self.assertEqual(cal.eq_unresolved_nodes(), [1])

    def test_apply_writes_no_thresholds_at_all(self):
        """Applying a fit touches the node constants and nothing else."""
        with patch('calibration.gevent.sleep'):
            ctx, _, cal = self.context()
            ctx.race.profile.enter_ats = json.dumps({'v': [169]})
            ctx.race.profile.exit_ats = json.dumps({'v': [160]})
            cal._eq_captured = dict(zip(('noise', 'low:R1', 'high:R1'),
                                        ([v] for v in (90, 150, 210))))
            cal._eq_note_capture_session()
            self.assertTrue(cal.eq_wizard_apply())
        ctx.interface.set_enter_at_level.assert_not_called()
        ctx.interface.set_exit_at_level.assert_not_called()

    def test_back_keeps_the_earlier_captures(self):
        """Stepping back cancels a pending capture, not the retained ones."""
        ctx, _, cal = self.context()
        cal._eq_captured = {'noise': [90], 'low:R1': [150], 'high:R1': [210]}
        cal._eq_note_capture_session()
        self.assertTrue(cal.eq_wizard_back())
        self.assertTrue(cal._eq_captures_are_current())
        # the state query must not discard what Back kept
        cal.eq_wizard_state()
        # low is the last step now, so high is what survives
        self.assertEqual(sorted(cal._eq_captured), ['high:R1', 'noise'])

    def test_history_ignores_races_under_another_correction(self):
        """Adaptive calibration must not restore thresholds from another axis."""
        ctx, _, cal = self.context()
        ctx.race.profile.eq_pivots = json.dumps({'v': [150]})
        ctx.race.profile.eq_offset_ups = json.dumps({'v': [89]})
        ctx.race.profile.eq_slope_ups = json.dumps({'v': [256]})
        ctx.race.profile.eq_offset_los = json.dumps({'v': [89]})
        ctx.race.profile.eq_slope_los = json.dumps({'v': [256]})
        live = cal._eq_signature()
        race = object()
        values = {'eq_signature': json.dumps(live)}
        ctx.rhdata.get_savedrace_attribute_value.side_effect = \
            lambda r, name, default=None: values.get(name, default)
        self.assertTrue(cal._race_matches_correction(race))
        values['eq_signature'] = json.dumps([[151, 89, 256, 89, 256]])
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
            captures = dict(zip(('noise', 'low:R1', 'high:R1'),
                                ([v] for v in (90, 150, 210))))

            ctx.interface.set_equalisation.return_value = False
            cal._eq_captured = dict(captures)
            cal._eq_note_capture_session()
            self.assertFalse(cal.eq_wizard_apply())
            self.assertEqual(json.loads(ctx.race.profile.enter_ats)['v'], [169])

            ctx.interface.set_equalisation.return_value = True
            cal._eq_captured = dict(captures)
            cal._eq_note_capture_session()
            self.assertTrue(cal.eq_wizard_apply())
            self.assertEqual(json.loads(ctx.race.profile.enter_ats)['v'], [169])
            self.assertFalse(cal.eq_state_is_unresolved())

    def test_reset_leaves_the_thresholds_alone(self):
        """Clearing the correction must not rewrite them either."""
        ctx, _, cal = self.context()
        ctx.race.profile.enter_ats = json.dumps({'v': [80]})
        ctx.race.profile.exit_ats = json.dumps({'v': [75]})
        ctx.race.profile.eq_pivots = json.dumps({'v': [150]})

        ctx.interface.set_equalisation.return_value = False
        self.assertFalse(cal.eq_wizard_reset())
        self.assertEqual(json.loads(ctx.race.profile.enter_ats)['v'], [80])

        ctx.interface.set_equalisation.return_value = True
        self.assertTrue(cal.eq_wizard_reset())
        self.assertEqual(json.loads(ctx.race.profile.enter_ats)['v'], [80])
        self.assertEqual(json.loads(ctx.race.profile.exit_ats)['v'], [75])
        self.assertFalse(cal.eq_state_is_unresolved())

    def test_reset_holds_the_guard_through_its_tracking_reset(self):
        """The final tracking reset is a hardware mutation too."""
        seen = []
        ctx, _, cal = self.context()
        ctx.interface.reset_node_extremums.side_effect = \
            lambda *a, **k: seen.append(cal._eq_busy)
        self.assertTrue(cal.eq_wizard_reset())
        self.assertTrue(seen, 'tracking was never reset')
        self.assertTrue(all(seen), 'guard released before the tracking reset')

    def test_capture_rejects_noise_only_signal(self):
        ctx, _, cal = self.context()
        cal._eq_captured = {'noise': [700], 'low:R1': [702], 'high:R1': [704]}
        self.assertFalse(cal.eq_wizard_apply())
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
