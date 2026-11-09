import sys; sys.path.insert(0, '/home/meccup/rh-c5-work')
from bench import *
sen, sim = setup()
def show(label, r):
    print('%-26s' % label, ' '.join('%6.1f' % r[f][0] for f in RB), flush=True)
print('%-26s' % 'sensor dB', ' '.join('  R%d  ' % (i+1) for i in range(8)))
print(sim.cmd('level 5750 0'))
show('PLL 5750, tone1 offset 0', scan(sen, 150))
for off in [64, 128, 256, 512, 1024, 2048, 4096-1024, 4096-512, 4096-256]:
    print(sim.cmd('tone2 1 %d 0 1' % off))
    show('tone1 offset %d' % off, scan(sen, 150))
sim.cmd('tone2 1 0 0 1'); sim.cmd('off')
