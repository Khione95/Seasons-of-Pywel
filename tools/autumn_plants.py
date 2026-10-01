"""Autumn colours for the plants' own textures (tree/texture atlases and
tree/impostor distance images): deciduous leaves, bushes, grasses, weeds.
Evergreens, palms, desert / sea / abyss plants, crops, moss and bark keep
their colours. Only green hues move (tools/autumn_tint.py), so flowers, bark
and dry leaves in the same image stay as they are.
Usage: python autumn_plants.py <mod folder> <textures|impostors|all> [strength]"""
import os, re, sys
import game_files as g
import textures
from autumn_tint import tint

# Plants that stay green (or are not leaves).
KEEP = re.compile(r'pine|spruce|cedar|redwood|sequoia|fir_|_fir|cypress|juniper|taxus|yew|hemlock|larch|'
                  r'palm|washingtonia|phoenix|canariensis|cactus|desert|agave|aloe|yucca|succulent|'
                  r'sea|algae|coral|kelp|starfish|abyss|bamboo|banana|ensete|jungle|mangrove|'
                  r'crop|moss|lichen|mushroom|bark|trunk|root|dead|dry|burnt|bare|debris')


_leaves = None


def leaves_tinted(plant):
    """Whether any of the plant's own leaf textures (listed in tree/<plant>.pat)
    gets autumn colours; None when the plant has no .pat."""
    global _leaves
    if _leaves is None:
        _leaves = {e['path'].split('/')[-1][:-4]: e for e in g.entries(r'^tree/[^/]+\.pat$')}
    pat = _leaves.get(plant)
    if not pat:
        return None
    names = set(t.decode().lower() for t in re.findall(rb'(\w+_color)\.tga', g.read(pat)))
    return any(not KEEP.search(n) for n in names)


def wanted(path, what):
    name = path.split('/')[-1]
    folder = path.split('/')[1]
    if folder == 'texture':
        if what == 'impostors' or not re.search(r'_(color|subsurface)\.dds$', name):
            return False
    elif folder == 'impostor':
        if what == 'textures' or not name.endswith('_impostor_color.dds'):
            return False
        # The distance image follows the plant's leaves up close.
        tinted = leaves_tinted(name[:-len('_impostor_color.dds')])
        if tinted is not None:
            return tinted
    else:
        return False
    return not KEEP.search(name)


def main():
    out, what = sys.argv[1], sys.argv[2]
    strength = float(sys.argv[3]) if len(sys.argv) > 3 else 1.0
    n = size = skipped = 0
    for e in g.entries(r'^tree/(texture|impostor)/.*\.dds$'):
        if not wanted(e['path'], what):
            skipped += 1
            continue
        try:
            data = tint(textures.unpack(e), strength)
        except Exception as ex:
            print('skipped', e['path'], ex)
            continue
        path = os.path.join(out, e['archive'], *e['path'].split('/'))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        open(path, 'wb').write(data)
        n += 1; size += len(data)
    print(n, 'textures,', size // 1000000, 'MB;', skipped, 'left alone')


if __name__ == "__main__":
    main()
