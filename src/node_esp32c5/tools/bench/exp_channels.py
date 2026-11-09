import sys; sys.path.insert(0, '/home/meccup/rh-c5-work')
from bench import *
sen, sim = setup()
print('LO plan:', [l.split(':')[0] for l in sen.cmd('scan 10')][:8])
def show(label, r):
    print('%-16s' % label, ' '.join('%6.1f' % r[f][0] for f in RB), ' clip', sum(r[f][3] for f in RB), flush=True)
print('%-16s' % 'sensor dB', ' '.join('  R%d  ' % (i+1) for i in range(8)))
show('off', scan(sen))
for f in RB:
    sim.cmd('level %d 0' % f)
    show('sim %d' % f, scan(sen))
    sim.cmd('off')
show('off again', scan(sen))
