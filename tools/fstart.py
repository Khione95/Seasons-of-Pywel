"""Function start containing rva (back to int3/cc padding)."""
import sys, xref

def fstart(rva):
    o = rva - xref.TVA
    c = xref.code
    while o > 0:
        if c[o - 1] == 0xCC and c[o - 2] == 0xCC:
            return xref.TVA + o
        o -= 1
    return None

if __name__ == '__main__':
    for a in sys.argv[1:]:
        print(f'{int(a,16):X} -> {fstart(int(a,16)):X}')
