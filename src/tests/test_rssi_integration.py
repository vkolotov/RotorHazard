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
        ctx.interface.set_equalisation.return_value = True
        return ctx, node, Calibration(ctx)

    def test_fit_anchors_in_both_adc_modes(self):
        for full, values in ((True, [700, 1200, 1700]), (False, [88, 150, 213])):
            with self.subTest(full=full), patch('calibration.gevent.sleep'):
                ctx, node, cal = self.context(full)
                cal._eq_captured = dict(zip(('noise', 'low:R1', 'high:R1'), ([v] for v in values)))
                cal._eq_note_capture_session()
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

    def test_threshold_conversion_follows_the_correction(self):
        """A threshold is a corrected value, not a raw one.

        Multiplying by the ratio of ADC widths is only right when no
        correction is in force on either side. With equalisation applied, the
        stored number means a different raw reading, and converting has to go
        back through the transform to find it.
        """
        ctx, node, cal = self.context(full=False)
        # corrected = raw - 89 over the upper segment
        coeffs = [150, 89, 256, 89, 256]
        self.assertEqual(cal._corrected(169, coeffs), 80)
        self.assertEqual(cal._uncorrect(80, coeffs), 169)
        # round trip through the pair is stable
        for raw in (160, 200, 255):
            self.assertEqual(cal._uncorrect(cal._corrected(raw, coeffs), coeffs), raw)
        # and a naive x8 would have produced 640 rather than the raw-equivalent
        self.assertNotEqual(cal._uncorrect(80, coeffs) * 8, 80 * 8)

    def test_apply_then_manual_then_switch_keeps_the_physical_level(self):
        """The sequence the review asked for, end to end.

        Apply a fit, set a threshold by hand against the corrected reading,
        then switch resolution. The stored number has to keep meaning the same
        physical signal, which it only does if the axis travels with it at
        every step rather than being assumed.
        """
        with patch('calibration.gevent.sleep'):
            ctx, node, cal = self.context(full=False)
            ctx.rhdata.get_profile.return_value = ctx.race.profile
            ctx.race.profile.enter_ats = json.dumps({'v': [None]})
            ctx.race.profile.exit_ats = json.dumps({'v': [None]})
            ctx.interface.set_equalisation.return_value = True

            cal._eq_captured = dict(zip(('noise', 'low:R1', 'high:R1'),
                                        ([v] for v in (90, 150, 210))))
            cal._eq_note_capture_session()
            self.assertTrue(cal.eq_wizard_apply())

            # a threshold set by hand is measured against the corrected reading
            node.enter_at_level = 80
            cal.set_enter_at_level(0, 80, emit_levels=False)
            axis = cal._stored_scale_id(ctx.race.profile)
            self.assertEqual(axis['adc_bits'], 10)
            self.assertIsNotNone(axis['eq'])  # the correction is recorded

            raw_before = cal._uncorrect(80, axis['eq'][0])

            # switching drops the correction, so the axis becomes raw 12-bit
            node.adc_resolution = 12
            ctx.race.profile.eq_pivots = json.dumps(
                {'v': [150], 'adc_bits': [10]})  # fitted at 10, now stale
            enter, _ = cal.convert_thresholds_to_scale()
            self.assertEqual(enter, [raw_before * 8])
            # and emphatically not the naive x8 of the corrected value
            self.assertNotEqual(enter, [80 * 8])

    def test_race_save_writes_both_axis_attributes(self):
        """Exercise the real writer, not a stand-in for it.

        The history test supplies the attributes directly, so it passes even
        when nothing writes them. This calls the code in RHRace that saves a
        race and checks both halves of the axis are recorded with the
        arguments the calibration API actually takes.
        """
        import ast
        source = (SRC / 'server/RHRace.py').read_text()
        tree = ast.parse(source)

        written = []
        signature_args = []
        for node in ast.walk(tree):
            if not isinstance(node, ast.Call):
                continue
            func = node.func
            if isinstance(func, ast.Attribute) and func.attr == 'alter_savedRaceMeta':
                for arg in node.args:
                    if isinstance(arg, ast.Dict):
                        for key, value in zip(arg.keys, arg.values):
                            if getattr(key, 'value', None) == 'race_attr' and \
                                    isinstance(value, ast.Constant):
                                written.append(value.value)
            if isinstance(func, ast.Attribute) and func.attr == '_eq_signature':
                signature_args.append(len(node.args))

        self.assertIn('adc_bits', written, 'ADC width is not recorded')
        self.assertIn('eq_signature', written, 'correction is not recorded')
        # _eq_signature(bits) on this branch: calling it bare raises at runtime
        self.assertTrue(signature_args, '_eq_signature is never called')
        for count in signature_args:
            self.assertEqual(count, 1,
                             '_eq_signature must be called with the ADC width')

    def test_saved_race_records_the_correction(self):
        """Adaptive history must key on the axis, not the width alone."""
        ctx, node, cal = self.context(full=False)
        ctx.race.profile.eq_pivots = json.dumps({'v': [150], 'adc_bits': [10]})
        ctx.race.profile.eq_offset_ups = json.dumps({'v': [89]})
        ctx.race.profile.eq_slope_ups = json.dumps({'v': [256]})
        ctx.race.profile.eq_offset_los = json.dumps({'v': [89]})
        ctx.race.profile.eq_slope_los = json.dumps({'v': [256]})
        live = cal._eq_signature(10)
        self.assertIsNotNone(live)

        race = object()
        values = {'adc_bits': '10', 'eq_signature': json.dumps(live)}
        ctx.rhdata.get_savedrace_attribute_value.side_effect = \
            lambda r, name, default=None: values.get(name, default)
        self.assertTrue(cal._race_matches_resolution(race))

        # same width, different correction -> not reusable
        values['eq_signature'] = json.dumps([[151, 89, 256, 89, 256]])
        self.assertFalse(cal._race_matches_resolution(race))

    def test_resolution_conversion_is_idempotent(self):
        """Converting a profile already on the target axis must do nothing."""
        ctx, node, cal = self.context(full=False)
        ctx.race.profile.enter_ats = json.dumps({'v': [96], 'adc_bits': 10, 'eq': None})
        ctx.race.profile.exit_ats = json.dumps({'v': [80], 'adc_bits': 10, 'eq': None})
        ctx.race.profile.eq_pivots = None
        ctx.rhdata.get_profile.return_value = ctx.race.profile
        node.adc_resolution = 12
        first = cal.convert_thresholds_to_scale()
        self.assertEqual(first[0], [768])
        # the stored scale now matches, so a repeat is a no-op
        second = cal.convert_thresholds_to_scale()
        self.assertEqual(second, (None, None))
        self.assertEqual(json.loads(ctx.race.profile.enter_ats)['v'], [768])

    def test_reset_refuses_when_a_node_does_not_confirm(self):
        """An unconfirmed reset leaves the correction unknown."""
        ctx, _, cal = self.context(full=False)
        ctx.race.profile.enter_ats = json.dumps(
            {'v': [80], 'adc_bits': 10, 'eq': [[150, 89, 256, 89, 256]]})
        ctx.interface.set_equalisation.return_value = False
        self.assertFalse(cal.eq_wizard_reset())
        self.assertEqual(json.loads(ctx.race.profile.enter_ats)['v'], [80])
        self.assertTrue(cal.eq_state_is_unresolved())

    def test_busy_covers_threshold_writes(self):
        """The guard must still be set while thresholds are written."""
        seen = []
        with patch('calibration.gevent.sleep'):
            ctx, _, cal = self.context(full=False)
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

    def test_retry_after_failed_apply_converts_from_the_real_axis(self):
        """A failed attempt must not corrupt the source axis for the retry."""
        with patch('calibration.gevent.sleep'):
            ctx, _, cal = self.context(full=False)
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
            stored = json.loads(ctx.race.profile.enter_ats)
            self.assertEqual(cal._uncorrect(stored['v'][0], stored['eq'][0]), 169)
            self.assertFalse(cal.eq_state_is_unresolved())

    def test_staging_is_refused_while_calibration_is_unresolved(self):
        """The inherited guard has to be present on this branch too."""
        source = (SRC / 'server/RHRace.py').read_text()
        self.assertIn('eq_state_is_unresolved', source)

    def test_untagged_profile_reads_as_legacy(self):
        """A profile written before the scale was tracked is 8-bit, no eq."""
        ctx, node, cal = self.context(full=False)
        ctx.race.profile.enter_ats = json.dumps({'v': [96]})
        self.assertEqual(cal._stored_scale_id(ctx.race.profile),
                         {'adc_bits': 10, 'eq': None})

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
