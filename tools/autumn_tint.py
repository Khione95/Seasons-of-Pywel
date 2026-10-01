"""Season colours (autumn, or spring with SEASON=spring) for block-compressed (DXT1/DXT5) textures: every block's two
colour endpoints (RGB565) are moved from green towards yellow / orange / rust,
keeping their brightness; the block's indices stay, so the texture's detail and
format stay the same. All mips.
Usage: python autumn_tint.py <in.dds> <out.dds> [strength 0-1]"""
import os, struct, sys
import numpy as np

# Which season the tools make: autumn (default) or spring (SEASON=spring).
SEASON = os.environ.get('SEASON', 'autumn')

# Target hues (degrees) and boosts, tuned in game.
HUE_LOW, HUE_HIGH = 8.0, 42.0
SATURATE, BRIGHTEN = 0.8, 0.18
# Spring: greens pulled towards fresh, light yellow-greens.
SPRING_LOW, SPRING_HIGH, SPRING_PULL = 75.0, 100.0, 0.9
SPRING_SATURATE, SPRING_BRIGHTEN = 0.3, 0.3


def rgb565_to_rgb(c):
    r = ((c >> 11) & 31) * 255 / 31
    g = ((c >> 5) & 63) * 255 / 63
    b = (c & 31) * 255 / 31
    return np.stack([r, g, b], -1)


def rgb_to_565(rgb):
    r = np.clip(np.round(rgb[..., 0] * 31 / 255), 0, 31).astype(np.uint16)
    g = np.clip(np.round(rgb[..., 1] * 63 / 255), 0, 63).astype(np.uint16)
    b = np.clip(np.round(rgb[..., 2] * 31 / 255), 0, 31).astype(np.uint16)
    return (r << 11) | (g << 5) | b


def autumn(rgb, strength):
    r, g, b = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    mx = rgb.max(-1); mn = rgb.min(-1); d = mx - mn + 1e-6
    h = np.where(mx == r, ((g - b) / d) % 6, np.where(mx == g, (b - r) / d + 2, (r - g) / d + 4)) * 60
    s = np.where(mx > 0, d / (mx + 1e-6), 0); v = mx
    green = (h > 55) & (h < 175) & (s > 0.08)
    # greens (hue 55-175) spread over rust -> orange -> gold; yellow-greens become gold,
    # deep greens orange/red. More saturated and a little brighter, so it shows
    # through the plants' own textures.
    t = np.clip((h - 55) / 120, 0, 1)
    if SEASON == 'spring':
        nh = np.where(green, h + (SPRING_LOW + (SPRING_HIGH - SPRING_LOW) * t - h) * SPRING_PULL * strength, h)
        ns = np.where(green, np.clip(s * (1 + SPRING_SATURATE * strength), 0, 1), s)
        nv = np.where(green, np.clip(v * (1 + SPRING_BRIGHTEN * strength), 0, 255), v)
    else:
        nh = np.where(green, h + (HUE_LOW + (HUE_HIGH - HUE_LOW) * (1 - t) - h) * strength, h)
        ns = np.where(green, np.clip(s * (1 + SATURATE * strength), 0, 1), s)
        nv = np.where(green, np.clip(v * (1 + BRIGHTEN * strength), 0, 255), v)
    c = nv * ns; x = c * (1 - np.abs((nh / 60) % 2 - 1)); m = nv - c
    i = (nh // 60).astype(int) % 6
    rr = np.choose(i, [c, x, 0 * c, 0 * c, x, c]); gg = np.choose(i, [x, c, c, x, 0 * c, 0 * c]); bb = np.choose(i, [0 * c, 0 * c, x, c, c, x])
    return np.clip(np.stack([rr + m, gg + m, bb + m], -1), 0, 255)


def tint(data, strength=1.0):
    """The .dds bytes with autumn colours."""
    d = bytearray(data)
    fourcc = bytes(d[84:88])
    block, off = {b'DXT1': (8, 0), b'DXT5': (16, 8)}[fourcc]
    n = (len(d) - 128) // block
    a = np.frombuffer(d, np.uint8, offset=128, count=n * block).reshape(n, block).copy()
    col = a[:, off:off + 8].view(np.uint16).reshape(n, 4)          # c0, c1, idx lo, idx hi
    c0, c1 = col[:, 0].copy(), col[:, 1].copy()
    n0 = rgb_to_565(autumn(rgb565_to_rgb(c0.astype(np.int64)), strength))
    n1 = rgb_to_565(autumn(rgb565_to_rgb(c1.astype(np.int64)), strength))
    if fourcc == b'DXT1':
        # keep each block's mode (c0 > c1: 4 colours); swap endpoints and indices when it would flip
        flip = ((c0 > c1) != (n0 > n1)) & (n0 != n1)
        idx = col[:, 2].astype(np.uint32) | (col[:, 3].astype(np.uint32) << 16)
        four = c0 > c1
        swapped = np.zeros_like(idx)
        for i in range(16):
            v = (idx >> (2 * i)) & 3
            nv = np.where(four, np.array([1, 0, 3, 2])[v], np.array([1, 0, 2, 3])[v])
            swapped |= nv.astype(np.uint32) << (2 * i)
        n0, n1 = np.where(flip, n1, n0), np.where(flip, n0, n1)
        idx = np.where(flip, swapped, idx)
        col[:, 2] = (idx & 0xFFFF).astype(np.uint16); col[:, 3] = (idx >> 16).astype(np.uint16)
    col[:, 0] = n0; col[:, 1] = n1
    a[:, off:off + 8] = col.view(np.uint8).reshape(n, 8)
    d[128:128 + n * block] = a.tobytes()
    return bytes(d)


def main():
    src, dst = sys.argv[1], sys.argv[2]
    strength = float(sys.argv[3]) if len(sys.argv) > 3 else 1.0
    open(dst, 'wb').write(tint(open(src, 'rb').read(), strength))
    print('wrote', dst)


if __name__ == '__main__':
    main()
