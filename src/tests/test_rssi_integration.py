"""Regression tests for normalisation and runtime ADC resolution."""
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

class RssiIntegrationTest(unittest.TestCase):
    def context(self, full=True):
        node = Node()
        node.api_level = 38
        node.firmware_proctype_str = 'STM32F4'
        node.adc_resolution = 12 if full else 10
        node.init()
        profile = SimpleNamespace(id=1, frequencies=json.dumps({'b': ['R'], 'c': [1], 'f': [5658]}))
        ctx = SimpleNamespace(race=SimpleNamespace(profile=profile, num_nodes=1),
                              interface=Mock(nodes=[node]), rhui=Mock(), rhdata=Mock(),
                              events=Mock())
        def save(data):
            for key, value in data.items():
                if key != 'profile_id':
                    setattr(profile, key, json.dumps(value))
            return profile
        ctx.rhdata.alter_profile.side_effect = save
        ctx.rhdata.get_profile.return_value = profile
        # The hardware accepts writes unless a test says otherwise; apply now
        #  refuses to record a fit the nodes did not confirm.
        ctx.interface.set_normalisation.return_value = True
        return ctx, node, Calibration(ctx)

    def test_fit_and_stale_resolution_in_both_adc_modes(self):
        for full, floor, gate in ((True, 700, 1700), (False, 88, 213)):
            with self.subTest(full=full), patch('calibration.gevent.sleep'):
                ctx, node, cal = self.context(full)
                cal._norm_scope_sel = 'current'
                cal._norm_saved_freqs = cal._norm_profile_freqs()
                cal._norm_channels = cal._norm_sweep_channels()
                cal._norm_captured = {'noise:R1': [floor], 'high:R1': [gate]}
                cal._norm_note_capture_session()
                self.assertTrue(cal.norm_wizard_apply())
                _, pivot, offset, scale = ctx.interface.set_normalisation.call_args.args
                self.assertLessEqual(abs(cal._corrected(gate, [pivot, offset, scale]) - gate), 2)
                self.assertEqual(cal.norm_wizard_state()['state'], 'applied')
                node.adc_resolution = 10 if full else 12
                self.assertEqual(cal.norm_wizard_state()['state'], 'incompatible')
                self.assertIsNone(cal._norm_signature())

    def test_threshold_conversion_follows_normalisation_and_width(self):
        ctx, node, cal = self.context(full=False)
        ctx.race.profile.norm_pivots = json.dumps({'v': [150], 'adc_bits': 10})
        ctx.race.profile.norm_offsets = json.dumps({'v': [89]})
        ctx.race.profile.norm_scales = json.dumps({'v': [256]})
        ctx.race.profile.enter_ats = json.dumps({'v': [80]})
        ctx.race.profile.exit_ats = json.dumps({'v': [70]})
        old_axis = cal.threshold_scale_id()
        raw_before = cal._uncorrect(80, old_axis['norm'][0])
        node.adc_resolution = 12
        cal._norm_store([0], [0], [256], {})
        enter, _ = cal.convert_thresholds_to_scale(from_axis=old_axis)
        self.assertEqual(enter, [raw_before * 8])
        self.assertNotEqual(enter, [80 * 8])
        self.assertEqual(cal.convert_thresholds_to_scale(), (None, None))

    def test_race_save_records_width_and_correction(self):
        source = (SRC / 'server/RHRace.py').read_text()
        self.assertIn("'adc_bits'", source)
        self.assertIn("'norm_signature'", source)
        self.assertIn('_norm_signature(', source)

    def test_history_rejects_another_width_or_fit(self):
        ctx, node, cal = self.context(full=False)
        ctx.race.profile.norm_pivots = json.dumps({'v': [150], 'adc_bits': 10})
        ctx.race.profile.norm_offsets = json.dumps({'v': [89]})
        ctx.race.profile.norm_scales = json.dumps({'v': [256]})
        live = cal._norm_signature()
        race = object()
        values = {'adc_bits': '10', 'norm_signature': json.dumps(live)}
        ctx.rhdata.get_savedrace_attribute_value.side_effect = \
            lambda r, name, default=None: values.get(name, default)
        self.assertTrue(cal._race_matches_correction(race))
        values['norm_signature'] = json.dumps([[151, 89, 256]])
        self.assertFalse(cal._race_matches_correction(race))
        values['norm_signature'] = json.dumps(live)
        node.adc_resolution = 12
        self.assertFalse(cal._race_matches_correction(race))

    def test_busy_covers_threshold_writes(self):
        seen = []
        with patch('calibration.gevent.sleep'):
            ctx, _, cal = self.context(full=False)
            ctx.race.profile.enter_ats = json.dumps({'v': [169]})
            ctx.race.profile.exit_ats = json.dumps({'v': [160]})
            ctx.interface.set_enter_at_level.side_effect = \
                lambda *a, **k: seen.append(cal._norm_busy)
            cal._norm_scope_sel = 'current'
            cal._norm_saved_freqs = cal._norm_profile_freqs()
            cal._norm_channels = cal._norm_sweep_channels()
            cal._norm_captured = {'noise:R1': [90], 'high:R1': [210]}
            cal._norm_note_capture_session()
            self.assertTrue(cal.norm_wizard_apply())
        self.assertTrue(seen)
        self.assertTrue(all(seen))

    def test_unconfirmed_adc_write_is_not_recorded(self):
        """A write the node never acknowledged must not change cached state."""
        _, node, _ = self.context(full=False)
        interface = RHInterface.RHInterface.__new__(RHInterface.RHInterface)
        interface.nodes = [node]
        interface.log = lambda *a, **k: None
        node.adc_resolution = 10
        with patch.object(RHInterface.RHInterface, 'set_and_validate_value_8',
                          return_value=12), \
             patch.object(RHInterface.RHInterface, 'get_value_8', return_value=None):
            ok = RHInterface.RHInterface.set_adc_resolution(interface, 0, True)
        self.assertFalse(ok)
        self.assertEqual(node.adc_resolution, 10)

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
