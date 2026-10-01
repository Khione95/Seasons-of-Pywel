"""Reads Crimson Desert's memory from outside (research only).

  python memscan.py snap <name> [lo hi]   save every float in [lo, hi] (first snap)
                                          or re-read the kept places (later snaps)
  python memscan.py fit <slider=snap> ... keep places linear in the slider
  python memscan.py show [n]              print kept places
  python memscan.py read <addr hex> <count>
  python memscan.py write <addr hex> <float>
"""
import ctypes
import ctypes.wintypes as wt
import os
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
k32 = ctypes.WinDLL('kernel32', use_last_error=True)
MODULE_ONLY = os.environ.get('MODULE_ONLY') == '1'


class MBI(ctypes.Structure):
    _fields_ = [('BaseAddress', ctypes.c_void_p), ('AllocationBase', ctypes.c_void_p),
                ('AllocationProtect', wt.DWORD), ('PartitionId', wt.WORD), ('RegionSize', ctypes.c_size_t),
                ('State', wt.DWORD), ('Protect', wt.DWORD), ('Type', wt.DWORD)]


def pid():
    out = subprocess.check_output(['tasklist', '/FI', 'IMAGENAME eq CrimsonDesert.exe', '/FO', 'CSV', '/NH'], text=True)
    return int(out.split('","')[1])


def open_game(write=False):
    access = 0x0410 | (0x0028 if write else 0)   # QUERY_INFORMATION | VM_READ (| VM_OPERATION | VM_WRITE)
    h = k32.OpenProcess(access, False, pid())
    if not h:
        raise SystemExit(f'OpenProcess failed {ctypes.get_last_error()}')
    return h


def regions(h):
    addr, mbi = 0x10000, MBI()
    while k32.VirtualQueryEx(h, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)):
        base, size = mbi.BaseAddress or 0, mbi.RegionSize
        if mbi.State == 0x1000 and mbi.Protect in (0x04, 0x40) and size < (1 << 31) and                 (not MODULE_ONLY or 0x140000000 <= base < 0x150000000):
            yield base, size
        if base + size <= addr:
            break
        addr = base + size


def read(h, addr, size):
    buf = ctypes.create_string_buffer(size)
    n = ctypes.c_size_t()
    if not k32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, size, ctypes.byref(n)) or n.value != size:
        return None
    return buf.raw


def path(name):
    return os.path.join(HERE, f'snap_{name}.npz')


def kept_path():
    return os.path.join(HERE, 'kept.npy')


def snap(name, lo=None, hi=None):
    h = open_game()
    if lo is not None:
        addrs, vals = [], []
        for base, size in regions(h):
            data = read(h, base, size)
            if data is None:
                continue
            f = np.frombuffer(data, dtype=np.float32, count=size // 4)
            idx = np.nonzero((f >= lo) & (f <= hi) & (f != 0))[0]
            addrs.append(base + idx.astype(np.int64) * 4)
            vals.append(f[idx])
        a, v = np.concatenate(addrs), np.concatenate(vals)
        np.save(kept_path(), a)
    else:
        a = np.load(kept_path())
        v = np.full(len(a), np.nan, dtype=np.float32)
        # Read by region of the kept places.
        order = np.argsort(a)
        a = a[order]
        v = v[order]
        for base, size in regions(h):
            i0, i1 = np.searchsorted(a, base), np.searchsorted(a, base + size)
            if i0 == i1:
                continue
            data = read(h, base, size)
            if data is None:
                continue
            f = np.frombuffer(data, dtype=np.float32, count=size // 4)
            v[i0:i1] = f[(a[i0:i1] - base) // 4]
        np.save(kept_path(), a)
    np.savez(path(name), a=a, v=v)
    print(f'{name}: {len(a)} places')


def fit(pairs):
    a = np.load(kept_path())
    xs, cols = [], []
    for p in pairs:
        s, name = p.split('=')
        d = np.load(path(name))
        m = dict(zip(d['a'].tolist(), d['v'].tolist())) if len(d['a']) != len(a) else None
        vals = d['v'] if m is None else np.array([m.get(x, np.nan) for x in a.tolist()], dtype=np.float32)
        xs.append(float(s))
        cols.append(vals)
    Y = np.stack(cols, axis=1).astype(np.float64)
    x = np.array(xs)
    ok = np.all(np.isfinite(Y), axis=1)
    # Least squares line per place; keep exact fits that change.
    A = np.vstack([x, np.ones_like(x)]).T
    coef, *_ = np.linalg.lstsq(A, Y[ok].T, rcond=None)
    pred = (A @ coef).T
    err = np.max(np.abs(pred - Y[ok]), axis=1)
    slope = coef[0]
    good = (err < 1e-4 + np.abs(slope) * 1e-3) & (np.abs(slope) > 1e-6)
    keep = a[ok][good]
    np.save(kept_path(), keep)
    print(f'{len(keep)} places linear in the slider (of {ok.sum()})')
    for addr, s, c in list(zip(keep, slope[good], coef[1][good]))[:60]:
        print(f'  {addr:016X}  value = {c:.6g} + {s:.6g} * slider')


def show(n=60):
    a = np.load(kept_path())
    h = open_game()
    for addr in a[:n]:
        data = read(h, int(addr), 4)
        print(f'{addr:016X}  {np.frombuffer(data, np.float32)[0] if data else None}')
    print(len(a), 'places')


def main():
    cmd = sys.argv[1]
    if cmd == 'snap':
        snap(sys.argv[2], *(float(x) for x in sys.argv[3:5])) if len(sys.argv) > 3 else snap(sys.argv[2])
    elif cmd == 'fit':
        fit(sys.argv[2:])
    elif cmd == 'show':
        show(int(sys.argv[2]) if len(sys.argv) > 2 else 60)
    elif cmd == 'read':
        h = open_game()
        data = read(h, int(sys.argv[2], 16), 4 * int(sys.argv[3]))
        print(np.frombuffer(data, np.float32))
    elif cmd == 'write':
        h = open_game(True)
        n = ctypes.c_size_t()
        k32.WriteProcessMemory(h, ctypes.c_void_p(int(sys.argv[2], 16)), ctypes.byref(ctypes.c_float(float(sys.argv[3]))), 4, ctypes.byref(n))
        print('written', n.value)


if __name__ == '__main__':
    main()


def class_name(h, obj):
    """MSVC RTTI name of the object's class."""
    import struct as _s
    vt = _s.unpack('<Q', read(h, obj, 8))[0]
    col = _s.unpack('<Q', read(h, vt - 8, 8))[0]
    sig, off, cdoff, td_rva, chd_rva, self_rva = _s.unpack('<IIIiii', read(h, col, 24))
    base = col - self_rva
    name = read(h, base + td_rva + 16, 128).split(b'\0')[0]
    return name.decode(errors='replace'), vt - 0x140000000
