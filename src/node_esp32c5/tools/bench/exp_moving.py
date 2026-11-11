"""Moving-emulator test: the emulator cycles R1-R8 while being moved by hand;
all three receivers read all eight Raceband seats at once. Averaging many
positions removes the room's notches and leaves each board's channel shape."""
import sys, json, time, threading; sys.path.insert(0, '/home/meccup/rh-c5-work')
import bench
from bench import Dev, scan, RB
P = '/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_38:44:BE:0E:%s-if00'
RX = ['28:F0', '26:A8', '26:18']
DUR = float(sys.argv[1]) if len(sys.argv) > 1 else 200
OUT = sys.argv[2] if len(sys.argv) > 2 else '/home/meccup/rh-c5-work/moving.json'
rxs = {s: Dev(P % s, True) for s in RX}
sim = Dev(bench.SIM, False)
for d in list(rxs.values()) + [sim]: d.boot()
sim.cmd('ant 0'); sim.cmd('off')
for d in rxs.values():
    d.cmd('ant 0'); d.cmd('gain 30')
    for i, f in enumerate(RB): d.cmd('freq %d %d' % (i, f))
def par(fn):
    out = {}
    ts = [threading.Thread(target=lambda s=s: out.__setitem__(s, fn(rxs[s]))) for s in RX]
    [t.start() for t in ts]; [t.join() for t in ts]
    return out
# all seats must be tuned before starting
for attempt in range(5):
    r = par(lambda d: scan(d, 20))
    missing = {s: [f for f in RB if f not in r[s]] for s in RX}
    if not any(missing.values()): break
    for s, m in missing.items():
        for f in m: rxs[s].cmd('freq %d %d' % (RB.index(f), f))
else:
    raise RuntimeError('seats not tuned: %r' % missing)
samples = []  # (t, emitted MHz or 0 for off, {board: {MHz: mean dB}})
t0 = time.monotonic()
print('RUNNING', time.strftime('%T'), flush=True)
cyc = 0
while time.monotonic() - t0 < DUR:
    for f in [0] + RB:
        if f: sim.cmd('level %d 0' % f)
        else: sim.cmd('off')
        r = par(lambda d: scan(d, 20))
        samples.append((round(time.monotonic() - t0, 2), f, {s: {m: v[0] for m, v in r[s].items()} for s in RX}))
    cyc += 1
    if cyc % 10 == 0: print('cycle', cyc, round(time.monotonic() - t0), flush=True)
sim.cmd('off')
json.dump({'rb': RB, 'boards': RX, 'samples': samples}, open(OUT, 'w'))
print('done', time.strftime('%T'), 'cycles', cyc, flush=True)
