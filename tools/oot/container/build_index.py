#!/usr/bin/env python3
"""build_index.py NUP DICTDIR SHADERDIR OUT_INDEX.json OUT_STATS.json

Enumerates every shader program stored in the game's romfs and maps each dumped runtime program
(key) to its on-disk location. Everything is read from the NUP and decompressed in memory.

Containers handled:
  BNSH (nn::gfx shader container, NVN2): inside *.bfsha (Shader/, Layout2Shader/, and the bfsha
      members of Shader/ApplicationPackage...sarc) and inside Effect/*.vsetb.byml. Parsed via
      bnsh.iter_programs (variations -> program -> per-stage code record).
  sharcb (agl 'BAHS'): members of Shader/ApplicationPackage...sarc and Lib/*/*.release.sarc.
      header +0x14 u32 string pool offset, +0x18 u32 code pool size, +0x1C u32 code pool offset;
      code records {u32 pool-relative code offset, u32 0, u32 control size, s32 record-relative offset of
      the NVN control section (magic 0x98761234, ver 0xc9)}; padded code size at ctrl+0x40.
  pck ('NDSR', Lib/agl/*.pck): not parsed; dumped programs are located by a 16-byte-stride scan.

Program bytes on disk: graphics = 0x80-byte SPH + SASS (+NOP padding), compute = SASS only.
A dumped program matches a program start s when dump == data[s+hdr : s+hdr+len(dump)] with
hdr = 0x80 for gfx, 0 for cs.
"""
import collections, fnmatch, hashlib, json, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nup_tool, unzs, sarc, bnsh

CTRL = b'\x34\x12\x76\x98\xc9\x00\x00\x00'


def sharcb_programs(d):
    """Code records: {u32 pool-relative code offset, u32 0, u32 control size (0xe0 graphics,
    0xf8 compute seen), s32 control offset relative to the record}. Controls may be shared."""
    pool_size, pool_off = struct.unpack_from('<II', d, 0x18)
    out = []
    for r in range(0x20, pool_off - 16, 4):
        code_rel, zero, csize, rel = struct.unpack_from('<IIIi', d, r)
        if zero or code_rel >= pool_size or not (0x40 <= csize <= 0x400):
            continue
        c = r + rel
        if 0 < c < pool_off and d[c:c + 8] == CTRL:
            size = struct.unpack_from('<I', d, c + 0x40)[0]
            out.append(dict(code=pool_off + code_rel, code_size=size, control=c, control_size=csize,
                            stage='?'))
    return out


def main():
    nup, dictdir, sd, out_index, out_stats = sys.argv[1:6]
    dicts = unzs.load_dicts(dictdir)
    # dumped programs
    progs = {}
    for f in sorted(os.listdir(sd)):
        if f.endswith('.header.bin') or not f.endswith('.bin') or f.startswith('rejected'):
            continue
        key, kind, addr = f[:-4].split('-')
        p = progs.setdefault(key, dict(kind=kind, file=f, addrs=[]))
        p['addrs'].append(int(addr, 16))
    code = {k: open(os.path.join(sd, p['file']), 'rb').read() for k, p in progs.items()}
    hdr = {}
    for k, p in progs.items():
        h = os.path.join(sd, p['file'][:-4] + '.header.bin')
        hdr[k] = open(h, 'rb').read() if os.path.exists(h) else None
    by_prefix = collections.defaultdict(list)
    short = collections.defaultdict(list)  # programs under 32 bytes (e.g. a lone EXIT): need the SPH too
    for k, b in code.items():
        if len(b) >= 32:
            by_prefix[(progs[k]['kind'], b[:32])].append(k)
        elif hdr[k] is not None:
            short[progs[k]['kind']].append(k)

    n = nup_tool.Nup(nup)
    romfs = n.packages[0]['romfs_dir']
    members = {rel: fi for rel, fi in n.walk(romfs)}

    def load(rel):
        d = n.read(members[rel])
        return unzs.decompress(d, dicts) if rel.endswith('.zs') else d

    # (label, data, program list, container type)
    sources = []
    def add_bnsh(label, d):
        ps = list(bnsh.iter_programs(d))
        if ps:
            sources.append((label, d, ps, 'BNSH'))
    for rel in sorted(members):
        if fnmatch.fnmatchcase(rel, 'Shader/*.bfsha.zs') or fnmatch.fnmatchcase(rel, 'Layout2Shader/*.bfsha.zs') \
                or fnmatch.fnmatchcase(rel, 'Effect/*.vsetb.byml.zs'):
            add_bnsh(rel, load(rel))
        elif fnmatch.fnmatchcase(rel, 'Shader/*.sarc.zs') or fnmatch.fnmatchcase(rel, 'Lib/*.release.sarc.zs'):
            outer = load(rel)
            for name, off, size in sarc.entries(outer):
                inner = outer[off:off + size]
                label = rel + '::' + name
                if inner[:4] == b'FSHA':
                    add_bnsh(label, inner)
                elif inner[:4] == b'BAHS':
                    sources.append((label, inner, sharcb_programs(inner), 'sharcb'))
                elif inner[:4] == b'NDSR':
                    sources.append((label, inner, [], 'pck'))
    print('sources', len(sources), file=sys.stderr)

    index = collections.defaultdict(list)
    stats = collections.OrderedDict()
    disk_unique = {'gfx': set(), 'cs': set(), 'sharcb_any_stage': set()}
    matched_unique = {'gfx': set(), 'cs': set()}
    gpu_bases = collections.defaultdict(collections.Counter)
    size_vs_ctrl = collections.Counter()
    for label, d, ps, ctype in sources:
        group = label.split('/')[0] if '::' not in label else label.split('::')[1].rsplit('.', 1)[-1]
        st = stats.setdefault(label, dict(container=ctype, program_refs=len(ps), unique_programs=0,
                                          code_bytes=0, matched_keys=0))
        seen = {}
        for p in ps:
            s = p['code']
            if s in seen:
                continue
            seen[s] = p
            blob = d[s:s + p['code_size']]
            kind = 'cs' if p['stage'] == 'compute' else ('gfx' if p['stage'] != '?' else 'sharcb_any_stage')
            p['hash'] = hashlib.sha1(blob).hexdigest()
            if kind in disk_unique:
                disk_unique[kind].add(p['hash'])
            st['code_bytes'] += p['code_size']
        st['unique_programs'] = len(seen)
        st['content_hashes'] = sorted({(('cs' if p['stage'] == 'compute' else 'gfx') if p['stage'] != '?' else 'any') + ':' + p['hash'] for p in seen.values()})
        # match dumps at program starts (both header conventions for sharcb, whose stage is unknown)
        starts = sorted(seen) if ctype != 'pck' else range(0, len(d) - 32, 16)
        for s in starts:
            p = seen.get(s)
            for kind, h in (('gfx', 0x80), ('cs', 0)):
                if p is not None and p['stage'] != '?' and (p['stage'] == 'compute') != (kind == 'cs'):
                    continue
                cands = list(by_prefix.get((kind, d[s + h:s + h + 32]), ())) + short.get(kind, [])
                for k in cands:
                    b = code[k]
                    if d[s + h:s + h + len(b)] != b:
                        continue
                    if ((p is not None and p['stage'] == '?') or len(b) < 32) and kind == 'gfx' \
                            and hdr[k] is not None and d[s:s + 0x80] != hdr[k]:
                        continue
                    hm = (d[s:s + 0x80] == hdr[k]) if (kind == 'gfx' and hdr[k] is not None) else None
                    ent = dict(romfs_path=label, offset=s, size=(p['code_size'] if p else len(b) + h),
                               code_offset=s + h, dumped_size=len(b), header_match=hm, container=ctype)
                    if p is not None:
                        ent['stage'] = p['stage']
                        ent['control_offset'] = p['control']
                        ctrl = d[p['control']:p['control'] + 0xe0]
                        ent['ctrl_program_size'] = struct.unpack_from('<I', ctrl, 0x38)[0]
                        ent['ctrl_regs'] = struct.unpack_from('<I', ctrl, 0x4c)[0]
                        size_vs_ctrl[(kind, ent['ctrl_program_size'] - h == len(b))] += 1
                        if ctype == 'BNSH':
                            ent['bnsh_offset'] = p['bnsh']
                            ent['variation'] = p['variation']
                            pl = bnsh.pool(d, p['bnsh'])
                            for a in progs[k]['addrs']:
                                gpu_bases[(label, p['bnsh'])][a - (s - pl['offset'])] += 1
                        matched_unique.setdefault(kind if p['stage'] != '?' else 'sharcb_' + kind, set()).add(p['hash'])
                    index[k].append(ent)
                    st['matched_keys'] += 1
    # tiny programs (< 32 bytes) cannot use the prefix index: record them as unlocated-by-design
    result = {}
    for k, p in progs.items():
        result[k] = dict(kind=p['kind'], dumped_size=len(code[k]),
                         gpu_addrs=[hex(a) for a in p['addrs']], locations=index.get(k, []))
    json.dump(result, open(out_index, 'w'), indent=1)
    found = collections.Counter(v['kind'] for v in result.values() if v['locations'])
    total = collections.Counter(v['kind'] for v in result.values())
    summary = dict(
        dumped_unique=dict(total), dumped_located=dict(found),
        unlocated=sorted(k for k, v in result.items() if not v['locations']),
        disk_unique_programs_by_content={k: len(v) for k, v in disk_unique.items()},
        disk_unique_matched_by_content={k: len(v) for k, v in matched_unique.items()},
        dump_size_equals_ctrl_program_size=[[list(k), v] for k, v in size_vs_ctrl.items()],
        gpu_base_per_bnsh={'%s@%#x' % k: {hex(b): c for b, c in v.most_common(6)} for k, v in gpu_bases.items()},
        per_source=stats)
    json.dump(summary, open(out_stats, 'w'), indent=1)
    print(json.dumps({k: summary[k] for k in ('dumped_unique', 'dumped_located', 'disk_unique_programs_by_content',
                                             'disk_unique_matched_by_content', 'dump_size_equals_ctrl_program_size')}))


if __name__ == '__main__':
    main()
