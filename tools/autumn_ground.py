"""Autumn colours for the terrain's grass ground textures (object/texture/
pa_terrain_grass_*, cd_bg_ground_grass_*, cd_hterrain_*grass*: the colour ones,
not normals / height / specular).
Usage: python autumn_ground.py <mod folder> [strength]"""
import os, re, sys
import game_files as g
import textures
from autumn_tint import tint


def main():
    out = sys.argv[1]
    strength = float(sys.argv[2]) if len(sys.argv) > 2 else 1.0
    n = 0
    for e in g.entries(r'^object/texture/(pa_terrain_(grass|weed|meadow|field|lawn)|cd_bg_ground_grass|cd_hterrain_.*grass).*\.dds$'):
        name = e['path'].split('/')[-1]
        if re.search(r'_(n|sp|d|h|m|mask|nm|normal)\.dds$', name):
            continue
        try:
            data = tint(textures.unpack(e), strength)
        except Exception as ex:
            print('skipped', name, ex)
            continue
        path = os.path.join(out, e['archive'], *e['path'].split('/'))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        open(path, 'wb').write(data)
        n += 1
    print(n, 'ground textures')


if __name__ == "__main__":
    main()
