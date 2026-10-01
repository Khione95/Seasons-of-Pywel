"""Find live objects of a class: vtable rva from its RTTI name, then scan memory
for pointers to it.  python vtfind.py <ClassName> [dump bytes]"""
import re, struct, sys, numpy as np
import xref, memscan as ms


def vtables(cls):
    d = xref.data; out = []
    for mm in re.finditer(re.escape(b'.?AV' + cls.encode() + b'@'), d):
        td = xref.rva_of(mm.start() - 16)
        for m in re.finditer(re.escape(struct.pack('<i', td)), d):
            o = m.start() - 12
            sig, of, cd, t, chd, selfr = struct.unpack_from('<IIIiii', d, o)
            if sig == 1 and t == td and xref.rva_of(o) == selfr:
                for p in re.finditer(re.escape(struct.pack('<Q', 0x140000000 + selfr)), d):
                    out.append(xref.rva_of(p.start() + 8))
    return out


def objects(h, vt):
    target = 0x140000000 + vt; found = []
    for base, size in ms.regions(h):
        dd = ms.read(h, base, size)
        if not dd:
            continue
        a = np.frombuffer(dd[:len(dd) // 8 * 8], np.uint64)
        found += [base + int(i) * 8 for i in np.nonzero(a == target)[0]]
    return found


if __name__ == '__main__':
    h = ms.open_game()
    n = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x100
    for vt in vtables(sys.argv[1]):
        objs = objects(h, vt)
        print(sys.argv[1], 'vtable', hex(vt), len(objs), 'objects', [hex(o) for o in objs[:10]])
        for o in objs[:3]:
            dd = ms.read(h, o, n)
            for i in range(0, n, 32):
                print('  +%03X' % i, dd[i:i + 32].hex(' ', 4), np.frombuffer(dd[i:i + 32], np.float32).round(3).tolist())
