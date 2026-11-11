"""Moving-emulator test over several bands: each cycle retunes the receivers'
eight seats to one band, measures it with the emulator off, then lights each
channel in turn; then the next band. Bands share the same emulator
positions, so their levels are comparable."""
import sys, json, time, threading; sys.path.insert(0, '/home/meccup/rh-c5-work')
import bench
from bench import Dev, scan
P = '/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_38:44:BE:0E:%s-if00'
RX = ['28:F0', '26:A8', '26:18']
BANDS = {'L': [5362, 5399, 5436, 5473, 5510, 5547, 5584, 5621],
         'R': [5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917],
         # check points between Raceband channels (A/B/E/F interpolation)
         'M': [5645, 5677, 5714, 5750, 5788, 5825, 5862, 5899]}
DUR = float(sys.argv[1]) if len(sys.argv) > 1 else 300
OUT = sys.argv[2] if len(sys.argv) > 2 else '/home/meccup/rh-c5-work/moving2.json'
rxs = {s: Dev(P % s, True) for s in RX}
sim = Dev(bench.SIM, False) if len(sys.argv) <= 3 else None
for d in list(rxs.values()) + ([sim] if sim else []): d.boot()
if sim: sim.cmd('ant 0'); sim.cmd('off')
for d in rxs.values(): d.cmd('ant 0'); d.cmd('gain 30')
def par(fn):
    out = {}
    ts = [threading.Thread(target=lambda s=s: out.__setitem__(s, fn(s, rxs[s]))) for s in RX]
    [t.start() for t in ts]; [t.join() for t in ts]
    return out
def tune(band):
    fs = BANDS[band]
    par(lambda s, d: [d.cmd('freq %d %d' % (i, f)) for i, f in enumerate(fs)])
    for attempt in range(8):
        r = par(lambda s, d: scan(d, 10))
        if all(all(f in r[s] for f in fs) for s in RX): return
        for s in RX:  # re-send the seats that did not take
            for i, f in enumerate(fs):
                if f not in r[s]: rxs[s].cmd('freq %d %d' % (i, f))
        time.sleep(0.2)
    raise RuntimeError('band %s did not tune: %r' % (band, {s: sorted(r[s]) for s in RX}))
samples = []  # (t, band, emitted MHz or 0, {board: {MHz: mean dB}})
t0 = time.monotonic()
print('RUNNING', time.strftime('%T'), flush=True)
cyc = 0
while time.monotonic() - t0 < DUR:
    for band, fs in BANDS.items():
        tune(band)
        for f in [0] + fs:
            if sim: sim.cmd('level %d 0' % f if f else 'off')
            r = par(lambda s, d: scan(d, 20))
            samples.append((round(time.monotonic() - t0, 2), band, f, {s: {m: v[0] for m, v in r[s].items()} for s in RX}))
        if not sim: break
    cyc += 1
    if not sim: break
    json.dump({'bands': BANDS, 'boards': RX, 'samples': samples}, open(OUT, 'w'))
    if cyc % 5 == 0: print('cycle', cyc, round(time.monotonic() - t0), flush=True)
if sim: sim.cmd('off')
json.dump({'bands': BANDS, 'boards': RX, 'samples': samples}, open(OUT, 'w'))
print('done', time.strftime('%T'), 'cycles', cyc, flush=True)
