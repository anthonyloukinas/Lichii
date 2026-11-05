"""Inspect dumped buffers (read-only). Usage:
  python3 -I oot_buf.py <dumpdir> show <buffer_hash> [offset] [nwords]
  python3 -I oot_buf.py <dumpdir> cmp <hashA> <offA> <hashB> <offB> <nbytes>
  python3 -I oot_buf.py <dumpdir> stats <hash> [offset] [nbytes]"""
import sys, os, struct, math
from compression import zstd
d = sys.argv[1]
def load(h):
    with open(os.path.join(d, f'buffer_{h}.bin.zst'), 'rb') as f:
        return zstd.decompress(f.read())
cmd = sys.argv[2]
if cmd == 'show':
    b = load(sys.argv[3]); off = int(sys.argv[4], 0) if len(sys.argv) > 4 else 0
    n = int(sys.argv[5]) if len(sys.argv) > 5 else 64
    print('len', len(b))
    for i in range(off, min(len(b), off + n * 4), 16):
        ws = struct.unpack_from('<4I', b + b'\0' * 16, i)
        fs = struct.unpack_from('<4f', b + b'\0' * 16, i)
        print(f"{i:6x}: " + ' '.join(f'{w:08x}' for w in ws) + '   ' + ' '.join(f'{x: .5g}' for x in fs))
elif cmd == 'stats':
    b = load(sys.argv[3]); off = int(sys.argv[4], 0) if len(sys.argv) > 4 else 0
    n = int(sys.argv[5], 0) if len(sys.argv) > 5 else len(b) - off
    seg = b[off:off + n]; cnt = len(seg) // 4
    fl = struct.unpack_from(f'<{cnt}f', seg)
    nan = sum(1 for x in fl if math.isnan(x)); inf = sum(1 for x in fl if math.isinf(x))
    zero = sum(1 for x in fl if x == 0); big = sum(1 for x in fl if not math.isnan(x) and abs(x) > 1e4)
    fin = [x for x in fl if math.isfinite(x)]
    print(f'len={len(b)} words={cnt} zero={zero} nan={nan} inf={inf} |x|>1e4={big} min={min(fin) if fin else None:.4g} max={max(fin) if fin else None:.4g}')
elif cmd == 'cmp':
    a = load(sys.argv[3]); oa = int(sys.argv[4], 0); bb = load(sys.argv[5]); ob = int(sys.argv[6], 0); n = int(sys.argv[7], 0)
    x = a[oa:oa + n]; y = bb[ob:ob + n]
    diff = sum(1 for i in range(0, min(len(x), len(y)) - 3, 4) if x[i:i + 4] != y[i:i + 4])
    print(f'lenA={len(a)} lenB={len(bb)} compared={min(len(x), len(y))} differing_words={diff}')
