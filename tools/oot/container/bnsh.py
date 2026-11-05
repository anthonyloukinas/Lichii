#!/usr/bin/env python3
"""bnsh.py FILE...: enumerate NVN2 BNSH shader containers inside FILE (bfsha / vsetb / any blob).
Layout (reverse-engineered here, little-endian, offsets relative to the BNSH start):
  +0x00 'BNSH\\0\\0\\0\\0'  +0x08 u32 version  +0x0C u16 BOM  +0x0E u8 align  +0x1C u32 file size
  +0x60 'grsc' block: +0x10 u16 api type (4=NVN) u16 api version  +0x1C u32 variation count
        +0x20 u64 variation array  +0x28 u64 memory-pool record
  variation (0x40): +0x10 u64 -> program record
  program record: +0x08..+0x48 u64 stage code records [vertex, hull, domain, geometry, pixel, compute,
        slot6, slot7, slot8]; slots 7/8 seen with pixel only = task/mesh pipelines (naming unverified)
  stage code record: +0x08 u64 control section (NVN magic 0x98761234), +0x10 u64 code (SPH+SASS),
        +0x18 u32 code size, +0x1C u32 control size
Prints a JSON summary; import iter_programs(data) for (bnsh_off, var, stage, code_off_abs, code_size,
control_off_abs, control_size)."""
import json, struct, sys, collections
STAGES = ['vertex', 'hull', 'domain', 'geometry', 'pixel', 'compute', 'slot6', 'task?', 'mesh?']
def iter_bnsh(d):
    o = d.find(b'BNSH\0\0\0\0')
    while o >= 0:
        size = struct.unpack_from('<I', d, o + 0x1c)[0]
        yield o, size
        o = d.find(b'BNSH\0\0\0\0', o + max(size, 8))
def iter_programs(d):
    for b, size in iter_bnsh(d):
        g = b + 0x60
        if d[g:g+4] != b'grsc': continue
        api, apiver = struct.unpack_from('<HH', d, g + 0x10)
        nvar = struct.unpack_from('<I', d, g + 0x1c)[0]
        varr = struct.unpack_from('<Q', d, g + 0x20)[0]
        for v in range(nvar):
            prog = struct.unpack_from('<Q', d, b + varr + v * 0x40 + 0x10)[0]
            if not prog: continue
            for s in range(9):
                rec = struct.unpack_from('<Q', d, b + prog + 0x08 + s * 8)[0]
                if not rec: continue
                ctrl, code = struct.unpack_from('<QQ', d, b + rec + 0x08)
                csize, ctsize = struct.unpack_from('<II', d, b + rec + 0x18)
                yield dict(bnsh=b, bnsh_size=size, variation=v, stage=STAGES[s], code=b + code,
                           code_size=csize, control=b + ctrl, control_size=ctsize, api=api, api_version=apiver)
def pool(d, b):
    g = b + 0x60
    pr = struct.unpack_from('<Q', d, g + 0x28)[0]
    flags, psize, poff = struct.unpack_from('<IIQ', d, b + pr)
    return dict(flags=flags, size=psize, offset=b + poff)
if __name__ == '__main__':
    for path in sys.argv[1:]:
        d = open(path, 'rb').read()
        for b, size in iter_bnsh(d):
            progs = [p for p in iter_programs(d) if p['bnsh'] == b]
            uniq = {p['code'] for p in progs}
            st = collections.Counter(p['stage'] for p in progs)
            ust = collections.Counter(s for s, c in {(p['stage'], p['code']) for p in progs})
            nvar = struct.unpack_from('<I', d, b + 0x60 + 0x1c)[0]
            print(json.dumps(dict(file=path.split('/')[-1], bnsh=hex(b), size=size, variations=nvar,
                stage_refs=dict(st), unique_code_by_stage=dict(ust), unique_code=len(uniq),
                code_bytes=sum(p['code_size'] for p in {p['code']: p for p in progs}.values()),
                pool=pool(d, b))))
