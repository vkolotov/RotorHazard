import sys, threading, time, json, statistics
sys.path.insert(0, '/home/meccup/rh-c5-work')
from bench import *
sen, sim = setup()
for i in range(8):
    if i not in (2, 3): sen.cmd('freq %d 0' % i)
script = [  # t_ms MHz kmh cm beam side peak id
    (800, 5732, 100, 100, 17.5, -20, 0, 3),
    (1300, 5769, 100, 100, 17.5, -20, 0, 4),
    (1310, 5732, 100, 100, 17.5, -20, 0, 3),
    (1800, 5658, 100, 100, 17.5, -20, 0, 1),
    (1805, 5769, 100, 100, 17.5, -20, 0, 4),
    (2400, 5732, 100, 100, 0, -20, 0, 3),
]
print(sim.cmd('script clear'))
for p in script: sim.cmd('script add %d %d %g %g %g %g %g %d' % p)
for l in sim.cmd('script check'): print('plan:', l)
res = {}
def tr(): res['t'] = sen.cmd('trace 12 3800', wait=60)
th = threading.Thread(target=tr); th.start(); time.sleep(0.2)
clk = json.loads(sim.cmd('clock')[0])['clock_us']
out = sim.cmd('play in 300', wait=20)
th.join()
evts = [json.loads(l) for l in out if l.startswith('{')]
for e in evts: print('sim:', e)
t0 = clk + 300000  # approx board t=0 (clock read ~ms before play)
rows = [l.split() for l in res['t'] if l.startswith('t ')]
t = [int(r[1]) for r in rows]; a = [float(r[2]) for r in rows]; b = [float(r[3]) for r in rows]
fa, fb = statistics.median(a[:100]), statistics.median(b[:100])
def peaks(v, fl, name):
    out, i = [], 0
    while i < len(v):
        if v[i] > fl + 6:
            j = i
            while j < len(v) and v[j] > fl + 3: j += 1
            k = max(range(i, j), key=lambda q: v[q])
            out.append((t[k], v[k] - fl, (t[j-1] - t[i]) / 1000.0)); i = j
        else: i += 1
    print('%s peaks on sensor (time order): %s' % (name, ', '.join('+%.1f dB width %.0f ms' % (h, w) for _, h, w in out)))
    return out
pa = peaks(a, fa, 'R3'); pb = peaks(b, fb, 'R4')
# spacing check against the script (sensor clock is independent: compare differences)
if len(pa) >= 3 and pb:
    print('R3 peak spacing (sensor): %.0f ms, %.0f ms   script: 510, 1090' % ((pa[1][0]-pa[0][0])/1000, (pa[2][0]-pa[1][0])/1000))
    print('R4 pass vs R3 2nd pass (sensor): %+.0f ms   script: -10' % ((pb[0][0]-pa[1][0])/1000))
