"""Builds a DMM test mod that fills climate texture channels (snow research).
Usage: python climate_test.py <out mod folder> <texture>:<channel B|G|R|A>=<0-255> ...
Reads the unpacked textures from research/climate (see notes)."""
import os, sys
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, '..', 'research', 'climate')

def main():
    out = sys.argv[1]
    edits = {}
    for arg in sys.argv[2:]:
        name, rest = arg.split(':')
        ch, val = rest.split('=')
        edits.setdefault(name, []).append(('BGRA'.index(ch), int(val)))
    for name, changes in edits.items():
        data = bytearray(open(os.path.join(SRC, name + '.dds'), 'rb').read())
        px = np.frombuffer(data, np.uint8, offset=128).reshape(512, 512, 4).copy()
        for ch, val in changes:
            px[:, :, ch] = val
        data[128:] = px.tobytes()
        data[0x20:0x24] = b'\0\0\0\0'      # no packed size: plain pixels
        folder = os.path.join(out, '0002', 'texture')
        os.makedirs(folder, exist_ok=True)
        open(os.path.join(folder, name + '.dds'), 'wb').write(data)
        print('wrote', name, changes)

main()
