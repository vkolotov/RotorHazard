"""Synthetic 8-pilot heat, the board planner replicated for 1/2/4-board splits."""
import math, random
RB = [5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917]
def pass_db(x, d, beam, side):
    path = -10 * math.log10((d*d + x*x) / (d*d))
    if beam <= 0: return path
    th = math.degrees(math.atan(abs(x) / d)); g = max(-3 * (th / beam) ** 2, side)
    return path + g
def half_us(p, rng=14.0):
    d, v = p['cm'] / 100, p['kmh'] / 3.6
    lo, hi = 0.0, 500.0
    for _ in range(32):
        mid = (lo + hi) / 2
        if pass_db(mid, d, p['beam'], p['side']) - p['peak'] > -rng: lo = mid
        else: hi = mid
    return int(hi / v * 1e6)
def race(seed=1, secs=300, beam=17.5, kmh=100):
    r = random.Random(seed); passes = []
    for i, f in enumerate(RB):
        pace = r.uniform(22, 30); t = r.uniform(1.5, 3.0)
        while t < secs:
            passes.append(dict(t_ms=int(t*1000), mhz=f, kmh=kmh*r.uniform(0.85, 1.15), cm=r.uniform(60, 160),
                               beam=beam, side=-20, peak=r.uniform(0, 3), id=i+1))
            t += pace + r.gauss(0, 1.5)
    return sorted(passes, key=lambda p: p['t_ms'])
def plan(passes, retune_us=2000, max_off=22.0, off=18.5):
    sign = lambda m: (-1 if RB.index(m) % 2 == 0 else 1) if m in RB else -1
    ps = sorted(passes, key=lambda p: p['t_ms']*1000 - half_us(p)); skipped = []
    cend = -10**18; pll = 0; tend = [-10**18]*2; lo = hi = 0
    for p in ps:
        h = half_us(p); s = p['t_ms']*1000 - h; e = p['t_ms']*1000 + h; why = 0
        if s >= cend + retune_us:
            pll = round(p['mhz'] - sign(p['mhz'])*off); lo = hi = p['mhz']; tend = [-10**18]*2
        elif s >= cend: why = 3
        else:
            l, hh = min(lo, p['mhz']), max(hi, p['mhz'])
            if abs(p['mhz'] - pll) > max_off or hh - l > 2*max_off: why = 1
        if not why:
            slot = 0 if tend[0] <= s else (1 if tend[1] <= s else -1)
            if slot < 0: why = 2
            else:
                tend[slot] = e; lo = min(lo, p['mhz']); hi = max(hi, p['mhz']); cend = max(cend, e)
        if why: skipped.append((p, why))
    return skipped
