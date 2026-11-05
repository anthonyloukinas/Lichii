#!/usr/bin/env python3
"""Minimal SARC reader. sarc.py FILE [OUTDIR]: list (and extract) members. Import: entries(bytes)."""
import os, struct, sys
def entries(d):
    assert d[:4] == b'SARC', d[:4]
    hl, bom = struct.unpack_from('<HH', d, 4)
    e = '<' if bom == 0xFEFF else '>'
    data_off = struct.unpack_from(e + 'I', d, 0xC)[0]
    sfat = hl; assert d[sfat:sfat+4] == b'SFAT'
    shl, cnt, mult = struct.unpack_from(e + 'HHI', d, sfat + 4)
    nodes = sfat + shl
    sfnt = nodes + cnt * 16; assert d[sfnt:sfnt+4] == b'SFNT'
    names = sfnt + struct.unpack_from(e + 'H', d, sfnt + 4)[0]
    out = []
    for i in range(cnt):
        h, attr, s, en = struct.unpack_from(e + 'IIII', d, nodes + i * 16)
        if attr & 0x01000000:
            no = names + (attr & 0xFFFF) * 4
            name = d[no:d.index(b'\0', no)].decode('utf-8', 'replace')
        else:
            name = '%08x' % h
        out.append((name, data_off + s, en - s))
    return out
if __name__ == '__main__':
    d = open(sys.argv[1], 'rb').read()
    for n, o, s in entries(d):
        print(s, n)
        if len(sys.argv) > 2:
            parts = [p for p in n.split('/') if p not in ('', '.', '..')]
            dst = os.path.join(sys.argv[2], *parts); os.makedirs(os.path.dirname(dst), exist_ok=True)
            open(dst, 'wb').write(d[o:o+s])
