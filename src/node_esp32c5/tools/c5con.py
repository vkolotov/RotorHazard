#!/usr/bin/env python3
"""Send console commands to a C5 node ('!' lines) or the quad simulator (plain
lines) and print the replies.

    python tools/c5con.py PORT 'status' ['scan 200' ...] [--plain] [--wait S]

The port is opened with DTR and RTS low, as RotorHazard does, so the board is
not reset. Replies are read until 'ok'/'err' or the wait runs out.
"""
import argparse
import sys
import time

import serial


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('port')
    ap.add_argument('commands', nargs='+')
    ap.add_argument('--plain', action='store_true', help="no '!' prefix (quad simulator)")
    ap.add_argument('--wait', type=float, default=5.0, help='seconds to wait for each reply')
    a = ap.parse_args()
    ser = serial.Serial(port=None, baudrate=115200, timeout=0.1)
    ser.dtr = False
    ser.rts = False
    ser.port = a.port
    ser.open()
    time.sleep(0.2)
    ser.reset_input_buffer()
    for cmd in a.commands:
        ser.write((('' if a.plain else '!') + cmd + '\n').encode())
        end = time.monotonic() + a.wait
        buf = b''
        while time.monotonic() < end:
            buf += ser.read(512)
            while b'\n' in buf:
                line, buf = buf.split(b'\n', 1)
                text = line.decode(errors='replace').rstrip()
                print(text)
                if text == 'ok' or text.startswith('err'):
                    end = 0
                    break
        sys.stdout.flush()
    ser.close()


if __name__ == '__main__':
    main()
