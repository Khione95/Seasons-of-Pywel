"""The season data for the Seasons ASI: every season's textures packed as the
game packs its own ("partial": the DDS header plain, the first four mips LZ4
packed where that is smaller and they are over 128 KB, their stored sizes at
+0x20 of the header), one data file per archive group, and an index the ASI
reads.

The game keeps a loaded texture's mip sizes: a season change must not change
them. So each texture's versions (summer - the game's own -, spring, autumn)
are packed together, every packed mip grown to the same stored size in all of
them (tools/lz4pad.py), and summer is kept too.

  <out>/<group>.dat     the packed files of all seasons, back to back
  <out>/seasons.idx     one line per file:
      season|group|path|offset|stored size|size|flags|s0|s1|s2|s3

Takes the per-season builds in build/release/Seasons - <Season>.
Usage: python build_season_data.py <out folder>"""
import os, struct, sys
import lz4.block

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import textures, game_files as g
from lz4pad import grow, longest_literals, split_matches

MAX_LITERALS = 2048     # the game's own blocks stay under about 2 KB of literals in a row

RELEASE = os.path.join(HERE, '..', 'build', 'release')
ALIGN = 16


def mip_sizes(d):
    """Raw size of each mip of a plain .dds."""
    h, w = struct.unpack_from('<II', d, 12)
    mips = max(1, struct.unpack_from('<I', d, 28)[0])
    fourcc = d[84:88]
    bpb = {b'DXT1': 8, b'DXT5': 16, b'DXT3': 16}.get(fourcc)
    bpp = None
    if fourcc == b'DX10':
        dxgi = struct.unpack_from('<I', d, 128)[0]
        bpb = {70: 8, 71: 8, 72: 8, 79: 8, 80: 8, 81: 8}.get(dxgi) or \
              ({73: 16, 74: 16, 75: 16, 76: 16, 77: 16, 78: 16, 82: 16, 83: 16, 84: 16, 94: 16, 95: 16,
                96: 16, 97: 16, 98: 16, 99: 16}.get(dxgi))
        bpp = None if bpb else {28: 4, 87: 4, 88: 4, 61: 1, 10: 8, 2: 16}.get(dxgi)
    if not bpb and not bpp and not any(fourcc):
        bpp = struct.unpack_from('<I', d, 88)[0] // 8
    out = []
    for i in range(mips):
        mw, mh = max(1, w >> i), max(1, h >> i)
        out.append(max(1, (mw + 3) // 4) * max(1, (mh + 3) // 4) * bpb if bpb else mw * mh * bpp)
    return out


def pack(d):
    """(stored bytes, stored sizes of the first four mips) of a plain .dds."""
    packed, stored = pack_variants([d])
    return packed[0], stored


def pack_variants(ds):
    """The versions of one texture (same size and format), packed so that every
    packed mip has the same stored size in all of them."""
    sizes = mip_sizes(ds[0])
    head_len = 128 + (20 if ds[0][84:88] == b'DX10' else 0)
    heads = [bytearray(d[:head_len]) for d in ds]
    outs = [bytearray() for _ in ds]
    stored = []
    pos = head_len
    for i, raw in enumerate(sizes):
        chunks = [d[pos:pos + raw] for d in ds]
        pos += raw
        # The game packs only mips bigger than 128 KB (the rest it reads raw).
        if i < 4 and raw > 131072:
            blocks = [split_matches(lz4.block.compress(c, store_size=False), c) for c in chunks]
            if all(len(b) < raw and longest_literals(b) <= MAX_LITERALS for b in blocks):
                target = max(len(b) for b in blocks)
                try:
                    grown = [grow(b, c, target, MAX_LITERALS) for b, c in zip(blocks, chunks)]
                except ValueError:
                    grown = None        # stored unpacked in all of them instead
                if grown:
                    for o, b in zip(outs, grown):
                        o += b
                    stored.append(target)
                    continue
        for o, c in zip(outs, chunks):
            o += c
        if i < 4:
            stored.append(raw)
    for o, d in zip(outs, ds):
        o += d[pos:]
    stored += [0] * (4 - len(stored))
    for h in heads:
        struct.pack_into('<4I', h, 0x20, *stored)
    return [bytes(h) + bytes(o) for h, o in zip(heads, outs)], stored


def check(original, packed_bytes):
    """Read back the way the game (textures.unpack) reads it."""
    import tempfile
    e = {'archive': 'x', 'paz': 0, 'offset': 0, 'packed': len(packed_bytes), 'size': len(original),
         'compression': 'Partial', 'path': 'check'}
    fd, path = tempfile.mkstemp()
    os.write(fd, packed_bytes); os.close(fd)
    old = g.GAME
    try:
        g.GAME = os.path.dirname(path)
        os.makedirs(os.path.join(g.GAME, 'x'), exist_ok=True)
        dst = os.path.join(g.GAME, 'x', '0.paz')
        os.replace(path, dst)
        back = textures.unpack(e)
    finally:
        g.GAME = old
        os.remove(dst)
    return back[128:] == original[128:]


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)

    # Every texture the seasons change, and its versions.
    variants = {}
    for season in ('Spring', 'Summer', 'Autumn', 'Winter'):
        src = os.path.join(RELEASE, 'Seasons - ' + season)
        for root, _, files in os.walk(src):
            for f in files:
                if f.endswith('.dds'):
                    rel = os.path.relpath(os.path.join(root, f), src).replace(os.sep, '/')
                    group, path = rel.split('/', 1)
                    variants.setdefault((group, path), {})[season.lower()] = os.path.join(root, f)
    wanted = {path for _, path in variants}
    entries = {}
    for e in g.entries(r'[.]dds$'):
        if e['path'] in wanted:
            entries[(e['archive'], e['path'])] = e

    dats, lines, checked, n = {}, [], 0, 0
    for (group, path), files in sorted(variants.items()):
        seasons = sorted(files)
        ds = [open(files[s], 'rb').read() for s in seasons]
        # Summer: the game's own texture, packed with the others (unless the
        # seasons bring their own summer, as for the climate texture).
        e = entries.get((group, path))
        if e and e['compression'] in ('Partial', 'None') and e['encryption'] == 'None' and 'summer' not in files and 'winter' not in files:
            summer = textures.unpack(e) if e['compression'] == 'Partial' else g.read(e)
            if mip_sizes(summer) == mip_sizes(ds[0]):
                seasons.append('summer')
                ds.append(summer)
        # The climate texture is read once at the start and changed on the
        # graphics card afterwards: its versions need no common sizes.
        if group != '0002' and all(mip_sizes(d) == mip_sizes(ds[0]) and d[84:88] == ds[0][84:88] for d in ds):
            packed, stored = pack_variants(ds)
            stored_each = [stored] * len(ds)
        else:
            packed, stored_each = [], []
            for d in ds:
                p, st = pack(d)
                packed.append(p)
                stored_each.append(st)
        if group not in dats:
            dats[group] = open(os.path.join(out, group + '.dat'), 'wb')
        dat = dats[group]
        for season, d, p, st in zip(seasons, ds, packed, stored_each):
            if checked < 60:
                assert check(d, p), (group, path, season)
                checked += 1
            dat.write(bytes(-dat.tell() % ALIGN))
            lines.append('|'.join(map(str, [season, group, path, dat.tell(), len(p), len(d), 1] + list(st))))
            dat.write(p)
        n += 1
        if n % 500 == 0:
            print(n, 'textures', flush=True)
    for dat in dats.values():
        dat.close()
    with open(os.path.join(out, 'seasons.idx'), 'w', newline='') as f:
        f.write(''.join(line + chr(10) for line in lines))

    # Every file must be where the index says.
    for line in lines:
        season, group, path, offset = line.split('|')[:4]
        with open(os.path.join(out, group + '.dat'), 'rb') as f:
            f.seek(int(offset))
            assert f.read(4) == b'DDS ', f'{group} {path}: not at {offset}'
    for group in dats:
        print(group, os.path.getsize(os.path.join(out, group + '.dat')) // 1000000, 'MB')
    print(len(lines), 'files')


if __name__ == '__main__':
    main()
