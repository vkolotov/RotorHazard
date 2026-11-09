import sys, threading, time, statistics
sys.path.insert(0, '/home/meccup/rh-c5-work')
from bench import *
sen, sim = setup()
for i in range(8):
    if i not in (2, 3): sen.cmd('freq %d 0' % i)
args = sys.argv[1:] or ['quad 1 5732 100 100 17.5 0', 'quad 2 5769 100 100 0 150']
sim.cmd('clear')
for a in args: print(a, sim.cmd(a))
res = {}
def tr(): res['t'] = sen.cmd('trace 12 1600', wait=60)
th = threading.Thread(target=tr); th.start()
time.sleep(0.25)
simout = sim.cmd('go', wait=10)
th.join()
print('sim:', simout)
rows = [l.split() for l in res['t'] if l.startswith('t ')]
t = [int(r[1]) for r in rows]; a = [float(r[2]) for r in rows]; b = [float(r[3]) for r in rows]
dt = [t[i+1]-t[i] for i in range(len(t)-1)]
print('sensor samples %d, cycle median %d us' % (len(t), statistics.median(dt)))
def analyse(name, v):
    floor = statistics.median(v[:200]); pk = max(v); ip = v.index(pk)
    def width(drop):
        above = [t[i] for i in range(len(v)) if v[i] >= pk - drop]
        # contiguous around the peak
        i0 = ip
        while i0 > 0 and v[i0-1] >= pk - drop: i0 -= 1
        i1 = ip
        while i1 < len(v)-1 and v[i1+1] >= pk - drop: i1 += 1
        return (t[i1] - t[i0]) / 1000.0
    print('%s: floor %.1f dB, peak %.1f dB (+%.1f) at %d us, width >= peak-3dB %.1f ms, -6dB %.1f ms, -10dB %.1f ms' % (
        name, floor, pk, pk - floor, t[ip], width(3), width(6), width(10)))
    return t[ip]
pa = analyse('R3 (horn)', a); pb = analyse('R4 (dome)', b)
sp = [int(l.split('peak ')[1].split()[0]) for l in simout if 'peak' in l]
if len(sp) == 2:
    print('peak separation: sensor %.1f ms, simulator %.1f ms' % ((pb - pa) / 1000.0, (sp[1] - sp[0]) / 1000.0))
# coarse picture: 5 ms bins around each peak
for name, v, p in (('R3', a, pa), ('R4', b, pb)):
    line = []
    for k in range(-60, 61, 5):
        vals = [v[i] for i in range(len(t)) if p + k*1000 <= t[i] < p + (k+5)*1000]
        line.append('%5.1f' % max(vals) if vals else '  .  ')
    print(name, 'ms -60..+60 by 5:', ' '.join(line))
