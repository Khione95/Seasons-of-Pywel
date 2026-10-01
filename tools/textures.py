"""Reads the game's textures (.dds), including "Partial" ones: the 128-byte
DDS header is stored plain, the pixels follow LZ4 packed, with the packed size
at +0x20 of the header.

  python textures.py get <regex> <out folder>   unpacked .dds files (+ .png preview)
"""
import os, struct, sys
import lz4.block
import game_files as g


def unpack(entry):
    with open(os.path.join(g.GAME, entry['archive'], f"{entry['paz']}.paz"), 'rb') as f:
        f.seek(entry['offset'])
        d = f.read(entry['packed'])
    if entry['compression'] != 'Partial':
        return g.read(entry)
    head = bytearray(d[:128 + (20 if d[84:88] == b'DX10' else 0)])
    body_size = entry['size'] - len(head)
    stored = [x for x in struct.unpack_from('<4I', d, 0x20) if x]
    # The first mips' stored sizes: packed (LZ4) when smaller than the mip.
    fourcc = d[84:88]
    h, w = struct.unpack_from('<II', d, 12)
    mips = max(1, struct.unpack_from('<I', d, 28)[0])
    bpb = {b'DXT1': 8, b'DXT5': 16, b'DXT3': 16}.get(fourcc)
    bpp = None
    if fourcc == b'DX10':
        dxgi = struct.unpack_from('<I', d, 128)[0]
        bc = {70: 8, 71: 8, 72: 8, 73: 16, 74: 16, 75: 16, 76: 16, 77: 16, 78: 16, 79: 8, 80: 8, 81: 8,
              82: 16, 83: 16, 84: 16, 94: 16, 95: 16, 96: 16, 97: 16, 98: 16, 99: 16}
        px = {61: 1, 62: 1, 63: 1, 64: 1, 65: 1, 49: 2, 50: 2, 51: 2, 52: 2, 53: 2, 54: 2, 56: 2, 57: 2, 58: 2,
              24: 4, 25: 4, 26: 4, 27: 4, 28: 4, 29: 4, 30: 4, 31: 4, 32: 4, 41: 4, 42: 4, 43: 4, 87: 4, 88: 4,
              10: 8, 11: 8, 2: 16}
        bpb, bpp = bc.get(dxgi), px.get(dxgi)
    if not bpb and not bpp and not any(fourcc):
        bpp = struct.unpack_from('<I', d, 88)[0] // 8     # plain (RGB / luminance) pixels
    def mip_size(i):
        mw, mh = max(1, w >> i), max(1, h >> i)
        if bpb:
            return max(1, (mw + 3) // 4) * max(1, (mh + 3) // 4) * bpb
        if bpp:
            return mw * mh * bpp
        return None
    pos = len(head)
    body = bytearray()
    if mips == 1 and len(stored) == 2 and stored[1] == body_size:
        packed, size = stored
        body = lz4.block.decompress(d[pos:pos + packed], uncompressed_size=size) if packed < size else d[pos:pos + size]
    else:
        for i, st in enumerate(stored):
            raw = mip_size(i)
            if raw is None:
                raise ValueError(f"{entry['path']}: unknown format for {mips} mips")
            chunk = d[pos:pos + st]
            body += lz4.block.decompress(chunk, uncompressed_size=raw) if st < raw else chunk
            pos += st
        body += d[pos:]
    # The game reads these as the stored sizes of the first mips: stored plain
    # now, so each is its mip's full size (clearing them breaks the texture).
    if mips == 1 and len(stored) == 2:
        struct.pack_into('<II', head, 0x20, stored[1], stored[1])
    else:
        for i in range(len(stored)):
            struct.pack_into('<I', head, 0x20 + 4 * i, mip_size(i))
    return bytes(head) + body


def main():
    if sys.argv[1] == 'get':
        from PIL import Image
        out = sys.argv[3]
        for e in g.entries(sys.argv[2]):
            try:
                data = unpack(e)
            except Exception as ex:
                print(ex)
                continue
            path = os.path.join(out, os.path.basename(e['path']))
            os.makedirs(out, exist_ok=True)
            open(path, 'wb').write(data)
            try:
                Image.open(path).convert('RGBA').save(path[:-4] + '.png')
                note = 'png ok'
            except Exception as ex:
                note = f'no preview ({ex})'
            print(e['path'], len(data), note)


if __name__ == '__main__':
    main()
