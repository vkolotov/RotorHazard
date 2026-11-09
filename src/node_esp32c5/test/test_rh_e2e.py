"""RotorHazard's own serial discovery and interface code against the C5 node
firmware's core (test/pty_node, built by `make -C test pty_node`).

Run with RotorHazard's Python environment:
    python test/test_rh_e2e.py
"""
import os
import struct
import subprocess
import sys
import time
import unittest
from pathlib import Path
from unittest.mock import Mock

HERE = Path(__file__).resolve().parent
SRC = HERE.parents[1]
for folder in ('interface', 'server', 'server/util'):
    sys.path.insert(0, str(SRC / folder))

import gevent  # noqa: E402
import gevent.lock  # noqa: E402,F401
import RHInterface  # noqa: E402
import serial_node  # noqa: E402

RACEBAND = [5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917]


class Intf(RHInterface.RHInterface):
    """Just enough of RHInterface for its node I/O helpers."""

    def __init__(self, nodes):  # pylint: disable=super-init-not-called
        self.nodes = nodes
        for node in nodes:
            node.init()   # as RHInterface's discovery does
        self.intf_read_block_count = 0
        self.intf_read_error_count = 0
        self.intf_write_block_count = 0
        self.intf_write_error_count = 0
        self.log = lambda *a, **k: None


class RhEndToEnd(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        binary = HERE / 'pty_node'
        cls.proc = subprocess.Popen([str(binary)], stdout=subprocess.PIPE, text=True)
        cls.port = cls.proc.stdout.readline().strip()
        config = Mock()
        config.get_item.return_value = [cls.port]
        t0 = time.monotonic()
        cls.nodes = serial_node.discover(0, config)
        cls.discover_secs = time.monotonic() - t0
        cls.intf = Intf(cls.nodes)

    @classmethod
    def tearDownClass(cls):
        for node in cls.nodes:
            try:
                node.serial.close()
            except Exception:
                pass
        cls.proc.kill()

    def console(self, line):
        ser = self.nodes[0].serial
        ser.reset_input_buffer()
        ser.write(('!' + line + '\n').encode())
        ser.flush()
        return ser.readline().decode().strip()

    def test_1_discovery(self):
        self.assertEqual(len(self.nodes), 8)
        for i, node in enumerate(self.nodes):
            self.assertEqual(node.api_level, 38)
            self.assertEqual(node.multi_node_index, i)
            self.assertEqual(node.firmware_proctype_str, 'ESP32C5')
            self.assertTrue(node.has_wide_rssi())
            self.assertTrue(node.supports_normalisation())
            self.assertTrue(node.supports_adc_resolution())
        print('\n  discovered 8 ESP32C5 nodes, API 38, fw %s, in %.1f s' %
              (self.nodes[0].firmware_version_str, self.discover_secs))

    def test_2_tune_and_time_a_pass(self):
        intf = self.intf
        for i, f in enumerate(RACEBAND):
            self.assertTrue(intf.set_frequency(i, f), 'seat %d frequency' % i)
            self.assertTrue(intf.set_adc_resolution(i, True), 'seat %d 12-bit' % i)
            intf.set_enter_at_level(i, 2400)
            intf.set_exit_at_level(i, 1700)
            self.assertEqual(self.nodes[i].enter_at_level, 2400)
        gevent.sleep(1.0)   # fill the 255 ms median filters
        node = self.nodes[3]

        def stats():
            data = node.read_block(intf, RHInterface.READ_LAP_PASS_STATS, 11)
            self.assertIsNotNone(data)
            t_read = time.monotonic()
            lap, ms_since = data[0], struct.unpack('>H', bytes(data[1:3]))[0]
            rssi = struct.unpack('>H', bytes(data[3:5]))[0]
            return lap, ms_since, rssi, t_read

        lap0, _, floor, _ = stats()
        self.assertTrue(100 < floor < 400, floor)

        # the node clock, read the way RH does
        t_req = time.monotonic()
        node_ms = intf.get_value_32(node, RHInterface.READ_TIME_MILLIS)
        t_resp = time.monotonic()
        node_offset = (t_req + t_resp) / 2 - node_ms / 1000.0

        reply = self.console('pass 5769 178 300')
        peak_ms = int(reply.split()[1])
        gevent.sleep(1.2)
        lap1, ms_since, _, t_read = stats()
        self.assertEqual(lap1, (lap0 + 1) % 256)
        lap_node_ms = (t_read - node_offset) * 1000.0 - ms_since
        err = lap_node_ms - peak_ms
        print('  pass on R4: lap time %+.1f ms from the true peak' % err)
        self.assertLess(abs(err), 30)
        for i in (0, 1, 2, 4, 5, 6, 7):
            data = self.nodes[i].read_block(intf, RHInterface.READ_LAP_PASS_STATS, 11)
            self.assertEqual(data[0], 0, 'seat %d counted a lap' % i)


if __name__ == '__main__':
    subprocess.check_call(['make', '-s', '-C', str(HERE), 'pty_node'])
    unittest.main(verbosity=1)
