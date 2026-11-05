"""Per-draw listing of buffers/textures for chosen passes (read-only). Usage: python3 -I oot_draws.py <dumpdir> pass[,pass...]"""
import json, sys, os, collections
d = sys.argv[1]; want = {int(x) for x in sys.argv[2].split(',')}
draws = collections.defaultdict(list)
for l in open(os.path.join(d, 'manifest.jsonl')):
    r = json.loads(l)
    if r.get('pass') in want and r['kind'] in ('buffer', 'sampled'):
        draws[(r['pass'], r['draw'])].append(r)
for (p, dr), rs in sorted(draws.items()):
    sh = sorted({(r['stage'], r['shader']) for r in rs if r['kind'] == 'buffer'})
    print(f"pass {p} draw {dr} shaders {sh}")
    for r in rs:
        if r['kind'] == 'buffer':
            print(f"   buf st{r['stage']} {r['type'][:3]}{r['index']} size={r['size']} bound={r['bound_size']} gpu={r['gpu_addr']} dev={r['device_addr']} written={r['written']} gpu_mod={r['gpu_modified']} src={r['source']} raw={r.get('raw')}")
        else:
            s = r.get('sampler', {})
            print(f"   tex st{r['stage']} tic{r['tic']} {r.get('format')} {r.get('width')}x{r.get('height')} addr={r.get('gpu_addr')} skipped={r.get('skipped')} raw={r.get('raw')} wrap={s.get('wrap')}")
