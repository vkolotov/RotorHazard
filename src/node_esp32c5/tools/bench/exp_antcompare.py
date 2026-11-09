import sys, statistics; sys.path.insert(0, '/home/meccup/rh-c5-work')
from bench import *
sen, sim = setup()
def sweep():
    fl = scan(sen, 200)
    rise = []
    for f in RB:
        sim.cmd('level %d 0' % f)
        rise.append(scan(sen, 200)[f][0] - fl[f][0])
        sim.cmd('off')
    return [fl[f][0] for f in RB], rise
print('%-34s %s   spread' % ('signal above floor (dB)', ' '.join('  R%d ' % (i+1) for i in range(8))))
for label, ae, at in [('both ceramic', 0, 0), ('both external', 1, 1), ('emulator ext, timer ceramic', 1, 0),
                      ('emulator ceramic, timer ext', 0, 1), ('both external (repeat)', 1, 1)]:
    sim.cmd('ant %d' % ae); sen.cmd('ant %d' % at)
    floors, rise = sweep()
    print('%-34s %s   %4.1f   (floors %.1f..%.1f)' % (label, ' '.join('%5.1f' % v for v in rise),
          max(rise) - min(rise), min(floors), max(floors)), flush=True)
sen.cmd('ant 1'); sen.cmd('save'); sim.cmd('ant 1')
