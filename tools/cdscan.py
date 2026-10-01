"""Research: find a cooldown that counts down in real time (read only).
  python cdscan.py first            remaining seconds (float) / ms (u32) within the ranges
  python cdscan.py next             keep the places that went down as much as the clock"""
import os, sys, time
import numpy as np
import memscan as ms

HERE = os.path.dirname(os.path.abspath(__file__))
STATE = os.path.join(HERE, 'cd_state.npz')
F_LO, F_HI = 7000.0, 10800.0           # seconds left (the necklace: 3 hours, used a while ago)
I_LO, I_HI = 7_000_000, 10_800_000     # ms left

def first():
    h = ms.open_game()
    fa, fv, ia, iv = [], [], [], []
    for base, size in ms.regions(h):
        d = ms.read(h, base, size)
        if not d:
            continue
        n = len(d) // 4
        f = np.frombuffer(d, np.float32, n)
        i = np.frombuffer(d, np.uint32, n)
        k = np.nonzero((f >= F_LO) & (f <= F_HI))[0]
        fa.append(base + 4 * k.astype(np.int64)); fv.append(f[k])
        k = np.nonzero((i >= I_LO) & (i <= I_HI))[0]
        ia.append(base + 4 * k.astype(np.int64)); iv.append(i[k])
    fa, fv, ia, iv = map(np.concatenate, (fa, fv, ia, iv))
    np.savez(STATE, fa=fa, fv=fv, ia=ia, iv=iv.astype(np.int64), t=time.time())
    print(f'{len(fa)} float places, {len(ia)} u32 places')

def reread(h, addrs, dtype):
    out = np.full(len(addrs), np.nan if dtype == np.float32 else -1, np.float64)
    order = np.argsort(addrs)
    a = addrs[order]
    for base, size in ms.regions(h):
        i0, i1 = np.searchsorted(a, base), np.searchsorted(a, base + size)
        if i0 == i1:
            continue
        d = ms.read(h, base, size)
        if not d:
            continue
        arr = np.frombuffer(d, dtype, len(d) // 4)
        out[order[i0:i1]] = arr[(a[i0:i1] - base) // 4]
    return out

def nxt():
    s = np.load(STATE)
    h = ms.open_game()
    dt = time.time() - float(s['t'])
    f = reread(h, s['fa'], np.float32)
    i = reread(h, s['ia'], np.uint32)
    kf = np.abs((s['fv'] - f) - dt) < max(2.0, dt * 0.05)
    ki = np.abs((s['iv'] - i) - dt * 1000) < max(2000, dt * 50)
    fa, fv, ia, iv = s['fa'][kf], f[kf].astype(np.float32), s['ia'][ki], i[ki].astype(np.int64)
    np.savez(STATE, fa=fa, fv=fv, ia=ia, iv=iv, t=time.time())
    print(f'after {dt:.1f} s: {len(fa)} float places, {len(ia)} u32 places')
    for a, v in list(zip(fa, fv))[:20]:
        print(f'  float {a:X} {v:.1f} s left ({v / 60:.1f} min)')
    for a, v in list(zip(ia, iv))[:20]:
        print(f'  u32   {a:X} {v} ms left ({v / 60000:.1f} min)')

if __name__ == '__main__':
    first() if sys.argv[1] == 'first' else nxt()
