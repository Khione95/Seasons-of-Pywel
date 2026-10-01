"""Find clock-like values: read memory 3 times, keep values that rise at a steady
rate (int32 / float32 / float64 / int64) inside the given ranges."""
import time, numpy as np, memscan as ms

RANGES = [(0, 1440), (0, 86400), (6.5e6, 6.8e6), (109000, 113000), (0.0, 1.0), (0.0, 24.0)]
TYPES = [('i4', 4), ('f4', 4), ('f8', 8), ('i8', 8)]


def snapshot(h, regs):
    out = []
    for base, size in regs:
        d = ms.read(h, base, size)
        out.append(d)
    return out


h = ms.open_game()
regs = [r for r in ms.regions(h) if r[1] <= 1 << 28]
t = []; snaps = []
for i in range(3):
    t.append(time.time()); snaps.append(snapshot(h, regs)); time.sleep(8 if i < 2 else 0)
r1 = (t[1] - t[0]); r2 = (t[2] - t[1])
found = []
for k, (base, size) in enumerate(regs):
    ds = [s[k] for s in snaps]
    if any(d is None for d in ds):
        continue
    for tp, st in TYPES:
        n = len(ds[0]) // 8 * 8
        a = [np.frombuffer(d[:n], tp).astype(np.float64) for d in ds]
        with np.errstate(all='ignore'):
            d1 = a[1] - a[0]; d2 = a[2] - a[1]
            ok = (d1 > 0) & (d2 > 0) & (np.abs(d1 / r1 - d2 / r2) <= 0.05 * np.abs(d1 / r1))
            inr = np.zeros(len(a[0]), bool)
            for lo, hi in RANGES:
                inr |= (a[2] >= lo) & (a[2] <= hi)
            idx = np.nonzero(ok & inr & np.isfinite(a[2]))[0]
        for j in idx:
            found.append((tp, base + int(j) * st, a[2][j], d2[j] / r2))
print(len(found))
import collections
c = collections.Counter((f[0], round(f[3], 3)) for f in found)
for (tp, rate), n in c.most_common(40):
    ex = [f for f in found if f[0] == tp and round(f[3], 3) == rate][:3]
    print(tp, 'rate/s', rate, n, [(hex(e[1]), round(e[2], 4)) for e in ex])
import pickle; pickle.dump(found, open(ms.HERE + '/../research/re/clock.pkl', 'wb'))
