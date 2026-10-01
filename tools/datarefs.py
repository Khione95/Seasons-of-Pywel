"""Code reading/writing a data rva through rip-relative operands (any instruction).
  python datarefs.py <rva hex> ..."""
import sys
import xref

def datarefs(target, start=None, end=None):
    md = xref.md
    md.skipdata = True
    out = []
    c = xref.code
    # rip-relative disp32 anywhere: scan every offset for a disp that lands on target
    import struct
    for o in range(0, len(c) - 4):
        d = struct.unpack_from('<i', c, o)[0]
        # the instruction ends at o+4 (+ an immediate of 0, 1 or 4 bytes)
        for imm in (0, 1, 4):
            if xref.TVA + o + 4 + imm + d == target:
                out.append((xref.TVA + o, imm))
    return out

if __name__ == '__main__':
    import numpy as np
    c = np.frombuffer(xref.code, dtype=np.uint8)
    for a in sys.argv[1:]:
        t = int(a, 16)
        n = len(c) - 4
        d = (c[:n].astype(np.int64) | (c[1:n+1].astype(np.int64) << 8) | (c[2:n+2].astype(np.int64) << 16) | (c[3:n+3].astype(np.int64) << 24))
        d = np.where(d >= 1 << 31, d - (1 << 32), d)
        pos = np.arange(n, dtype=np.int64) + xref.TVA + 4
        hits = []
        for imm in (0, 1, 4):
            hits += [int(x) for x in np.nonzero(pos + imm + d == t)[0]]
        found = []
        for o in sorted(set(hits)):
            # verify by disassembling from a few bytes back
            for back in range(2, 9):
                s = o - back
                ins = next(xref.md.disasm(xref.code[s:s + 16], 0x140000000 + xref.TVA + s), None)
                if ins and ins.size >= back + 4 and 'rip' in ins.op_str and s + ins.size >= o + 4:
                    found.append(f'{ins.address - 0x140000000:X} {ins.mnemonic} {ins.op_str}')
                    break
        print(f'{t:X}:'); [print('  ', f) for f in found]
