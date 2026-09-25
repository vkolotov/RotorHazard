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
        node.api_level = 38
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
                targets = cal._eq_destination([(values[1]-values[0], values[2]-values[1])])
                def corrected(raw):
                    return ((raw-ou)*su if raw >= pivot else (raw-ol)*sl) >> 8
                for raw, target in zip(values, targets):
                    self.assertLessEqual(abs(corrected(raw)-target), 2)
                self.assertEqual(cal.eq_wizard_state()['state'], 'applied')
                node.adc_resolution = 10 if full else 12
                self.assertEqual(cal.eq_wizard_state()['state'], 'incompatible')
                cal.hardware_set_all_equalisation()
                self.assertEqual(ctx.interface.set_equalisation.call_args.args[1], 0)

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
            return {serial_node.READ_REVISION_CODE: [0x25, 38],
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

if __name__ == '__main__':
    unittest.main()
