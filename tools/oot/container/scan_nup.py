#!/usr/bin/env python3
"""scan_nup.py [--full] NUP DICTDIR SHADERDIR OUT.json GLOB [GLOB...]   (a GLOB starting with ! excludes)
For every romfs member matching a GLOB: read it from the NUP, zstd-decompress (.zs) in memory,
and locate dumped programs exactly as match.py does. Nothing is written but OUT.json.
Also counts BNSH magics per member. Default scans only inside BNSH blobs; --full scans whole members."""
import fnmatch, json, os, sys, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nup_tool, unzs
full = '--full' in sys.argv
argv = [a for a in sys.argv if a != '--full']
nup, dictdir, sd, outp, globs = argv[1], argv[2], argv[3], argv[4], argv[5:]
dicts = unzs.load_dicts(dictdir)
progs = {}
for f in sorted(os.listdir(sd)):
    if f.endswith('.header.bin') or not f.endswith('.bin') or f.startswith('rejected'): continue
    key, kind, addr = f[:-4].split('-')
    progs.setdefault(key, dict(kind=kind, file=f))
code = {k: open(os.path.join(sd, p['file']), 'rb').read() for k, p in progs.items()}
index = collections.defaultdict(list)
for k, b in code.items(): index[b[:32]].append(k)
n = nup_tool.Nup(nup)
p = n.packages[0]
hits = collections.defaultdict(list); bnsh = {}
for rel, fi in n.walk(p['romfs_dir']):
    if not any(fnmatch.fnmatchcase(rel, g) for g in globs if not g.startswith('!')): continue
    if any(fnmatch.fnmatchcase(rel, g[1:]) for g in globs if g.startswith('!')): continue
    d = n.read(fi)
    if rel.endswith('.zs'): d = unzs.decompress(d, dicts)
    c = d.count(b'BNSH')
    if c: bnsh[rel] = c
    if b'BAHS' in d[:16] or b'FSHA' in d[:16]: bnsh.setdefault(rel, 0)
    o = d.find(b'BNSH')
    if full: o = 0
    while o >= 0:
        # scan only inside BNSH blobs: size at +0x1c (u32)
        size = (len(d) - o) if full else (int.from_bytes(d[o+0x1c:o+0x20], 'little') or (len(d) - o))
        end = min(len(d), o + size)
        for q in range(o, end - 32, 16):
            ks = index.get(d[q:q+32])
            if ks:
                for k in ks:
                    if d[q:q+len(code[k])] == code[k]:
                        hits[k].append(dict(file=rel, offset=q, size=len(code[k])))
        o = -1 if full else d.find(b'BNSH', end if end > o else o + 4)
json.dump(dict(bnsh_members=bnsh, hits=hits), open(outp, 'w'), indent=1)
found = set(hits)
print('members with BNSH', len(bnsh), 'BNSH total', sum(bnsh.values()))
print('programs found', collections.Counter(progs[k]['kind'] for k in found))
