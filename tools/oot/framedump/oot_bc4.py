"""Decode a dumped BC4_UNORM .raw.zst (level 0, pitch-linear blocks as the dump stores them) to a
grayscale PNG. Read-only on the dump. Usage: python3 -I oot_bc4.py <in.raw.zst> <w> <h> <out.png>"""
import sys, struct, zlib
from compression import zstd
src, w, h, out = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
data = zstd.decompress(open(src, 'rb').read())
bw, bh = (w + 3) // 4, (h + 3) // 4
img = bytearray(w * h)
for by in range(bh):
    for bx in range(bw):
        o = (by * bw + bx) * 8
        if o + 8 > len(data):
            break
        a0, a1 = data[o], data[o + 1]
        bits = int.from_bytes(data[o + 2:o + 8], 'little')
        pal = [a0, a1]
        if a0 > a1:
            pal += [((7 - i) * a0 + i * a1) // 7 for i in range(1, 7)]
        else:
            pal += [((5 - i) * a0 + i * a1) // 5 for i in range(1, 5)] + [0, 255]
        for i in range(16):
            x, y = bx * 4 + i % 4, by * 4 + i // 4
            if x < w and y < h:
                img[y * w + x] = pal[(bits >> (3 * i)) & 7]
raw = b''.join(b'\0' + bytes(img[y * w:(y + 1) * w]) for y in range(h))
def chunk(t, p):
    return struct.pack('>I', len(p)) + t + p + struct.pack('>I', zlib.crc32(t + p) & 0xffffffff)
open(out, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 0, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))
print('decoded', len(data), 'bytes ->', out)
