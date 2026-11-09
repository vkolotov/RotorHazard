import sys, json; sys.path.insert(0, '/home/meccup/rh-c5-work')
from bench import *
sen, sim = setup()
sim.cmd('ant 0'); sen.cmd('ant 0')
levels = list(range(0, 15, 2))
res = {}
for g in (30, 40, 50, 60, 70):
    sen.cmd('gain %d' % g)
    sim.cmd('off'); fl = scan(sen, 150)
    res[g] = {'floor': {f: fl[f][0] for f in RB}, 'read': {}, 'clip': {}}
    for f in RB:
        row, clip = [], []
        for L in levels:
            sim.cmd('level %d %d' % (f, L))
            r = scan(sen, 120)[f]
            row.append(r[0]); clip.append(r[3])
        sim.cmd('off')
        res[g]['read'][f] = row; res[g]['clip'][f] = clip
    print('gain %d done, floor %.1f..%.1f' % (g, min(res[g]['floor'].values()), max(res[g]['floor'].values())), flush=True)
sen.cmd('gain 30')
json.dump({'levels': levels, 'res': {str(g): {k: {str(f): v for f, v in d.items()} for k, d in r.items()} for g, r in res.items()}},
          open('/home/meccup/rh-c5-work/power_curve.json', 'w'))
