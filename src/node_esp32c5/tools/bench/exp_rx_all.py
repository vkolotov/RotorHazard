import sys, json, time, threading; sys.path.insert(0, '/home/meccup/rh-c5-work')
import bench
from bench import Dev, scan
P = '/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_38:44:BE:0E:%s-if00'
RX = ['28:F0', '26:A8', '26:18']
rxs = {s: Dev(P % s, True) for s in RX}
sim = Dev(bench.SIM, False)
for d in list(rxs.values()) + [sim]: d.boot()
sim.cmd('ant 0')
for d in rxs.values():
    d.cmd('ant 0'); d.cmd('gain 30')
    for i in range(1, 8): d.cmd('freq %d 0' % i)
def rscan(d, f, n):
    for attempt in range(4):
        r = scan(d, n)
        if f in r:
            return r[f][0]
        d.s.reset_input_buffer(); d.cmd('freq 0 %d' % f)
    raise RuntimeError('seat would not tune to %d' % f)
def par(fn):
    out = {}
    ts = [threading.Thread(target=lambda s=s: out.__setitem__(s, fn(rxs[s]))) for s in RX]
    [t.start() for t in ts]; [t.join() for t in ts]
    return out
freqs = list(range(5645, 5916, 5)) + [5917]
levels = [0, 6, 12]
res = {s: {'floors': [], 'data': {L: [] for L in levels}} for s in RX}
for f in freqs:
    par(lambda d: d.cmd('freq 0 %d' % f))
    sim.cmd('off')
    fl = par(lambda d: rscan(d, f, 60))
    for s in RX: res[s]['floors'].append(fl[s])
    for L in levels:
        sim.cmd('level %d %d' % (f, L))
        r = par(lambda d: rscan(d, f, 100))
        for s in RX: res[s]['data'][L].append(r[s] - fl[s])
    sim.cmd('off')
json.dump({'freqs': freqs, 'res': {s: {'floors': v['floors'], 'data': {str(L): x for L, x in v['data'].items()}} for s, v in res.items()}},
          open('/home/meccup/rh-c5-work/rx_all.json', 'w'))
print('done', time.strftime('%T'))
