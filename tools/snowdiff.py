"""Differential float scan for the snow research.
  python snowdiff.py snap          record floats in [0.3, 1.0] (in the snow)
  python snowdiff.py keep lo hi    keep kept places whose value is now in [lo, hi]
  python snowdiff.py show [n]      print kept places with values
  python snowdiff.py set <value> [i-j]   write value to kept places (backs up)
  python snowdiff.py restore       write back backed-up values
"""
import ctypes, os, struct, sys
import numpy as np
from memscan import open_game, regions, read, k32

HERE = os.path.dirname(os.path.abspath(__file__))
KEPT = os.path.join(HERE, 'snow_kept.npy')
BACK = os.path.join(HERE, 'snow_back.npy')

def snap():
    h = open_game(); addrs = []
    for base, size in regions(h):
        d = read(h, base, size)
        if not d: continue
        f = np.frombuffer(d[:len(d)//4*4], np.float32)
        idx = np.nonzero((f >= 0.3) & (f <= 1.0))[0]
        addrs.append(base + 4 * idx.astype(np.uint64))
    a = np.concatenate(addrs) if addrs else np.array([], np.uint64)
    a.sort(); np.save(KEPT, a); print(len(a), 'places')

def values(h, a):
    """Current floats at the sorted addresses a (one read per memory region)."""
    out = np.full(len(a), np.nan, np.float32)
    for base, size in regions(h):
        lo = np.searchsorted(a, np.uint64(base)); hi = np.searchsorted(a, np.uint64(base + size))
        if lo == hi: continue
        d = read(h, base, size)
        if not d: continue
        f = np.frombuffer(d[:len(d)//4*4], np.float32)
        out[lo:hi] = f[((a[lo:hi] - np.uint64(base)) // np.uint64(4)).astype(np.int64)]
    return out

def keep(lo, hi):
    h = open_game(); a = np.load(KEPT); v = values(h, a)
    k = a[(v >= lo) & (v <= hi)]
    np.save(KEPT, k); print(len(a), '->', len(k))

def show(n=40):
    h = open_game(); a = np.load(KEPT); v = values(h, a)
    for i in range(min(n, len(a))): print(i, '%X' % int(a[i]), v[i])
    print(len(a), 'kept')

def setv(val, sel=None):
    h = open_game(True); a = np.load(KEPT)
    if sel:
        lo, hi = (int(x) for x in sel.split('-')); a = a[lo:hi + 1]
    v = values(h, a); np.save(BACK, np.stack([a.astype(np.float64), v.astype(np.float64)]))
    buf = struct.pack('<f', val); n = ctypes.c_size_t()
    for x in a: k32.WriteProcessMemory(h, ctypes.c_void_p(int(x)), buf, 4, ctypes.byref(n))
    print('written', len(a))

def restore():
    h = open_game(True); b = np.load(BACK); n = ctypes.c_size_t()
    for x, v in zip(b[0], b[1]):
        if not np.isnan(v): k32.WriteProcessMemory(h, ctypes.c_void_p(int(x)), struct.pack('<f', float(v)), 4, ctypes.byref(n))
    print('restored', b.shape[1])

if __name__ == '__main__':
    c = sys.argv[1]
    if c == 'snap': snap()
    elif c == 'keep': keep(float(sys.argv[2]), float(sys.argv[3]))
    elif c == 'show': show(int(sys.argv[2]) if len(sys.argv) > 2 else 40)
    elif c == 'set': setv(float(sys.argv[2]), sys.argv[3] if len(sys.argv) > 3 else None)
    elif c == 'restore': restore()
