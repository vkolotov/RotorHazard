import sys; sys.path.insert(0, '/home/meccup/rh-c5-work')
from bench import *
sen, sim = setup()
sim.cmd('level 5750 0')
res = []
for off in list(range(160, 420, 20)) + [4096 - v for v in range(160, 420, 20)]:
    sim.cmd('tone2 1 %d 0 1' % off)
    r = scan(sen, 120)
    signed = off - 4096 if off >= 2048 else off
    res.append((signed, r[5732][0], r[5769][0]))
    print('offset %+5d  R3 %6.1f  R4 %6.1f' % res[-1], flush=True)
sim.cmd('off')
