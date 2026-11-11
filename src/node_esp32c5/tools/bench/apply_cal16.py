"""Write the 16-point L+R gain table (combined moving-emulator curve,
2026-11-09) to the three receivers: floors measured on both bands with the
emulator off, then gains, cal on, save."""
import sys, threading, time; sys.path.insert(0, '/home/meccup/rh-c5-work')
import bench
from bench import Dev, scan
P = '/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_38:44:BE:0E:%s-if00'
RX = ['28:F0', '26:A8', '26:18']
L = [5362, 5399, 5436, 5473, 5510, 5547, 5584, 5621]
R = [5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917]
GAIN = dict(zip(L + R, [1.2, 1.5, 1.7, 2.0, 2.2, 2.4, 2.6, 2.6,
                        2.4, 2.1, 1.7, 1.0, 0.0, -1.2, -2.7, -4.5]))
rxs = {s: Dev(P % s, True) for s in RX}
sim = Dev(bench.SIM, False)
for d in list(rxs.values()) + [sim]: d.boot()
sim.cmd('off')
def tune(d, fs):
    for i, f in enumerate(fs): d.cmd('freq %d %d' % (i, f))
    for a in range(8):
        r = scan(d, 10)
        miss = [f for f in fs if f not in r]
        if not miss: return
        for f in miss: d.cmd('freq %d %d' % (fs.index(f), f))
    raise RuntimeError('did not tune %r' % miss)
def one(s):
    d = rxs[s]; log = d.cmd('cal clear')
    for fs in (L, R):
        tune(d, fs); log += d.cmd('cal floor 3000', wait=20)
    for f, g in GAIN.items(): log += d.cmd('cal set %d %.1f' % (f, g))
    log += d.cmd('cal on'); log += d.cmd('cal save')
    return [l for l in log if l.startswith('err')]
out = {}
ts = [threading.Thread(target=lambda s=s: out.__setitem__(s, one(s))) for s in RX]
[t.start() for t in ts]; [t.join() for t in ts]
print('errors:', out)
