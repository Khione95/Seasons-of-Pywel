"""Vtables of a class by its RTTI name (MSVC x64).
  python vtof.py <substring of .?AV name> ..."""
import re, struct, sys
import xref

data = xref.data

def file_off(rva):
    for nm, va, vs, rw, rs in xref.SECTIONS:
        if va <= rva < va + max(vs, rs):
            return rw + rva - va
    return None

def vtables(name):
    out = []
    for m in re.finditer(re.escape(name.encode()), data):
        s = data.rfind(b'.?AV', 0, m.start() + 1)
        if s < 0 or m.start() - s > 200:
            continue
        full = data[s:data.index(b'\0', s)].decode(errors='replace')
        td = xref.rva_of(s) - 0x10
        # complete object locators: sig 1, offset, cd offset, td rva, chd rva, self rva
        for c in re.finditer(re.escape(struct.pack('<I', td)), data):
            co = c.start() - 12
            sig, off, cd, tdr, chd, selfr = struct.unpack_from('<6I', data, co)
            if sig != 1 or xref.rva_of(co) != selfr:
                continue
            col = 0x140000000 + selfr
            for v in re.finditer(re.escape(struct.pack('<Q', col)), data):
                out.append((full, off, xref.rva_of(v.start()) + 8))
    return out

if __name__ == '__main__':
    for n in sys.argv[1:]:
        for full, off, vt in vtables(n):
            print(full, f'offset {off:X}', f'vtable {vt:X}')
