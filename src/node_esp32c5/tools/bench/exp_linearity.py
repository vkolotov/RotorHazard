import sys; sys.path.insert(0, '/home/meccup/rh-c5-work')
from bench import *
sen, sim = setup()
steps = [0, 4, 8, 12, 14]
print('reading above floor (dB) at emulator level 0/-4/-8/-12/-14 dB; slope = dB read per dB sent')
fl = scan(sen, 200)
for i, f in enumerate(RB):
    row = []
    for st in steps:
        sim.cmd('level %d %d' % (f, st))
        r = scan(sen, 200)
        row.append(r[f][0] - fl[f][0])
    sim.cmd('off')
    # slope over the steps still well above the floor (> 6 dB)
    pts = [(steps[k], row[k]) for k in range(len(steps)) if row[k] > 6]
    n = len(pts); mx = sum(p[0] for p in pts)/n; my = sum(p[1] for p in pts)/n
    slope = -sum((p[0]-mx)*(p[1]-my) for p in pts) / sum((p[0]-mx)**2 for p in pts)
    print('R%d %d: floor %5.1f  %s   slope %.2f' % (i+1, f, fl[f][0], ' '.join('%5.1f' % v for v in row), slope), flush=True)
