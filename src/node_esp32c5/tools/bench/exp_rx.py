"""Sweep one receiver board against the fixed emulator.
   python exp_rx.py <receiver serial suffix e.g. 28:F0> <label>"""
import sys, json, time; sys.path.insert(0, '/home/meccup/rh-c5-work')
import bench
from bench import Dev, scan, RB
rx_serial, label = sys.argv[1], sys.argv[2]
SEN = '/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_38:44:BE:0E:%s-if00' % rx_serial
sen, sim = Dev(SEN, True), Dev(bench.SIM, False)
sen.boot(); sim.boot()
sim.cmd('ant 0'); sen.cmd('ant 0'); sen.cmd('gain 30')
for i in range(1, 8): sen.cmd('freq %d 0' % i)
freqs = list(range(5645, 5916, 5)) + [5917]
levels = [0, 6, 12]
data = {L: [] for L in levels}; floors = []
for f in freqs:
    sen.cmd('freq 0 %d' % f)
    sim.cmd('off'); fl = scan(sen, 60)[f][0]; floors.append(fl)
    for L in levels:
        sim.cmd('level %d %d' % (f, L))
        data[L].append(scan(sen, 100)[f][0] - fl)
    sim.cmd('off')
out = {'rx': rx_serial, 'label': label, 'time': time.strftime('%H:%M:%S'), 'freqs': freqs,
       'floors': floors, 'data': {str(L): data[L] for L in levels}}
json.dump(out, open('/home/meccup/rh-c5-work/rx_%s.json' % label, 'w'))
rb = [5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917]
near = lambda m: min(range(len(freqs)), key=lambda i: abs(freqs[i] - m))
print('%s (%s): full-power rise at R1..R8: %s' % (label, rx_serial, ' '.join('%.1f' % data[0][near(m)] for m in rb)))
d6 = [data[0][i] - data[6][i] for i in range(len(freqs))]; d12 = [data[0][i] - data[12][i] for i in range(len(freqs))]
print('   6 dB step reads %.2f (sd %.2f), 12 dB step reads %.2f (sd %.2f); floors %.1f..%.1f' % (
    sum(d6)/len(d6), (sum((x-sum(d6)/len(d6))**2 for x in d6)/len(d6))**.5, sum(d12)/len(d12),
    (sum((x-sum(d12)/len(d12))**2 for x in d12)/len(d12))**.5, min(floors), max(floors)))
