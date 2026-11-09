#!/usr/bin/env python3
"""Record a RotorHazard timer's live node RSSI (socket.io 'heartbeat') for a
while and print per-node mean, min and max.

    python tools/rh_watch.py http://akltimer2 --secs 5
"""
import argparse
import statistics
import time

import socketio


def watch(url, secs):
    samples = {}
    sio = socketio.Client(reconnection=False)

    @sio.on('heartbeat')
    def heartbeat(data):
        for i, v in enumerate(data.get('current_rssi', [])):
            samples.setdefault(i, []).append(v)

    sio.connect(url, transports=['websocket', 'polling'], wait_timeout=10)
    time.sleep(secs)
    sio.disconnect()
    return samples


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('url')
    ap.add_argument('--secs', type=float, default=5)
    ap.add_argument('--label', default='')
    a = ap.parse_args()
    s = watch(a.url, a.secs)
    names = ['R1', 'R2', 'R3', 'R4', 'R5', 'R6', 'R7', 'R8']
    cells = []
    for i in sorted(s):
        v = s[i]
        cells.append('%s %5.1f [%d-%d]' % (names[i] if i < 8 else i, statistics.mean(v), min(v), max(v)))
    print('%-14s n=%d  %s' % (a.label, len(s.get(0, [])), '  '.join(cells)))


if __name__ == '__main__':
    main()
