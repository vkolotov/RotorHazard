"""Lap timestamps must not depend on read retries.

RotorHazard dates a lap as ``readtime - ms_since_lap``, where ``ms_since_lap``
comes from the node's READ_LAP_PASS_STATS reply. The node computes it from its
own clock at the moment it handles that command (``handleReadLapPassStats(
millis())`` in src/node/commands.cpp), so ``readtime`` must be the moment of that
same reply. On a lossy link (e.g. an ELRS AirPort radio) a reply can be lost and
retried; the retry must not shift the lap.

These tests drive the real ``RHInterface.update()`` and ``SerialNode.read_block()``
against a fake serial port that emulates node firmware plus link latency, with a
fake clock. A dropped reply returns nothing after the serial timeout, as pyserial
does.
"""
import gevent.event
import gevent.lock
import sys
import unittest
from pathlib import Path
from unittest.mock import Mock, patch

SRC = Path(__file__).resolve().parents[1]
for folder in ('interface', 'server'):
    sys.path.insert(0, str(SRC / folder))

import BaseHardwareInterface as BHI
import RHInterface
import serial_node

SERIAL_TIMEOUT = 0.25   # serial_node.py opens the port with timeout=0.25
UPLINK = 0.004          # host -> node latency per command (s)
DOWNLINK = 0.005        # node -> host latency per reply (s)
TOL = 0.003             # 1 ms ms_since_lap resolution + float noise


class FakeClock:
    def __init__(self, t=1000.0):
        self.t = t

    def monotonic(self):
        return self.t

    def sleep(self, secs=0):
        self.t += secs


class FakeNodeLink:
    """Node firmware + radio link behind a serial port.

    The node stamps each command with its own clock when it handles it, like
    src/node/commands.cpp. ``drop`` decides per command whether its reply is lost.
    """
    port = 'fake'

    def __init__(self, clock, node_epoch=990.0, wide_rssi=True):
        self.clock = clock
        self.node_epoch = node_epoch      # node clock is unrelated to the host's
        self.rssi = u16 if wide_rssi else (lambda v: [v & 0xFF])   # API 38 STM32: 16-bit RSSI
        self.crossings = []               # host times of lap crossings
        self.peak = None                  # (host start, host end, rssi) of a reported peak
        self.drop = lambda cmd: False
        self._reply = None
        self._lost = False

    def node_ms(self, host_t):
        return int((host_t - self.node_epoch) * 1000)

    def flushInput(self):
        self._reply = None

    def write(self, data):
        cmd = data[0]
        handled_at = self.clock.t + UPLINK
        self._lost = self.drop(cmd)
        self._reply = self._build(cmd, handled_at)
        self._handled_at = handled_at

    def read(self, n):
        if self._lost or self._reply is None:
            self.clock.t += SERIAL_TIMEOUT
            return b''
        self.clock.t = self._handled_at + DOWNLINK
        return bytes(self._reply[:n])

    def _build(self, cmd, t):
        now = self.node_ms(t)
        past = [c for c in self.crossings if c <= t]
        lap_id = len(past) & 0xFF
        last_ts = self.node_ms(past[-1]) if past else 0
        r = self.rssi
        if cmd == RHInterface.READ_LAP_PASS_STATS:
            ms_since = (now - last_ts) if past else 0
            body = [lap_id] + u16(ms_since) + r(100) + r(150) + r(140) + u16(100)
        elif cmd == RHInterface.READ_LAP_EXTREMUMS:
            if self.peak and self.peak[1] <= t:
                start, end, rssi = self.peak
                first_ms = now - self.node_ms(start)              # ms *since* first peak time
                duration = self.node_ms(end) - self.node_ms(start)
                body = [RHInterface.LAPSTATS_FLAG_PEAK] + r(80) + r(70) + \
                       r(rssi) + u16(first_ms) + u16(duration)
            else:
                body = [0] + r(80) + r(70) + r(0) + u16(0) + u16(0)
        else:
            raise AssertionError('unexpected command {}'.format(cmd))
        return body + [RHInterface.calculate_checksum(body)]


def u16(v):
    return [(v >> 8) & 0xFF, v & 0xFF]


class LapReadtimeTest(unittest.TestCase):
    API_LEVEL = 38          # STM32 with 16-bit RSSI: 11-byte lap stats + extremums
    PEAK_RSSI = 200

    def setUp(self):
        self.clock = FakeClock()
        self.link = FakeNodeLink(self.clock, wide_rssi=self.API_LEVEL >= 38)
        patches = [
            patch.object(serial_node, 'monotonic', self.clock.monotonic),
            patch.object(RHInterface, 'monotonic', self.clock.monotonic),
            patch.object(BHI, 'monotonic', self.clock.monotonic),
            patch.object(serial_node.gevent, 'sleep', self.clock.sleep),
        ]
        for p in patches:
            p.start()
            self.addCleanup(p.stop)

        node = serial_node.SerialNode(0, self.link)
        node.api_level = self.API_LEVEL
        node.firmware_proctype_str = 'STM32F1'
        node.init()
        node.frequency = 5658
        self.node = node

        intf = RHInterface.RHInterface.__new__(RHInterface.RHInterface)
        intf.nodes = [node]
        intf.data_loggers = {}
        intf.race_status = BHI.BaseHardwareInterface.RACE_STATUS_RACING
        intf.intf_read_block_count = 0
        intf.intf_read_error_count = 0
        intf.node_crossing_callback = None
        intf.new_enter_or_exit_at_callback = None
        intf.log = Mock()
        self.passes = []
        intf.pass_record_callback = lambda node, ts, source, peak=None: self.passes.append(ts)
        self.intf = intf

    def cycle(self):
        self.intf.update()
        self.clock.sleep(0.1)  # RotorHazard's UPDATE_SLEEP

    def record_lap(self, drop_after_crossing=lambda cmd, n: False):
        """Fly one lap: settle, cross the gate, poll until RotorHazard records it.

        drop_after_crossing(cmd, n) -> True loses the reply to the n-th (0-based)
        occurrence of cmd after the crossing. Returns lap timestamp - true crossing.
        """
        for _ in range(3):
            self.cycle()                     # node_lap_id becomes 0, nothing recorded
        crossing = self.clock.t + 0.0371     # mid-cycle, not aligned to any read
        self.link.crossings.append(crossing)
        counts = {}

        def drop(cmd):
            if self.clock.t < crossing:
                return False
            n = counts.get(cmd, 0)
            counts[cmd] = n + 1
            return drop_after_crossing(cmd, n)

        self.link.drop = drop
        self.clock.sleep(0.05)
        for _ in range(5):
            self.cycle()
            if self.passes:
                break
        self.link.drop = lambda cmd: False
        self.assertEqual(len(self.passes), 1, 'lap not recorded')
        return self.passes[0] - crossing

    def test_clean_link_dates_lap_at_crossing(self):
        err = self.record_lap()
        self.assertLess(abs(err), 0.015, 'error {:.1f} ms'.format(err * 1000))

    def test_lost_extremums_reply_does_not_shift_lap(self):
        clean = LapReadtimeTest.clean_error(self)
        err = self.record_lap(lambda cmd, n: cmd == RHInterface.READ_LAP_EXTREMUMS and n == 0)
        self.assertAlmostEqual(err, clean, delta=TOL,
                               msg='lap shifted by {:.1f} ms after a lost EXTREMUMS reply'
                                   .format((err - clean) * 1000))

    def test_lost_pass_stats_reply_does_not_shift_lap(self):
        clean = LapReadtimeTest.clean_error(self)
        err = self.record_lap(lambda cmd, n: cmd == RHInterface.READ_LAP_PASS_STATS and n == 0)
        self.assertAlmostEqual(err, clean, delta=TOL,
                               msg='lap shifted by {:.1f} ms after a lost PASS_STATS reply'
                                   .format((err - clean) * 1000))

    def test_history_dated_from_extremums_reply(self):
        """Peak history times are 'ms since' the EXTREMUMS reply; a lost reply must not shift them."""
        for _ in range(3):
            self.cycle()
        start = self.clock.t + 0.0123
        end = start + 0.080
        self.link.peak = (start, end, self.PEAK_RSSI)
        lost = {'n': 0}

        def drop(cmd):
            if cmd == RHInterface.READ_LAP_EXTREMUMS and self.clock.t > end and lost['n'] == 0:
                lost['n'] = 1
                return True
            return False

        self.link.drop = drop
        self.clock.sleep(0.2)
        self.cycle()
        self.assertEqual(lost['n'], 1, 'test did not drop a reply')
        times = [t for t, v in zip(self.node.history_times, self.node.history_values) if v == self.PEAK_RSSI]
        self.assertTrue(times, 'no peak history recorded')
        self.assertAlmostEqual(times[0], start, delta=0.015,
                               msg='peak start dated {:.1f} ms off'.format((times[0] - start) * 1000))

    def test_failed_extremums_read_does_not_crash_or_lose_lap(self):
        """All EXTREMUMS attempts lost: update() must not raise, and the lap still counts."""
        err = self.record_lap(lambda cmd, n: cmd == RHInterface.READ_LAP_EXTREMUMS and n <= RHInterface.MAX_RETRY_COUNT)
        clean = LapReadtimeTest.clean_error(self)
        self.assertAlmostEqual(err, clean, delta=TOL)

    def clean_error(self):
        """Lap error on a clean link, measured on a fresh fixture."""
        other = type(self)(self._testMethodName)
        other.setUp()
        try:
            return other.record_lap()
        finally:
            other.doCleanups()


class LapReadtimeNarrowRssiTest(LapReadtimeTest):
    '''Same checks on API 32-37 nodes: 8-bit RSSI, 8-byte lap stats + extremums.'''
    API_LEVEL = 35


if __name__ == '__main__':
    unittest.main()
