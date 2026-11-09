import sys, json, statistics; sys.path.insert(0, '/home/meccup/rh-c5-work')
from bench import *
sen, sim = setup()
sim.cmd('ant 0'); sen.cmd('ant 0')
for i in range(1, 8): sen.cmd('freq %d 0' % i)
freqs = list(range(5645, 5916, 5)) + [5917]
levels = [0, 6, 12]
out = {'freqs': freqs, 'runs': []}
for run in range(2):
    data = {L: [] for L in levels}; floors = []
    for f in freqs:
        sen.cmd('freq 0 %d' % f)
        sim.cmd('off'); fl = scan(sen, 60)[f][0]; floors.append(fl)
        for L in levels:
            sim.cmd('level %d %d' % (f, L))
            data[L].append(scan(sen, 100)[f][0] - fl)
        sim.cmd('off')
    out['runs'].append({'floors': floors, 'data': {str(L): data[L] for L in levels}})
    print('run %d done' % (run + 1), flush=True)
json.dump(out, open('/home/meccup/rh-c5-work/sweep_ceramic.json', 'w'))
