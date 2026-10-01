"""Find a byte pattern ("48 8B ?? ...") in the game's code; prints rvas.
  python aob.py "<pattern>" ..."""
import re, sys
import xref

def find(pattern):
    parts = pattern.split()
    rx = b''.join(b'.' if p == '??' else re.escape(bytes([int(p, 16)])) for p in parts)
    return [xref.TVA + m.start() for m in re.finditer(rx, xref.code, re.S)]

if __name__ == '__main__':
    for p in sys.argv[1:]:
        print(p[:40], '...', [f'{x:X}' for x in find(p)][:10])
