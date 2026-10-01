"""Class names from vtables (MSVC RTTI, read from the exe file).
  python rtti.py <vtable rva hex> ...   or import: name(vtable_rva)"""
import struct, sys
import xref

def raw(rva, n):
    for nm, va, vs, rw, rs in xref.SECTIONS:
        if va <= rva < va + max(vs, rs):
            return xref.data[rw + rva - va: rw + rva - va + n]
    return None

def name(vt):
    b = raw(vt - 8, 8)
    if not b:
        return None
    col = struct.unpack('<Q', b)[0] - 0x140000000
    c = raw(col, 24)
    if not c:
        return None
    sig, off, cd, td = struct.unpack('<IIII', c[:16])
    t = raw(td + 0x10, 128)
    return t.split(b'\0')[0].decode(errors='replace') if t else None

if __name__ == '__main__':
    for a in sys.argv[1:]:
        print(a, name(int(a, 16)))
