"""Regression tests for combined equalisation and runtime ADC resolution."""
import gevent.event
import gevent.lock
import importlib.util
import json
from pathlib import Path
import sqlite3
import shutil
import subprocess
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
import RHInterface
import serial_node

spec = importlib.util.spec_from_file_location('eq_migration', SRC / 'server/util/add_equalisation_columns.py')
migration = importlib.util.module_from_spec(spec)
spec.loader.exec_module(migration)


class RssiIntegrationTest(unittest.TestCase):
    def context(self, full=True):
        node = Node()
        node.api_level = 37
        node.firmware_proctype_str = 'STM32F4'
        node.adc_resolution = 12 if full else 10
        node.init()
        profile = SimpleNamespace(id=1, frequencies=json.dumps({'b': ['R'], 'c': [1]}))
        ctx = SimpleNamespace(race=SimpleNamespace(profile=profile, num_nodes=1),
                              interface=Mock(nodes=[node]), rhui=Mock(), rhdata=Mock())
        def save(data):
            for key, value in data.items():
                if key != 'profile_id':
                    setattr(profile, key, json.dumps(value))
            return profile
        ctx.rhdata.alter_profile.side_effect = save
        return ctx, node, Calibration(ctx)

    def test_fit_anchors_in_both_adc_modes(self):
        for full, values in ((True, [700, 1200, 1700]), (False, [88, 150, 213])):
            with self.subTest(full=full), patch('calibration.gevent.sleep'):
                ctx, node, cal = self.context(full)
                cal._eq_captured = dict(zip(('noise', 'low:R1', 'high:R1'), ([v] for v in values)))
                self.assertTrue(cal.eq_wizard_apply())
                _, pivot, ou, su, ol, sl = ctx.interface.set_equalisation.call_args.args
                targets = cal._eq_targets(0)
                def corrected(raw):
                    return ((raw-ou)*su if raw >= pivot else (raw-ol)*sl) >> 8
                for raw, target in zip(values, targets):
                    self.assertLessEqual(abs(corrected(raw)-target), 2)
                self.assertEqual(cal.eq_wizard_state()['state'], 'applied')
                node.adc_resolution = 10 if full else 12
                self.assertEqual(cal.eq_wizard_state()['state'], 'incompatible')
                cal.hardware_set_all_equalisation()
                self.assertEqual(ctx.interface.set_equalisation.call_args.args[1], 0)

    def test_decision_band_is_not_compressed_in_either_mode(self):
        """The pass/miss band must not shrink through the correction.

        The upper segment spans the levels lap detection decides between, so a
        slope below 1.0 there throws away resolution the ADC did supply. Equal
        fractions of a 255-count and a 4095-count range look alike but are not:
        the input span shrinks by eight as well, so the narrow pipeline needs
        proportionally wider fractions to hold the same slope.
        """
        # Measured on an eight-node fleet, 12-bit counts: floor -> low -> high.
        floor_span, band_span = 427.0, 350.0
        for full in (True, False):
            with self.subTest(full=full):
                _, _, cal = self.context(full)
                t_floor, t_low, t_high = cal._eq_targets(0)
                div = 1.0 if full else 8.0
                upper = (t_high - t_low) / (band_span / div)
                lower = (t_low - t_floor) / (floor_span / div)
                self.assertGreater(
                    upper, 1.0,
                    'decision band compressed at {} bits'.format(12 if full else 8))
                self.assertGreater(lower, 0.0)
                # a quad closer than the calibration spot must still fit
                scale = 4095 if full else 255
                self.assertLess(t_high * 2, scale)

    def test_capture_rejects_noise_only_signal(self):
        ctx, _, cal = self.context()
        cal._eq_captured = {'noise': [700], 'low:R1': [702], 'high:R1': [704]}
        self.assertFalse(cal.eq_wizard_apply())
        ctx.rhdata.alter_profile.assert_not_called()

    def test_rssi_transport_stays_wide_in_legacy_sampling_mode(self):
        _, node, _ = self.context(full=False)
        self.assertTrue(node.has_wide_rssi())
        self.assertEqual(RHInterface.unpack_rssi(node, [0x0F, 0xFF]), 4095)
        node.firmware_proctype_str = 'ATmega328P'
        node.init()
        self.assertFalse(node.has_wide_rssi())
        self.assertEqual(node.max_rssi_value, 255)
        self.assertEqual(RHInterface.unpack_rssi(node, [123]), 123)

    def test_multinode_discovery_propagates_processor_type(self):
        config = Mock()
        config.get_item.return_value = ['/dev/ttyAMA0']
        def read(node, interface, command, *args):
            return {serial_node.READ_REVISION_CODE: [0x25, 37],
                    serial_node.READ_MULTINODE_COUNT: [8]}.get(command)
        def firmware(node):
            node.firmware_version_str = '1.2.0'
        def processor(node):
            node.firmware_proctype_str = 'STM32F4'
        with patch.object(serial_node.serial, 'Serial'), patch.object(serial_node.gevent, 'sleep'), \
             patch.object(serial_node.SerialNode, 'read_block', read), \
             patch.object(serial_node.SerialNode, 'read_firmware_version', firmware), \
             patch.object(serial_node.SerialNode, 'read_firmware_proctype', processor), \
             patch.object(serial_node.SerialNode, 'read_firmware_timestamp'), \
             patch.object(serial_node.SerialNode, 'read_node_slot_index'):
            nodes = serial_node.discover(0, config, isS32BPillFlag=True)
        self.assertEqual(len(nodes), 8)
        for node in nodes:
            node.init()
            self.assertTrue(node.has_wide_rssi())
            self.assertEqual(node.max_rssi_value, 65535)

    def test_combined_opcodes_match_and_are_unique(self):
        import re
        header = (SRC / 'node/commands.h').read_text()
        names = re.findall(r'^#define ((?:READ_|WRITE_|RESET_NODE_)\w+) (0x[0-9A-F]+)', header, re.M)
        values = [int(value, 16) for _, value in names]
        self.assertEqual(len(values), len(set(values)))
        for name, value in names:
            if hasattr(RHInterface, name):
                self.assertEqual(getattr(RHInterface, name), int(value, 16), name)

    @unittest.skipUnless(shutil.which('g++'), 'host C++ compiler required')
    def test_firmware_threshold_frames_match_rssi_width(self):
        # Compile the actual framing function, buffer and RSSI types for both
        # targets. Arduino pin definitions are irrelevant to this wire test.
        code = (SRC / 'node/commands.cpp').read_text()
        function = 'byte Message::getPayloadSize()' + code.split(
            'byte Message::getPayloadSize()', 1)[1].split('\n}\n', 1)[0] + '\n}\n'
        harness = r'''#include <cassert>
#include <cstdint>
using byte = uint8_t;
#include "util/rhtypes.h"
#define config_h
class RssiNode;
#include "commands.h"
''' + function + r'''
int main() {
    Message message;
    for (int index = 0; index < 2; ++index) {
        message.command = index == 0 ? WRITE_ENTER_AT_LEVEL : WRITE_EXIT_AT_LEVEL;
        const rssi_t threshold = sizeof(rssi_t) == 2 ? (index == 0 ? 540 : 490)
                                                   : (index == 0 ? 96 : 80);
        Buffer frame;
        ioBufferWriteRssi(frame, threshold);
        assert(message.getPayloadSize() == frame.size);
        frame.writeChecksum();
        assert(frame.size == message.getPayloadSize() + 1);
        assert(frame.data[frame.size - 1] == frame.calculateChecksum(frame.size - 1));
        frame.flipForRead();
        assert(ioBufferReadRssi(frame) == threshold);
    }
}
'''
        with tempfile.TemporaryDirectory() as temp:
            source = Path(temp) / 'threshold.cpp'
            source.write_text(harness)
            for mode in ('avr', 'stm32'):
                with self.subTest(mode=mode):
                    binary = str(Path(temp) / mode)
                    flags = ['-DSTM32_CORE_VERSION=1'] if mode == 'stm32' else []
                    subprocess.run(['g++', '-std=c++11', '-I', str(SRC / 'node'),
                                    *flags, str(source), '-o', binary], check=True,
                                   capture_output=True)
                    subprocess.run([binary], check=True, capture_output=True)

    def test_existing_api37_calibration_migrates_once(self):
        with tempfile.TemporaryDirectory() as temp:
            db = str(Path(temp) / 'database.db')
            c = sqlite3.connect(db)
            c.execute('CREATE TABLE profiles (id INTEGER PRIMARY KEY, eq_pivots TEXT, eq_kups TEXT, eq_klos TEXT, enter_ats TEXT)')
            encode = lambda vals: json.dumps({'v': vals})
            c.execute('INSERT INTO profiles VALUES (1, ?, ?, ?, ?)',
                      [encode(v) for v in ([1222, 1416], [435, 601], [138, 134], [540, 540])])
            c.commit()
            self.assertEqual(migration.main(db), 0)
            first = c.execute('SELECT * FROM profiles').fetchone()
            self.assertEqual(migration.main(db), 0)
            self.assertEqual(c.execute('SELECT * FROM profiles').fetchone(), first)
            row = dict(zip([d[0] for d in c.execute('SELECT * FROM profiles').description], first))
            pivots = json.loads(row['eq_pivots'])['v']
            for i, pivot in enumerate(pivots):
                for raw in (pivot-300, pivot, pivot+200):
                    above = raw >= pivot
                    slope = json.loads(row['eq_slope_ups' if above else 'eq_slope_los'])['v'][i]
                    offset = json.loads(row['eq_offset_ups' if above else 'eq_offset_los'])['v'][i]
                    old = 300+(((raw-pivot)*slope)>>8) if above else 300-(((pivot-raw)*slope)>>8)
                    self.assertLessEqual(abs(((raw-offset)*slope >> 8)-old), 2)
            self.assertEqual(json.loads(row['enter_ats'])['v'], [540, 540])
            c.close()


if __name__ == '__main__':
    unittest.main()
