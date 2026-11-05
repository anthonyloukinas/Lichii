#!/usr/bin/env python3
"""nso.py IN OUT: decompress an NSO0 (LZ4 block segments) into a flat memory image. Pure Python."""
import struct, sys
def lz4_block(src, out_size):
    dst = bytearray(); i = 0; n = len(src)
    while i < n:
        tok = src[i]; i += 1
        lit = tok >> 4
        if lit == 15:
            while True:
                b = src[i]; i += 1; lit += b
                if b != 255: break
        dst += src[i:i+lit]; i += lit
        if i >= n: break
        off = src[i] | (src[i+1] << 8); i += 2
        ml = tok & 15
        if ml == 15:
            while True:
                b = src[i]; i += 1; ml += b
                if b != 255: break
        ml += 4
        start = len(dst) - off
        if off >= ml:
            dst += dst[start:start+ml]
        else:
            for j in range(ml): dst.append(dst[start + j])
    assert len(dst) == out_size, (len(dst), out_size)
    return bytes(dst)
d = open(sys.argv[1], 'rb').read()
assert d[:4] == b'NSO0'
flags = struct.unpack_from('<I', d, 0xC)[0]
segs = [struct.unpack_from('<III', d, 0x10 + 0x10 * i) for i in range(3)]
csz = struct.unpack_from('<III', d, 0x60)
mem_end = max(m + s for _, m, s in segs)
img = bytearray(mem_end)
for i, (fo, mo, sz) in enumerate(segs):
    raw = d[fo:fo + csz[i]]
    seg = lz4_block(raw, sz) if flags & (1 << i) else raw[:sz]
    img[mo:mo + sz] = seg
    print('seg', i, hex(mo), sz, 'compressed' if flags & (1 << i) else 'raw')
open(sys.argv[2], 'wb').write(img)
