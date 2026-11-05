"""Draw-bound buffers whose start address equals a compute dispatch's written-binding start (exact
producer match). Read-only. Usage: python3 -I oot_exact.py <dumpdir>"""
import json, sys, os, collections
from compression import zstd
d = sys.argv[1]
prod = {}
rows = []
for l in open(os.path.join(d, 'manifest.jsonl')):
    r = json.loads(l)
    if r['kind'] != 'buffer':
        continue
    if 'dispatch' in r and r['written'] and r['type'] == 'storage':
        prod.setdefault(r['gpu_addr'], []).append((r['dispatch'], r['shader'], r['after_draws']))
    elif 'pass' in r and r['type'] == 'storage':
        rows.append(r)
def fill(h, n):
    with open(os.path.join(d, f'buffer_{h}.bin.zst'), 'rb') as f:
        b = zstd.decompress(f.read())[:n]
    w = len(b) // 4
    return sum(1 for i in range(0, w * 4, 4) if b[i:i + 4] == b'oooo') / max(w, 1)
seen = set(); summ = collections.Counter(); per_pass = collections.defaultdict(set)
for r in rows:
    p = prod.get(r['gpu_addr'])
    if not p:
        continue
    key = (r['pass'], r['draw'], r['gpu_addr'])
    if key in seen:
        continue
    seen.add(key)
    ff = fill(r['hash'], r['size']) if r['source'] == 'guest' else None
    summ[(r['pass'], r['gpu_modified'])] += 1
    per_pass[r['pass']].add(r['draw'])
    print(f"pass {r['pass']} draw {r['draw']} st{r['stage']} sto{r['index']} vs/fs={r['shader']} gpu={r['gpu_addr']} size={r['size']} gpu_mod={r['gpu_modified']} src={r['source']} <- D{p[-1][0]} {p[-1][1]}" + (f" fill6f={ff:.2f}" if ff is not None else ''))
print(summ)
for k, v in sorted(per_pass.items()):
    print('pass', k, 'draws reading compute output:', len(v), sorted(v)[:40])
