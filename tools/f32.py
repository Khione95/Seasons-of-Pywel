"""Read constants: python f32.py <rva hex> ... (float and u32 at each rva)."""
import struct, sys
import xref

def raw(rva, n=4):
    for name, va, vs, rw, rs in xref.SECTIONS:
        if va <= rva < va + max(vs, rs):
            return xref.data[rw + rva - va: rw + rva - va + n]

def f32(rva):
    return struct.unpack('<f', raw(rva))[0]

if __name__ == '__main__':
    for a in sys.argv[1:]:
        r = int(a, 16)
        print(f'{r:X}: {f32(r)}  ({raw(r).hex()})')
