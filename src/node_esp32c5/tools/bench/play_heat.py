import sys, json, time
sys.path.insert(0, '/home/meccup/rh-c5-work')
import race
from bench import *
sim = Dev(SIM, False); sim.boot()
ps = race.race(seed=7, secs=300, beam=17.5, kmh=100)
host_skips = race.plan(ps)
sim.cmd('script clear')
t = time.monotonic()
for p in ps:
    r = sim.cmd('script add %d %d %.1f %.1f %.1f %.1f %.2f %d' % (p['t_ms'], p['mhz'], p['kmh'], p['cm'], p['beam'], p['side'], p['peak'], p['id']))
    if r: print('add error', r); break
print('uploaded %d passes in %.1f s' % (len(ps), time.monotonic() - t))
plan = sim.cmd('script check'); print(plan[0]); print('host planner skipped %d' % len(host_skips))
dev_skips = [json.loads(l) for l in plan[1:]]
same = sorted((s['t_ms'], s['mhz']) for s in dev_skips) == sorted((p['t_ms'], p['mhz']) for p, _ in host_skips)
print('device and host planners agree on skips:', same)
t0 = time.monotonic()
out = sim.cmd('play in 500', wait=330)
evts = [json.loads(l) for l in out if l.startswith('{')]
prog = [e for e in evts if e['evt'] == 'progress']; passes = [e for e in evts if e['evt'] == 'pass']
print('played for %.1f s wall; %d pass events; progress ticks %d (last t_ms %d); done: %s' % (
    time.monotonic() - t0, len(passes), len(prog), prog[-1]['t_ms'] if prog else -1, [e for e in evts if e['evt'] == 'done']))
# events arrive in time order and match the plan
planned = sorted((p['t_ms'], p['mhz']) for p in ps if (p['t_ms'], p['mhz']) not in {(s['t_ms'], s['mhz']) for s in dev_skips})
got = sorted((e['t_ms'], e['mhz']) for e in passes)
print('every planned pass reported exactly once:', planned == got)
