"""LZ4 blocks grown to an exact size without changing what they decode to: the
end of a match is handed over to the literals after it (the same bytes, stored
plainly instead of copied), until the block is as long as wanted.

  grow(block, raw, size) -> a block of `size` bytes that decodes to `raw`"""
import lz4.block


def parse(block):
    """Sequences of [literals, match offset, match length]; the last has no match."""
    seqs, i, n = [], 0, len(block)
    while i < n:
        token = block[i]; i += 1
        lit = token >> 4
        if lit == 15:
            while True:
                b = block[i]; i += 1; lit += b
                if b != 255:
                    break
        literals = block[i:i + lit]; i += lit
        if i >= n:
            seqs.append([bytearray(literals), 0, 0])
            break
        offset = block[i] | (block[i + 1] << 8); i += 2
        ml = token & 15
        if ml == 15:
            while True:
                b = block[i]; i += 1; ml += b
                if b != 255:
                    break
        seqs.append([bytearray(literals), offset, ml + 4])
    return seqs


def _length(out, extra):
    while extra >= 255:
        out.append(255); extra -= 255
    out.append(extra)


def encode(seqs):
    out = bytearray()
    for literals, offset, ml in seqs:
        lit = len(literals)
        m = ml - 4 if ml else 0
        out.append((min(lit, 15) << 4) | (min(m, 15) if ml else 0))
        if lit >= 15:
            _length(out, lit - 15)
        out += literals
        if ml:
            out += bytes((offset & 255, offset >> 8))
            if m >= 15:
                _length(out, m - 15)
    return bytes(out)


def _extra(x):
    """Bytes a length field needs beyond its nibble."""
    return 0 if x < 15 else 1 + (x - 15) // 255


def split_matches(block, raw, max_match=60000, max_near=1024, near=16):
    """The same block with no copy longer than max_match, and none longer than
    max_near from close behind (the game's own blocks stay under about 60 KB
    and 1 KB): a long copy goes on as further copies from the same distance.
    A copy may not start in the last 12 bytes of the output (LZ4's rule)."""
    seqs = parse(block)
    out = []
    pos = 0
    total = len(raw)
    for literals, offset, ml in seqs:
        pos += len(literals)
        limit = max_near if offset < near else max_match
        if not ml or ml <= limit:
            out.append([literals, offset, ml])
            pos += ml
            continue
        first = True
        while ml > 0:
            take = min(ml, limit)
            rest = ml - take
            # The next part must be a whole copy (4 bytes or more) that starts
            # 12 bytes or more before the end: otherwise this part takes it all.
            if rest and (rest < 4 or pos + take + 12 > total):
                take = ml
            out.append([literals if first else bytearray(), offset, take])
            first = False
            pos += take
            ml -= take
    block2 = encode(out)
    if lz4.block.decompress(block2, uncompressed_size=len(raw)) != raw:
        raise ValueError('split block decodes differently')
    return block2


def longest_match(block):
    return max((q[2] for q in parse(block)), default=0)


def longest_literals(block):
    return max(len(q[0]) for q in parse(block))


def grow(block, raw, size, max_literals=2048):
    if len(block) == size:
        return block
    if len(block) > size:
        raise ValueError('block already bigger')
    seqs = parse(block)
    ends, pos = [], 0
    for literals, offset, ml in seqs:
        pos += len(literals) + ml
        ends.append(pos)
    remaining = size - len(block)
    k = len(seqs) - 2
    while remaining > 0 and k >= 0:
        ml = seqs[k][2]
        lit = len(seqs[k + 1][0])

        def delta(d):
            # Shortening match k by d: d more literal bytes after it, and the
            # two length fields may change size.
            return d + _extra(lit + d) - _extra(lit) + _extra(ml - 4 - d) - _extra(ml - 4)

        # The game's own blocks never have literal runs over about 2 KB: the
        # next sequence's literals may grow only up to max_literals.
        room = min(ml - 4, max_literals - lit)
        if ml > 4 and room > 0:
            lo, hi = 0, room            # largest d with delta(d) <= remaining (delta never falls)
            while lo < hi:
                mid = (lo + hi + 1) // 2
                if delta(mid) <= remaining:
                    lo = mid
                else:
                    hi = mid - 1
            d = lo
            if d:
                end = ends[k]
                seqs[k + 1][0] = bytearray(raw[end - d:end]) + seqs[k + 1][0]
                seqs[k][2] = ml - d
                ends[k] -= d
                remaining -= delta(d)
        k -= 1
    out = encode(seqs)
    if len(out) != size:
        raise ValueError(f'could not reach {size} (got {len(out)})')
    if lz4.block.decompress(out, uncompressed_size=len(raw)) != raw:
        raise ValueError('grown block decodes differently')
    return out


if __name__ == '__main__':
    import os, time
    raw = os.urandom(2000) + bytes(600000) + os.urandom(3000) + b'abc' * 50000
    b = lz4.block.compress(raw, store_size=False)
    t = time.time()
    for extra in (0, 1, 2, 15, 16, 255, 300, 1500):
        g = grow(b, raw, len(b) + extra)
        assert len(g) == len(b) + extra and lz4.block.decompress(g, uncompressed_size=len(raw)) == raw
        assert longest_literals(g) <= max(2048, longest_literals(b))
    print('ok', len(b), '%.2fs' % (time.time() - t))
