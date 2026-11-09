import sys; sys.path.insert(0, '/home/meccup/rh-c5-work')
from bench import *
sen, sim = setup()
K = 4096 / 240.0
sim.cmd('level 5769 0')
r = scan(sen, 150); print('PLL on R4, offset 0: R4 %.1f' % r[5769][0])
for target in [5658, 5695, 5732, 5806, 5843, 5880]:
    off = round((target - 5769) * K)
    sim.cmd('tone2 1 %d 0 1' % (off & 0xFFF))
    r = scan(sen, 150)
    print('tone at %d (offset %+4d MHz, word %+5d): that seat %6.1f dB, R4 %6.1f, others max %5.1f' % (
        target, target - 5769, off, r[target][0], r[5769][0],
        max(r[f][0] for f in RB if f not in (target, 5769))), flush=True)
sim.cmd('off')
