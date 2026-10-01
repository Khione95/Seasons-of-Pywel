"""Research: objects whose first qword is a vtable rva (read only).
  python vtscan.py <vtable rva hex> [<more>]"""
import struct, sys
import numpy as np
import memscan as ms

BASE = 0x140000000
h = ms.open_game()
want = [BASE + int(a, 16) for a in sys.argv[1:]]
found = {w: [] for w in want}
for base, size in ms.regions(h):
    try:
        d = ms.read(h, base, size)
    except Exception:
        continue
    if not d or len(d) < 8:
        continue
    a = np.frombuffer(d[:len(d) // 8 * 8], np.uint64)
    for w in want:
        for i in np.nonzero(a == w)[0][:50]:
            found[w].append(base + int(i) * 8)
for w, l in found.items():
    print(f'{w - BASE:X}:', ' '.join(f'{x:X}' for x in l[:20]))
