"""Writes climate_texture_1 (512x512 B8G8R8A8) with a full mip chain, so DMM keeps
it as it is (DMM re-encodes single-mip textures, which garbles the climate zone
values). Lower mips take every second pixel (no averaging: no new zone values).
  python climate_dds.py <out.dds> [--except-desert] [--rect x0 x1 y0 y1] [B|G|R|A=value ...]"""
import struct, sys, os
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, '..', 'research', 'climate', 'climate_texture_1.dds')


def build(pixels):
    h, w, _ = pixels.shape
    mips = []
    p = pixels
    while True:
        mips.append(p)
        if p.shape[0] == 1 and p.shape[1] == 1:
            break
        p = p[::2, ::2] if p.shape[0] > 1 and p.shape[1] > 1 else p[::2] if p.shape[0] > 1 else p[:, ::2]
    head = bytearray(open(SRC, 'rb').read()[:128])
    flags = struct.unpack_from('<I', head, 8)[0] | 0x20000          # DDSD_MIPMAPCOUNT
    struct.pack_into('<I', head, 8, flags)
    struct.pack_into('<I', head, 28, len(mips))
    caps = struct.unpack_from('<I', head, 108)[0] | 0x400008        # COMPLEX | MIPMAP
    struct.pack_into('<I', head, 108, caps)
    sizes = [m.nbytes for m in mips]
    struct.pack_into('<4I', head, 0x20, *(sizes + [0, 0, 0, 0])[:4])  # stored plain
    return bytes(head) + b''.join(m.tobytes() for m in mips), len(mips)


def main():
    out = sys.argv[1]
    d = open(SRC, 'rb').read()
    px = np.frombuffer(d, np.uint8, offset=128, count=512 * 512 * 4).reshape(512, 512, 4).copy()
    mask = None
    args = sys.argv[2:]
    if args and args[0] == '--except-desert':
        # The desert keeps its own climate: its climate zones (B 0-120 and the white
        # desert 240-246) and whatever is reddish in the terrain colour map
        # (research/climate/desert_mask.npy, made in research).
        mask = ~np.load(os.path.join(HERE, '..', 'research', 'climate', 'desert_mask.npy'))
        args = args[1:]
    if args and args[0] == '--rect':
        # Locator: only inside the rectangle x0 x1 y0 y1 (texels).
        x0, x1, y0, y1 = (int(v) for v in args[1:5])
        rect = np.zeros((512, 512), bool)
        rect[y0:y1, x0:x1] = True
        mask = rect if mask is None else mask & rect
        args = args[5:]
    for arg in args:
        ch, val = arg.split('=')
        if mask is None:
            px[:, :, 'BGRA'.index(ch)] = int(val)
        else:
            px[:, :, 'BGRA'.index(ch)][mask] = int(val)
    data, n = build(px)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    open(out, 'wb').write(data)
    print('wrote', out, len(data), 'bytes,', n, 'mips')


main()
