"""Research: dump an object's memory as floats / u32 (read only).
  python dumpobj.py <addr hex> <bytes hex> [out.bin]"""
import struct, sys
import memscan as ms

h = ms.open_game()
addr, n = int(sys.argv[1], 16), int(sys.argv[2], 16)
d = ms.read(h, addr, n)
if len(sys.argv) > 3:
    open(sys.argv[3], 'wb').write(d)
for off in range(0, len(d), 16):
    v = struct.unpack_from('<4f', d, off)
    u = struct.unpack_from('<4I', d, off)
    print(f'+{off:04X}', ' '.join(f'{x:11.4g}' for x in v), ' ', ' '.join(f'{x:08X}' for x in u))
