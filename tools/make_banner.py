"""The mod's image: four screenshots of one view (spring, summer, autumn,
winter) cut into slanted slices of one picture, the title above.

  python make_banner.py <spring> <summer> <autumn> <winter> <out.png> [width]"""
import sys
from PIL import Image, ImageDraw, ImageFilter, ImageFont, ImageChops

FONTS = r'C:\Windows\Fonts'
TITLE = 'Seasons of Pywel'
SUBTITLE = 'A LIVING YEAR FOR CRIMSON DESERT'
LABELS = ['SPRING', 'SUMMER', 'AUTUMN', 'WINTER']
TINTS = [(196, 236, 150), (255, 222, 140), (255, 168, 84), (200, 232, 255)]
SLANT = 0.16            # the slices lean: x moves this much per pixel of height


def load(path, size):
    im = Image.open(path).convert('RGB')
    return im.resize(size, Image.LANCZOS) if im.size != size else im


def spaced(draw, xy, text, font, spacing, fill, anchor_center=True):
    """Text with letter spacing, centred on xy."""
    widths = [draw.textlength(c, font=font) for c in text]
    total = sum(widths) + spacing * (len(text) - 1)
    x = xy[0] - total / 2 if anchor_center else xy[0]
    for c, w in zip(text, widths):
        draw.text((x, xy[1]), c, font=font, fill=fill)
        x += w + spacing
    return total


def text_layer(size, draw_fn):
    layer = Image.new('L', size, 0)
    draw_fn(ImageDraw.Draw(layer))
    return layer


def main():
    paths, out = sys.argv[1:5], sys.argv[5]
    first = Image.open(paths[0])
    width = int(sys.argv[6]) if len(sys.argv) > 6 else first.width
    height = round(width * first.height / first.width)
    size = (width, height)
    shots = [load(p, size) for p in paths]
    s = width / 1500.0

    # The slices: four bands across, their edges leaning.
    canvas = shots[0].copy()
    edges = [width * k / 4 for k in range(5)]
    for k in range(1, 4):
        mask = Image.new('L', size, 0)
        d = ImageDraw.Draw(mask)
        top, bottom = -SLANT * height / 2, SLANT * height / 2
        d.polygon([(edges[k] + top, 0), (width + 400, 0), (width + 400, height), (edges[k] + bottom, height)], fill=255)
        canvas.paste(shots[k], (0, 0), mask)

    # Light seams between the seasons, with a soft glow.
    seams = Image.new('L', size, 0)
    d = ImageDraw.Draw(seams)
    for k in range(1, 4):
        d.line([(edges[k] - SLANT * height / 2, 0), (edges[k] + SLANT * height / 2, height)], fill=255, width=max(2, round(2 * s)))
    glow = seams.filter(ImageFilter.GaussianBlur(6 * s))
    white = Image.new('RGB', size, (255, 248, 232))
    canvas = Image.composite(white, canvas, ImageChops.lighter(seams.point(lambda v: v * 0.85), glow.point(lambda v: min(255, v * 1.6))))

    # Shade at the top (the title) and at the bottom (the seasons' names).
    shade = Image.new('L', size, 0)
    px = shade.load()
    for y in range(height):
        t = y / height
        a = 0
        if t < 0.42:
            a = 175 * (1 - t / 0.42) ** 1.6
        if t > 0.74:
            a = max(a, 165 * ((t - 0.74) / 0.26) ** 1.4)
        for x in range(width):
            px[x, y] = int(a)
    canvas = Image.composite(Image.new('RGB', size, (8, 6, 10)), canvas, shade)

    # The title: warm gold to pale, a dark halo and a soft glow behind.
    title_font = ImageFont.truetype(FONTS + r'\palab.ttf', round(118 * s))
    sub_font = ImageFont.truetype(FONTS + r'\pala.ttf', round(25 * s))
    label_font = ImageFont.truetype(FONTS + r'\palab.ttf', round(31 * s))
    cx = width / 2
    ty = round(58 * s)

    title_mask = text_layer(size, lambda d: spaced(d, (cx, ty), TITLE, title_font, round(3 * s), 255))
    bbox = title_mask.getbbox()
    halo = title_mask.filter(ImageFilter.MaxFilter(5)).filter(ImageFilter.GaussianBlur(10 * s))
    canvas = Image.composite(Image.new('RGB', size, (0, 0, 0)), canvas, halo.point(lambda v: min(255, int(v * 1.25))))
    warm = title_mask.filter(ImageFilter.GaussianBlur(26 * s))
    canvas = Image.composite(Image.new('RGB', size, (255, 196, 110)), canvas, warm.point(lambda v: int(v * 0.35)))

    fill = Image.new('RGB', size)
    fd = ImageDraw.Draw(fill)
    y0, y1 = bbox[1], bbox[3]
    for y in range(height):
        t = 0 if y <= y0 else 1 if y >= y1 else (y - y0) / (y1 - y0)
        top_c, bottom_c = (255, 246, 222), (232, 172, 82)
        fd.line([(0, y), (width, y)], fill=tuple(int(a + (b - a) * t) for a, b in zip(top_c, bottom_c)))
    edge = title_mask.filter(ImageFilter.MaxFilter(3))
    canvas = Image.composite(Image.new('RGB', size, (54, 30, 12)), canvas, edge)
    canvas = Image.composite(fill, canvas, title_mask)

    # A thin rule with the subtitle under the title.
    d = ImageDraw.Draw(canvas)
    sub_y = bbox[3] + round(20 * s)
    sub_mask = text_layer(size, lambda dd: spaced(dd, (cx, sub_y), SUBTITLE, sub_font, round(6 * s), 255))
    sb = sub_mask.getbbox()
    canvas = Image.composite(Image.new('RGB', size, (0, 0, 0)), canvas, sub_mask.filter(ImageFilter.GaussianBlur(4 * s)))
    canvas = Image.composite(Image.new('RGB', size, (238, 226, 204)), canvas, sub_mask)
    d = ImageDraw.Draw(canvas)
    mid = (sb[1] + sb[3]) / 2
    gap, reach = round(22 * s), round(150 * s)
    for side in (-1, 1):
        x_in = sb[0] - gap if side < 0 else sb[2] + gap
        x_out = x_in + side * reach
        for i in range(reach):
            a = 1 - i / reach
            x = x_in + side * i
            d.point((x, mid), fill=tuple(int(c * a + 30 * (1 - a)) for c in (232, 190, 120)))
        d.ellipse([x_in - 3 * s * side - 3 * s, mid - 3 * s, x_in - 3 * s * side + 3 * s, mid + 3 * s], fill=(240, 200, 130))

    # The seasons' names under their slices.
    label_y = height - round(66 * s)
    for k, (name, tint) in enumerate(zip(LABELS, TINTS)):
        centre = (edges[k] + edges[k + 1]) / 2 + SLANT * (label_y + round(16 * s) - height / 2)
        if k == 0:
            centre += round(10 * s)
        if k == 3:
            centre -= round(10 * s)
        m = text_layer(size, lambda dd: spaced(dd, (centre, label_y), name, label_font, round(7 * s), 255))
        canvas = Image.composite(Image.new('RGB', size, (0, 0, 0)), canvas, m.filter(ImageFilter.MaxFilter(3)).filter(ImageFilter.GaussianBlur(5 * s)))
        canvas = Image.composite(Image.new('RGB', size, tint), canvas, m.filter(ImageFilter.GaussianBlur(9 * s)).point(lambda v: int(v * 0.45)))
        canvas = Image.composite(Image.new('RGB', size, tint), canvas, m)

    canvas.save(out, quality=95)
    print(out, canvas.size)


if __name__ == '__main__':
    main()
