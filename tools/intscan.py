"""Find int32 values in the game's memory and narrow them down.
  python intscan.py first <value>     all places holding value (saved)
  python intscan.py next <value>      keep places now holding value
  python intscan.py show [n]          print places with a few neighbours"""
import os, sys, numpy as np
import memscan as ms

KEPT = os.path.join(ms.HERE, '..', 'research', 're', 'intscan.npy')


def first(v):
    h = ms.open_game(); found = []
    for base, size in ms.regions(h):
        data = ms.read(h, base, size)
        if not data:
            continue
        a = np.frombuffer(data[:len(data) // 4 * 4], np.int32)
        idx = np.nonzero(a == v)[0]
        found.append(base + idx.astype(np.int64) * 4)
    found = np.concatenate(found) if found else np.zeros(0, np.int64)
    np.save(KEPT, found); print(len(found), 'places')


def nxt(v):
    h = ms.open_game(); found = np.load(KEPT); keep = []
    for a in found:
        d = ms.read(h, int(a), 4)
        if d and int.from_bytes(d, 'little', signed=True) == v:
            keep.append(a)
    keep = np.array(keep, np.int64); np.save(KEPT, keep); print(len(keep), 'places')


def show(n=40):
    h = ms.open_game()
    for a in np.load(KEPT)[:n]:
        d = ms.read(h, int(a) - 16, 48)
        print(hex(int(a)), d and np.frombuffer(d, np.int32).tolist())


if __name__ == '__main__':
    {'first': lambda: first(int(sys.argv[2])), 'next': lambda: nxt(int(sys.argv[2])),
     'show': lambda: show(int(sys.argv[2]) if len(sys.argv) > 2 else 40)}[sys.argv[1]]()
