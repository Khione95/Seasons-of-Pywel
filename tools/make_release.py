"""Assembles the release zip, laid out for DMM and for installing by hand:

  Seasons of Pywel <version>.zip
    Seasons/Seasons.asi
    Seasons/Seasons/data/<group>.dat, seasons.idx   (tools/build_season_data.py)
    Seasons/README.txt, THIRD_PARTY.txt

DMM: enabling the plugin copies the .asi and the folder beside it (its data)
into the game's bin64. By hand: Seasons.asi and the Seasons folder go into bin64.

Build the plugin (Release x64) and the season data first.
Usage: python make_release.py <version>"""
import os
import sys
import zipfile

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
PLUGIN = os.path.join(ROOT, 'asi', 'bin', 'Seasons.asi')
DATA = os.path.join(ROOT, 'build', 'season_data')
DOCS = os.path.join(ROOT, 'release')
OUT = os.path.join(ROOT, 'dist')


def main():
    version = sys.argv[1]
    files = [(PLUGIN, 'Seasons/Seasons.asi')]
    for name in sorted(os.listdir(DATA)):
        files.append((os.path.join(DATA, name), 'Seasons/Seasons/data/' + name))
    for doc in ('README.txt', 'THIRD_PARTY.txt'):
        files.append((os.path.join(DOCS, doc), 'Seasons/' + doc))
    for path, _ in files:
        if not os.path.exists(path):
            raise SystemExit(f'missing {path}')
    os.makedirs(OUT, exist_ok=True)
    archive = os.path.join(OUT, f'Seasons of Pywel {version}.zip')
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for path, name in files:
            z.write(path, name)
            print(name, os.path.getsize(path), flush=True)
    print('release:', archive, os.path.getsize(archive))


if __name__ == '__main__':
    main()
