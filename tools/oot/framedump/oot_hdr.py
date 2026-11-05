"""Exposure-scaled preview + stats of a dumped B10G11R11_FLOAT or R16G16B16A16_FLOAT target.
Read-only. Usage: python3 -I oot_hdr.py <in.raw.zst> <fmt:b10|rgba16f> <w> <h> <vis_w> <scale> <out.png> [x0 y0 x1 y1]"""
import sys, struct, zlib, math
from compression import zstd
src, fmt, w, h, vw, scale, out = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5]), float(sys.argv[6]), sys.argv[7]
box = [int(v) for v in sys.argv[8:12]] if len(sys.argv) > 8 else [0, 0, vw, h]
data = zstd.decompress(open(src, 'rb').read())
def pf(bits, mb):
    e = bits >> mb; m = bits & ((1 << mb) - 1)
    if e == 0: return m * 2.0 ** (-14 - mb)
    if e == 31: return math.nan if m else math.inf
    return (m | (1 << mb)) * 2.0 ** (e - 15 - mb)
def half(u):
    v = pf(u & 0x7fff, 10); return -v if u & 0x8000 else v
x0, y0, x1, y1 = box
rows = []; lum = []; nonfin = 0; big = 0; mx = 0.0
for y in range(y0, y1):
    row = bytearray(b'\0')
    for x in range(x0, x1):
        if fmt == 'b10':
            v = struct.unpack_from('<I', data, (y * w + x) * 4)[0]
            r, g, b = pf(v & 0x7ff, 6), pf((v >> 11) & 0x7ff, 6), pf(v >> 22, 5)
        else:
            r, g, b = (half(u) for u in struct.unpack_from('<3H', data, (y * w + x) * 8))
        if not all(math.isfinite(c) for c in (r, g, b)):
            nonfin += 1; row += b'\xff\x00\xff'; continue
        l = 0.2126 * r + 0.7152 * g + 0.0722 * b
        lum.append(l); mx = max(mx, r, g, b); big += l > 100
        for c in (r, g, b):
            t = c * scale; t = t / (1 + t)  # Reinhard on the scaled value
            t = 1.055 * t ** (1 / 2.4) - 0.055 if t > 0.0031308 else 12.92 * t
            row.append(max(0, min(255, int(t * 255 + 0.5))))
    rows.append(bytes(row))
lum.sort()
pct = lambda p: lum[min(len(lum) - 1, int(p * len(lum)))] if lum else None
print(f'box={box} pixels={len(lum)} nonfinite={nonfin} max_channel={mx:.4g} lum p50={pct(.5):.4g} p90={pct(.9):.4g} p99={pct(.99):.4g} p999={pct(.999):.4g} lum>100={big}')
W = x1 - x0; H = y1 - y0
def chunk(t, p):
    return struct.pack('>I', len(p)) + t + p + struct.pack('>I', zlib.crc32(t + p) & 0xffffffff)
open(out, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(b''.join(rows))) + chunk(b'IEND', b''))
