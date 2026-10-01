"""Reads the reflection schema at the start of .palevel / .prefab files:
per class: u32 name length, name, u16 field count, then per field
u32 name length, name, u32 type length, type, u16 kind, u16 size, u32 offset."""
import struct, sys


def s(d, p):
    n = struct.unpack_from('<I', d, p)[0]
    return d[p + 4:p + 4 + n].decode('latin-1'), p + 4 + n


def find_class(d, name):
    key = struct.pack('<I', len(name)) + name.encode()
    p = d.find(key)
    if p < 0:
        return None
    _, p = s(d, p)
    count = struct.unpack_from('<H', d, p)[0]
    p += 2
    fields = []
    for _ in range(count):
        fname, p = s(d, p)
        ftype, p = s(d, p)
        kind, size, offset = struct.unpack_from('<HHI', d, p)
        p += 8
        fields.append((fname, ftype, kind, size, offset))
    return fields, p


if __name__ == '__main__':
    d = open(sys.argv[1], 'rb').read()
    for cls in sys.argv[2:]:
        r = find_class(d, cls)
        if not r:
            print(cls, 'not found'); continue
        fields, end = r
        print(f'== {cls} ({len(fields)} fields, schema ends 0x{end:x})')
        for f in fields:
            print('  %-36s %-28s kind %d size %d off 0x%X' % f)
