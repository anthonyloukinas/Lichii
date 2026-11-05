#!/usr/bin/env python3
"""needles.py SHADERDIR N_PER_KIND FILE...: pick dumped programs (gfx + cs), take 128-byte needles
(start of code, middle of code) and search each FILE (mmap). Prints hits."""
import mmap, os, sys, collections
sd, n = sys.argv[1], int(sys.argv[2])
progs = {}
for f in sorted(os.listdir(sd)):
    if f.endswith('.header.bin') or not f.endswith('.bin') or f.startswith('rejected'): continue
    key, kind, addr = f[:-4].split('-')
    progs.setdefault((key, kind), f)
picked = []
for kind in ('gfx', 'cs'):
    ks = [k for k in progs if k[1] == kind]
    step = max(1, len(ks) // n)
    picked += ks[::step][:n]
needles = []
for k in picked:
    b = open(os.path.join(sd, progs[k]), 'rb').read()
    if len(b) < 512: needles.append((k, 'start', b[:min(len(b), 128)])); continue
    needles.append((k, 'start', b[:128]))
    m = (len(b) // 2) & ~15
    needles.append((k, 'mid', b[m:m+128]))
hits = collections.Counter()
for path in sys.argv[3:]:
    with open(path, 'rb') as fh:
        mm = mmap.mmap(fh.fileno(), 0, access=mmap.ACCESS_READ)
        for k, where, nd in needles:
            o = mm.find(nd)
            if o >= 0:
                hits[k] += 1
                c = 0; p = o
                while p >= 0: c += 1; p = mm.find(nd, p + 1)
                print('HIT', k[0], k[1], where, os.path.basename(path), hex(o), 'count', c)
print('programs picked', len(picked), 'with any hit', len(hits))
