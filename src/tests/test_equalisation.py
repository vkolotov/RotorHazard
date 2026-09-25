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
                              interface=Mock(nodes=nodes), rhui=Mock(), rhdata=Mock())
        def save(data):
            for key, value in data.items():
                if key != 'profile_id':
                    setattr(profile, key, json.dumps(value))
            return profile
        ctx.rhdata.alter_profile.side_effect = save
        return ctx, nodes, Calibration(ctx)

    def test_fit_passes_through_the_captured_levels(self):
        values = [90, 150, 210]
        with patch('calibration.gevent.sleep'):
            ctx, _, cal = self.context()
            cal._eq_captured = dict(zip(('noise', 'low:R1', 'high:R1'),
                                        ([v] for v in values)))
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
