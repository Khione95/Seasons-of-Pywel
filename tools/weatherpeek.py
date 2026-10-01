"""Research: the climate manager's state in the running game (read only).
  python weatherpeek.py"""
import struct, sys
import memscan as ms

BASE = 0x140000000
h = ms.open_game()

def q(a):
    return struct.unpack('<Q', ms.read(h, a, 8))[0]

def floats(a, n):
    return struct.unpack(f'<{n}f', ms.read(h, a, 4 * n))

root = q(BASE + 0x6D69198)
a = q(root + 0x40)
mgr = q(a + 0xF68)
print(f'root {root:X}  +40 {a:X}  climate manager {mgr:X}  vtable {q(mgr) - BASE:X}')
print('map +D8', hex(q(mgr + 0xD8)))
d = ms.read(h, mgr, 0x200)
for off in range(0, 0x200, 16):
    v = struct.unpack_from('<4f', d, off)
    u = struct.unpack_from('<4I', d, off)
    print(f'+{off:03X}', ' '.join(f'{x:12.4g}' for x in v), ' ', ' '.join(f'{x:08X}' for x in u))
print('tail +4001C0:', floats(mgr + 0x4001C0, 12))
