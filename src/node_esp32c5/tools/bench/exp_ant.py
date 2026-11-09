import sys; sys.path.insert(0, '/home/meccup/rh-c5-work')
from bench import *
sen, sim = setup()
fl = scan(sen, 150)[5769][0]
sim.cmd('level 5769 0')
for a in (0, 1, 0):
    print(sen.cmd('ant %d' % a)[0], end='  ->  ')
    r = scan(sen, 200)[5769]
    print('R4 %.1f dB (floor %.1f, signal %+.1f dB above it)' % (r[0], fl, r[0] - fl), flush=True)
sim.cmd('off')
sen.cmd('ant 0'); sen.cmd('save')
