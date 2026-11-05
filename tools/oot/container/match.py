#!/usr/bin/env python3
"""match.py SHADERDIR OUT.json LABEL=FILE...
Locate every unique dumped program (by key) in the given files. A program matches at offset o
when file[o:o+len(dump)] == dump (the dump is the captured code without the 0x80 SPH);
for graphics programs it also records whether file[o-0x80:o] equals the dumped .header.bin.
Scans each file at a 16-byte stride using the first 32 code bytes as an index key."""
import json, mmap, os, sys, collections
sd, outp = sys.argv[1], sys.argv[2]
progs = {}
for f in sorted(os.listdir(sd)):
    if f.endswith('.header.bin') or not f.endswith('.bin') or f.startswith('rejected'):
        continue
    key, kind, addr = f[:-4].split('-')
    p = progs.setdefault(key, dict(kind=kind, addrs=[], file=f))
    p['addrs'].append(addr)
code = {}; hdr = {}
for k, p in progs.items():
    code[k] = open(os.path.join(sd, p['file']), 'rb').read()
    h = os.path.join(sd, p['file'][:-4] + '.header.bin')
    hdr[k] = open(h, 'rb').read() if os.path.exists(h) else None
index = collections.defaultdict(list)
for k, b in code.items():
    index[b[:32]].append(k)
hits = collections.defaultdict(list)
for arg in sys.argv[3:]:
    label, path = arg.split('=', 1)
    with open(path, 'rb') as fh:
        if os.fstat(fh.fileno()).st_size == 0: continue
        mm = mmap.mmap(fh.fileno(), 0, access=mmap.ACCESS_READ)
        n = len(mm)
        for align in range(0, 16, 16):
            for o in range(align, n - 32, 16):
                ks = index.get(mm[o:o+32])
                if not ks: continue
                for k in ks:
                    b = code[k]
                    if mm[o:o+len(b)] == b:
                        hm = None
                        if hdr[k] is not None and o >= 0x80:
                            hm = mm[o-0x80:o] == hdr[k]
                        hits[k].append(dict(file=label, offset=o, size=len(b), header_match=hm))
    print('scanned', label, file=sys.stderr)
res = {}
for k, p in progs.items():
    res[k] = dict(kind=p['kind'], dumped_size=len(code[k]), gpu_addrs=p['addrs'], locations=hits.get(k, []))
json.dump(res, open(outp, 'w'), indent=1)
c = collections.Counter((v['kind'], bool(v['locations'])) for v in res.values())
print(dict(c))
