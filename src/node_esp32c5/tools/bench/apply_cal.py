import sys, threading; sys.path.insert(0, '/home/meccup/rh-c5-work')
import bench
from bench import Dev, RB
P = '/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_38:44:BE:0E:%s-if00'
RX = ['28:F0', '26:A8', '26:18']
TABLE = dict(zip(RB, [2.0, 2.0, 1.3, 1.0, 0.9, -1.1, -2.3, -3.9]))  # moving_20261109.json average
rxs = {s: Dev(P % s, True) for s in RX}
sim = Dev(bench.SIM, False)
for d in list(rxs.values()) + [sim]: d.boot()
sim.cmd('off')
def one(s):
    d = rxs[s]; log = []
    for i, f in enumerate(RB): d.cmd('freq %d %d' % (i, f))
    log += d.cmd('cal clear')
    log += d.cmd('cal floor 3000', wait=20)
    for f, g in TABLE.items(): log += d.cmd('cal set %d %.1f' % (f, g))
    log += d.cmd('cal on'); log += d.cmd('cal save'); log += d.cmd('cal show')
    return log
out = {}
ts = [threading.Thread(target=lambda s=s: out.__setitem__(s, one(s))) for s in RX]
[t.start() for t in ts]; [t.join() for t in ts]
for s in RX: print(s); print('\n'.join('  ' + l for l in out[s]))
