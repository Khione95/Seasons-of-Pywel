"""Callers of a function: E8 rel32 calls (and E9 jumps) to rva target.
  python callers.py <rva hex> ..."""
import re, struct, sys
import xref

def callers(target):
    out = []
    c = xref.code
    for m in re.finditer(rb'[\xe8\xe9]', c):
        o = m.start()
        if o + 5 > len(c):
            break
        d = struct.unpack_from('<i', c, o + 1)[0]
        if xref.TVA + o + 5 + d == target:
            out.append((xref.TVA + o, 'call' if c[o] == 0xE8 else 'jmp'))
    return out

if __name__ == '__main__':
    for a in sys.argv[1:]:
        t = int(a, 16)
        print(f'{t:X}:', [f'{r:X} {k}' for r, k in callers(t)])
