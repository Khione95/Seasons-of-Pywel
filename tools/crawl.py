"""Research: snapshot the memory near the climate/weather objects (read only),
following pointers a few levels, to compare weather states later.
  python crawl.py snap <name>        saves research/weather/<name>.npz
  python crawl.py diff <a> <b> [...] places whose float differs, with values"""
import os, struct, sys
import numpy as np
import memscan as ms

BASE = 0x140000000
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'research', 'weather')
SPAN = 0x1000
LEVELS = 2

def roots(h):
    def q(a):
        return struct.unpack('<Q', ms.read(h, a, 8))[0]
    world = q(q(BASE + 0x6D69198) + 0x40)
    mgr = q(world + 0xF68)
    other = q(q(BASE + 0x6D69190) + 0xE0)
    return {'world': world, 'climate': mgr, 'other': other, 'mgr90': q(mgr + 0x90)}

def snap(name):
    h = ms.open_game()
    r = roots(h)
    seen, frontier, blocks = set(), list(r.values()), {}
    for level in range(LEVELS + 1):
        nxt = []
        for a in frontier:
            a &= ~7
            if a in seen or not (0x10000 < a < 0x7FFFFFFFFFFF):
                continue
            seen.add(a)
            try:
                d = ms.read(h, a, SPAN)
            except Exception:
                continue
            if not d or len(d) < SPAN:
                continue
            blocks[a] = d
            for p in struct.unpack(f'<{SPAN // 8}Q', d):
                if 0x10000000000 < p < 0x7FFFFFFFFFFF and not (BASE <= p < BASE + 0x10000000):
                    nxt.append(p)
        frontier = nxt
        if len(blocks) > 6000:
            break
    os.makedirs(OUT, exist_ok=True)
    keys = np.array(sorted(blocks), np.uint64)
    data = np.frombuffer(b''.join(blocks[int(k)] for k in keys), np.uint8)
    np.savez_compressed(os.path.join(OUT, name + '.npz'), keys=keys, data=data, roots=np.array(list(r.values()), np.uint64))
    print(name, len(keys), 'blocks', {k: hex(v) for k, v in r.items()})

def load(name):
    z = np.load(os.path.join(OUT, name + '.npz'))
    return dict(zip(z['keys'].tolist(), z['data'].reshape(-1, SPAN)))

def diff(names, lo=-1e6, hi=1e6, limit=300):
    snaps = [load(n) for n in names]
    common = set(snaps[0])
    for s in snaps[1:]:
        common &= set(s)
    rows = []
    for a in sorted(common):
        fs = [np.frombuffer(s[a].tobytes(), np.float32) for s in snaps]
        st = np.stack(fs)
        ok = np.isfinite(st).all(axis=0) & (np.abs(st) < hi).all(axis=0)
        ch = ok & (np.abs(st.max(axis=0) - st.min(axis=0)) > 1e-4)
        for i in np.nonzero(ch)[0]:
            rows.append((a + 4 * int(i), st[:, i]))
    print(len(common), 'common blocks,', len(rows), 'changed floats')
    for addr, v in rows[:limit]:
        print(f'{addr:X}', ' '.join(f'{x:10.4g}' for x in v))

if __name__ == '__main__':
    if sys.argv[1] == 'snap':
        snap(sys.argv[2])
    else:
        diff(sys.argv[2:])
