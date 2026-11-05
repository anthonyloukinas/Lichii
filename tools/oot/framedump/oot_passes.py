"""Summarise a Lichii frame dump manifest per pass (read-only). Usage: python3 -I oot_passes.py <dumpdir> [shader-index.json]"""
import json, sys, collections, os
d = sys.argv[1]
idx = {}
if len(sys.argv) > 2:
    try:
        idx = json.load(open(sys.argv[2]))
    except Exception as e:
        print("index load failed", e)
def prov(k):
    v = idx.get(k)
    if not v:
        return '(not in index)'
    locs = v.get('locations', [])
    eff = sorted({l['romfs_path'].split('/')[-1].split('.')[0] for l in locs if l['romfs_path'].startswith('Effect/')})
    oth = sorted({l['romfs_path'] for l in locs if not l['romfs_path'].startswith('Effect/')})
    st = sorted({l.get('stage','?') for l in locs})
    out = '/'.join(st) + ' '
    if eff:
        out += f"EFFECT x{len(eff)} [{','.join(eff[:4])}{'...' if len(eff)>4 else ''}] "
    if oth:
        out += ' '.join(o.split('/')[-1][:40] for o in oth[:2])
    return out
rows = [json.loads(l) for l in open(os.path.join(d, 'manifest.jsonl'))]
passes = collections.defaultdict(lambda: {'draws': set(), 'vs': set(), 'fs': set(), 'shaders': set(), 'tex': [], 'rt': []})
for r in rows:
    if 'pass' not in r:
        continue
    p = passes[r['pass']]
    if r['kind'] == 'buffer':
        p['draws'].add(r['draw'])
        if r['shader'] != '0' * 16:
            p['shaders'].add((r['stage'], r['shader']))
    elif r['kind'] == 'sampled':
        p['draws'].add(r['draw'])
        p['tex'].append((r['draw'], r['stage'], r['tic'], r.get('format'), r.get('width'), r.get('height'), r.get('skipped')))
    elif r['kind'] == 'pass':
        p['rt'].append((r['slot'], r['format'], r['width'], r['height'], r['first_draw'], r['draw_count'], r['gpu_addr']))
for n in sorted(passes):
    p = passes[n]
    dr = sorted(p['draws'])
    print(f"pass {n:3d} draws {dr[0] if dr else '-'}..{dr[-1] if dr else '-'} n_seen={len(dr)} rt={p['rt']}")
    for st, sh in sorted(p['shaders']):
        print(f"    stage{st} {sh} {prov(sh)}"[:240])
    for t in p['tex'][:40]:
        print("    tex", t)
