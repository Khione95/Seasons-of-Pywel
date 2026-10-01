"""Sets one colour channel of a DXT1 .dds (all mips) to a constant: the two
RGB565 endpoints of every 4x4 block get that channel, keeping the block's mode
(swapping endpoints and indices when their order would flip).
Usage: python dxt1_channel.py <in.dds> <out.dds> <R|G|B> <0-255>"""
import struct, sys

def main():
    src, dst, ch, val = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
    d = bytearray(open(src, 'rb').read())
    assert d[84:88] == b'DXT1'
    shift, bits = {'R': (11, 5), 'G': (5, 6), 'B': (0, 5)}[ch]
    q = round(val * ((1 << bits) - 1) / 255)
    mask = ((1 << bits) - 1) << shift
    changed = 0
    for p in range(128, len(d) - 7, 8):
        c0, c1, idx = struct.unpack_from('<HHI', d, p)
        n0, n1 = (c0 & ~mask) | (q << shift), (c1 & ~mask) | (q << shift)
        if (c0 > c1) != (n0 > n1) and n0 != n1:
            # keep the 4-colour / 3-colour mode: swap the endpoints, remap indices
            n0, n1 = n1, n0
            new = 0
            for i in range(16):
                v = (idx >> (2 * i)) & 3
                v = {0: 1, 1: 0, 2: 3, 3: 2}[v] if c0 > c1 else {0: 1, 1: 0, 2: 2, 3: 3}[v]
                new |= v << (2 * i)
            idx = new
        struct.pack_into('<HHI', d, p, n0, n1, idx)
        changed += 1
    open(dst, 'wb').write(d)
    print('blocks', changed, 'channel', ch, '=', val, '(565 value', q, ')')

main()
