"""Emulator passes vs the timer's real RotorHazard detection (RH binary protocol over USB)."""
import sys, time, struct, json
sys.path.insert(0, '/home/meccup/rh-c5-work')
from bench import *
sen, sim = setup()            # sets seats 0-7 to R1..R8 via console
sim.cmd('ant 0'); sen.cmd('ant 0')
s = sen.s
def wr(cmd, payload):
    b = bytes([cmd]) + payload + bytes([sum(payload) & 0xFF]); s.write(b); time.sleep(0.02)
def rd(cmd, n):
    s.reset_input_buffer(); s.write(bytes([cmd])); d = b''; t = time.monotonic()
    while len(d) < n + 1 and time.monotonic() - t < 0.5: d += s.read(n + 1 - len(d))
    assert len(d) == n + 1 and (sum(d[:n]) & 0xFF) == d[n], d
    return d[:n]
SEAT, MHZ = 3, 5769
wr(0x7A, bytes([SEAT])); wr(0x51, struct.pack('>H', MHZ)); wr(0x6A, bytes([12]))
time.sleep(3.0)
print(sen.cmd('filter'))
floor = struct.unpack('>H', rd(0x0D, 11)[3:5])[0]
enter, exit_ = floor + 10 * 64, floor + 6 * 64     # EnterAt 10 dB, ExitAt 6 dB over the floor (64 counts/dB)
wr(0x71, struct.pack('>H', enter)); wr(0x72, struct.pack('>H', exit_))
print('floor %d counts; EnterAt %d (+10 dB), ExitAt %d (+6 dB)' % (floor, enter, exit_))
sim.cmd('ref 100')
print('%-34s %8s %10s %12s' % ('pass (dome, ref 100 cm)', 'lap?', 'RH peak', 'RH peak dB'))
for cm in (30, 100, 200):
    for kmh in (5, 30, 60, 100, 150, 200):
        lap0 = rd(0x0D, 11)[0]
        wr(0x69, bytes([0]))   # reset node peak/nadir
        time.sleep(0.4)
        sim.cmd('clear'); sim.cmd('quad 1 %d %d %d 0 0' % (MHZ, kmh, cm)); sim.cmd('go', wait=15)
        time.sleep(0.6)
        d = rd(0x0D, 11)
        lap1 = d[0]; npk = struct.unpack('>H', d[5:7])[0]
        print('%-34s %8s %10d %+11.1f' % ('%3d km/h, closest %3d cm' % (kmh, cm), 'yes' if lap1 != lap0 else 'NO', npk, (npk - floor) / 64))
sim.cmd('ref 0')
