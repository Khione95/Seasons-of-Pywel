"""Static helpers on CrimsonDesert.exe: find a string, find code referencing it
(rip-relative lea/mov), disassemble around an address.
  python xref.py str <text>          -> rva of the string and code refs
  python xref.py dis <rva hex> [n]   -> disassemble n instructions"""
import re, struct, sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

EXE = r'E:\SteamLibrary\steamapps\common\Crimson Desert\bin64\CrimsonDesert.exe'
data = open(EXE, 'rb').read()
_nt = struct.unpack_from('<I', data, 0x3C)[0]
_nsec = struct.unpack_from('<H', data, _nt + 6)[0]
_opt = struct.unpack_from('<H', data, _nt + 20)[0]
SECTIONS = []
for _i in range(_nsec):
    _o = _nt + 24 + _opt + 40 * _i
    _name = data[_o:_o + 8].rstrip(bytes(1))
    _vsize, _va, _rsize, _raw = struct.unpack_from('<IIII', data, _o + 8)
    SECTIONS.append((_name, _va, _vsize, _raw, _rsize))
_t = SECTIONS[0]   # the game's code sits in the first section (named .rsrc)
TVA, TRAW, TSIZE = _t[1], _t[3], _t[4]
code = bytes(data[TRAW:TRAW + TSIZE])
md = Cs(CS_ARCH_X86, CS_MODE_64)


def rva_of(off):
    for name, va, vsize, raw, rsize in SECTIONS:
        if raw <= off < raw + rsize:
            return va + off - raw
    return None


def refs(target):
    """Code places whose rip-relative disp32 points at rva target (lea/mov r, [rip+x])."""
    out = []
    for m in re.finditer(rb'[\x48\x4c][\x8d\x8b][\x05\x0d\x15\x1d\x25\x2d\x35\x3d]', code):
        o = m.start()
        disp = struct.unpack_from('<i', code, o + 3)[0]
        if TVA + o + 7 + disp == target:
            out.append(TVA + o)
    return out


def dis(rva, n=40):
    o = rva - TVA
    for i in md.disasm(code[o:o + n * 15], 0x140000000 + rva):
        print(f'{i.address - 0x140000000:08X}  {i.mnemonic} {i.op_str}')
        n -= 1
        if n == 0:
            break


if __name__ == '__main__':
    if sys.argv[1] == 'str':
        s = sys.argv[2].encode()
        for m in re.finditer(re.escape(s), data):
            r = rva_of(m.start())
            print(f'string at {r:08X}:', [f'{x:08X}' for x in refs(r)])
    else:
        dis(int(sys.argv[2], 16), int(sys.argv[3]) if len(sys.argv) > 3 else 40)
