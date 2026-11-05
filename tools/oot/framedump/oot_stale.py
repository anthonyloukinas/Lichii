"""For each draw-bound buffer, find earlier compute dispatches in the frame that wrote an overlapping
device range; report the draw's gpu_modified flag and how much of the draw's guest snapshot is the
0x6f6f6f6f fill. Read-only; loads one small buffer at a time. Usage: python3 -I oot_stale.py <dumpdir>"""
import json, sys, os, collections
from compression import zstd
d = sys.argv[1]
writes = []  # (after_draws, dispatch, shader, dev, size)
rows = []
for l in open(os.path.join(d, 'manifest.jsonl')):
    r = json.loads(l)
    if r['kind'] != 'buffer':
        continue
    if 'dispatch' in r and r['written']:
        writes.append((r['after_draws'], r['dispatch'], r['shader'], int(r['device_addr'], 16), r['bound_size']))
    elif 'pass' in r:
        rows.append(r)
def fill_fraction(h, n):
    with open(os.path.join(d, f'buffer_{h}.bin.zst'), 'rb') as f:
        b = zstd.decompress(f.read())[:n]
    words = len(b) // 4
    return sum(1 for i in range(0, words * 4, 4) if b[i:i + 4] == b'\x6f\x6f\x6f\x6f') / max(words, 1)
summary = collections.Counter()
for r in rows:
    dev = int(r['device_addr'], 16); end = dev + r['size']
    hits = [w for w in writes if w[0] <= r['draw'] and w[3] < end and dev < w[3] + w[4]]
    if not hits:
        continue
    w = hits[-1]
    ff = fill_fraction(r['hash'], r['size']) if r['source'] == 'guest' else None
    summary[(r['pass'], r['gpu_modified'], r['source'])] += 1
    print(f"pass {r['pass']} draw {r['draw']} st{r['stage']} {r['type'][:3]}{r['index']} {r['shader']} gpu={r['gpu_addr']} size={r['size']} gpu_mod={r['gpu_modified']} src={r['source']} "
          f"<- D{w[1]} {w[2]} (after_draws {w[0]})" + (f" fill6f={ff:.2f}" if ff is not None else ''))
print(summary)
