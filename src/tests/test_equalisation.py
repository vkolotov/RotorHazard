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

    def test_apply_moves_thresholds_onto_the_new_axis(self):
        """A threshold is a corrected value, so applying a fit must move it."""
        with patch('calibration.gevent.sleep'):
            ctx, _, cal = self.context()
            ctx.race.profile.enter_ats = json.dumps({'v': [169]})
            ctx.race.profile.exit_ats = json.dumps({'v': [160]})
            cal._eq_captured = dict(zip(('noise', 'low:R1', 'high:R1'),
                                        ([v] for v in (90, 150, 210))))
            cal._eq_note_capture_session()
            self.assertTrue(cal.eq_wizard_apply())
            stored = json.loads(ctx.race.profile.enter_ats)
            # the raw level 169 is unchanged; its corrected value is not 169
            self.assertIsNotNone(stored['eq'])
            self.assertNotEqual(stored['v'], [169])
            self.assertEqual(cal._uncorrect(stored['v'][0], stored['eq'][0]), 169)

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

    def test_busy_covers_threshold_writes(self):
        """The guard must still be set while thresholds are written."""
        seen = []
        with patch('calibration.gevent.sleep'):
            ctx, _, cal = self.context()
            ctx.race.profile.enter_ats = json.dumps({'v': [169]})
            ctx.race.profile.exit_ats = json.dumps({'v': [160]})
            ctx.interface.set_enter_at_level.side_effect = \
                lambda *a, **k: seen.append(cal._eq_busy)
            cal._eq_captured = dict(zip(('noise', 'low:R1', 'high:R1'),
                                        ([v] for v in (90, 150, 210))))
            cal._eq_note_capture_session()
            self.assertTrue(cal.eq_wizard_apply())
        self.assertTrue(seen, 'thresholds were never written')
        self.assertTrue(all(seen), 'guard was released before threshold writes')

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

    def test_retry_after_failed_apply_converts_from_the_real_axis(self):
        """A failed attempt must not corrupt the source axis for the retry.

        The failure persists the desired coefficients while leaving the
        thresholds alone, so the axis has to be read from the record that
        travels with the thresholds, not from the coefficients.
        """
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

            # retry, this time the nodes take it
            ctx.interface.set_equalisation.return_value = True
            cal._eq_captured = dict(captures)
            cal._eq_note_capture_session()
            self.assertTrue(cal.eq_wizard_apply())
            stored = json.loads(ctx.race.profile.enter_ats)
            self.assertEqual(cal._uncorrect(stored['v'][0], stored['eq'][0]), 169)
            self.assertNotEqual(stored['v'], [169])
            self.assertFalse(cal.eq_state_is_unresolved())

    def test_retry_after_failed_reset_converts_from_the_real_axis(self):
        """Same for the reverse path: a failed reset then a good one."""
        ctx, _, cal = self.context()
        corrected = {'v': [80], 'eq': [[150, 89, 256, 89, 256]]}
        ctx.race.profile.enter_ats = json.dumps(corrected)
        ctx.race.profile.exit_ats = json.dumps(corrected)
        ctx.race.profile.eq_pivots = json.dumps({'v': [150]})
        ctx.race.profile.eq_offset_ups = json.dumps({'v': [89]})
        ctx.race.profile.eq_slope_ups = json.dumps({'v': [256]})
        ctx.race.profile.eq_offset_los = json.dumps({'v': [89]})
        ctx.race.profile.eq_slope_los = json.dumps({'v': [256]})

        ctx.interface.set_equalisation.return_value = False
        self.assertFalse(cal.eq_wizard_reset())
        self.assertEqual(json.loads(ctx.race.profile.enter_ats)['v'], [80])

        ctx.interface.set_equalisation.return_value = True
        self.assertTrue(cal.eq_wizard_reset())
        # correction is gone, so the threshold returns to the raw level
        self.assertEqual(json.loads(ctx.race.profile.enter_ats)['v'], [169])
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
