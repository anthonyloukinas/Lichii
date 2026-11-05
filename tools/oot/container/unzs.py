#!/usr/bin/env python3
"""Decompress game-level .zs (zstd) files.
unzs.py DICTDIR OUTDIR FILE...  -> OUTDIR/<name without .zs>
Dictionaries (*.zsdic from Pack/ZsDic.pack) are matched by the frame's dictionary id."""
import os, struct, sys
from compression import zstd

def load_dicts(dictdir):
    out = {}
    for n in (os.listdir(dictdir) if os.path.isdir(dictdir) else []):
        b = open(os.path.join(dictdir, n), 'rb').read()
        if b[:4] == b'\x37\xa4\x30\xec':
            out[struct.unpack_from('<I', b, 4)[0]] = zstd.ZstdDict(b)
    return out

def frame_dict_id(d):
    fhd = d[4]; did_flag = fhd & 3; single = (fhd >> 5) & 1
    pos = 5 + (0 if single else 1)
    n = [0, 1, 2, 4][did_flag]
    return int.from_bytes(d[pos:pos + n], 'little') if n else 0

def decompress(d, dicts):
    did = frame_dict_id(d)
    return zstd.decompress(d, zstd_dict=dicts[did]) if did else zstd.decompress(d)

if __name__ == '__main__':
    dicts = load_dicts(sys.argv[1]); out = sys.argv[2]; os.makedirs(out, exist_ok=True)
    for p in sys.argv[3:]:
        d = open(p, 'rb').read()
        try:
            r = decompress(d, dicts)
        except Exception as e:
            print('FAIL', p, e); continue
        name = os.path.basename(p)
        name = name[:-3] if name.endswith('.zs') else name + '.dec'
        open(os.path.join(out, name), 'wb').write(r)
        print(len(d), '->', len(r), name, r[:8])
