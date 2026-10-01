"""Autumn colours for every terrain colour tile (leveldata/rootlevel/terrain/color/
terrain_x_y_color_c.dds): what the terrain and its vegetation look like from afar.
Usage: python autumn_tiles.py <mod folder> [strength]"""
import os, sys
import game_files as g
import textures
from autumn_tint import tint


def main():
    out = sys.argv[1]
    strength = float(sys.argv[2]) if len(sys.argv) > 2 else 1.0
    n = 0
    for e in g.entries(r'leveldata/rootlevel/terrain/color/terrain_-?\d+_-?\d+_color_c\.dds$'):
        data = tint(textures.unpack(e), strength)
        path = os.path.join(out, e['archive'], *e['path'].split('/'))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        open(path, 'wb').write(data)
        n += 1
    print(n, 'tiles')


if __name__ == "__main__":
    main()
