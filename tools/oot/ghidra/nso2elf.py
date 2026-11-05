#!/usr/bin/env python3
"""nso2elf.py - convert a Nintendo Switch NSO0 executable to an ELF64 AArch64 ET_DYN.

stdlib only. Usage: python3 -I nso2elf.py <in.nso> <out.elf> [--info]
The input is treated as untrusted data: it is only parsed, never executed.
Layout: file offset = vaddr + 0x1000 (one page for ELF header + phdrs), base vaddr 0,
section headers + .shstrtab appended at the end. Pick the image base at import time
(Ghidra: -loader-imagebase 7100000000).
"""
import hashlib, struct, sys

def lz4_block_decompress(src, out_size):
    dst = bytearray()
    i, n = 0, len(src)
    while i < n:
        tok = src[i]; i += 1
        lit = tok >> 4
        if lit == 15:
            while True:
                b = src[i]; i += 1; lit += b
                if b != 255: break
        dst += src[i:i + lit]; i += lit
        if i >= n: break                      # last sequence has literals only
        off = src[i] | (src[i + 1] << 8); i += 2
        if off == 0: raise ValueError("lz4: zero offset")
        ml = tok & 15
        if ml == 15:
            while True:
                b = src[i]; i += 1; ml += b
                if b != 255: break
        ml += 4
        start = len(dst) - off
        if start < 0: raise ValueError("lz4: offset before start")
        if off >= ml:
            dst += dst[start:start + ml]
        else:                                 # overlapping copy = repeat pattern
            pat = bytes(dst[start:])
            q, r = divmod(ml, off)
            dst += pat * q + pat[:r]
    if len(dst) != out_size:
        raise ValueError("lz4: got %d bytes, expected %d" % (len(dst), out_size))
    return bytes(dst)

def parse_nso(data):
    if data[:4] != b"NSO0": raise ValueError("not NSO0 (magic %r)" % data[:4])
    u32 = lambda o: struct.unpack_from("<I", data, o)[0]
    h = {"version": u32(4), "flags": u32(0xC)}
    segs = []
    for k, (ho, fso, hasho) in enumerate([(0x10, 0x60, 0xA0), (0x20, 0x64, 0xC0), (0x30, 0x68, 0xE0)]):
        foff, moff, size = struct.unpack_from("<III", data, ho)
        segs.append(dict(name=["text", "rodata", "data"][k], file_off=foff, mem_off=moff, size=size,
                         file_size=u32(fso), compressed=bool(h["flags"] >> k & 1),
                         check_hash=bool(h["flags"] >> (k + 3) & 1), sha256=data[hasho:hasho + 32]))
    h["segs"] = segs
    h["modname_off"], h["modname_size"] = u32(0x1C), u32(0x2C)
    h["bss_size"] = u32(0x3C)
    h["build_id"] = data[0x40:0x60]
    h["api_info"] = struct.unpack_from("<II", data, 0x88)
    h["dynstr"] = struct.unpack_from("<II", data, 0x90)
    h["dynsym"] = struct.unpack_from("<II", data, 0x98)
    for s in segs:
        raw = data[s["file_off"]:s["file_off"] + s["file_size"]]
        body = lz4_block_decompress(raw, s["size"]) if s["compressed"] else raw[:s["size"]]
        if len(body) != s["size"]: raise ValueError("%s: short segment" % s["name"])
        s["hash_ok"] = None
        if s["check_hash"]:
            s["hash_ok"] = hashlib.sha256(body).digest() == s["sha256"]
            if not s["hash_ok"]: raise ValueError("%s: SHA256 mismatch" % s["name"])
        s["body"] = body
    return h

DT = dict(NULL=0, PLTRELSZ=2, PLTGOT=3, HASH=4, STRTAB=5, SYMTAB=6, RELA=7, RELASZ=8, STRSZ=10,
          JMPREL=23, RELRSZ=0x23, RELR=0x24, INIT_ARRAY=25, FINI_ARRAY=26, INIT_ARRAYSZ=27, FINI_ARRAYSZ=28, GNU_HASH=0x6ffffef5)

def build_elf(h):
    t, r, d = h["segs"]
    img_end = d["mem_off"] + d["size"]
    img = bytearray(img_end)
    for s in h["segs"]:
        img[s["mem_off"]:s["mem_off"] + s["size"]] = s["body"]
    mod0 = struct.unpack_from("<I", img, 4)[0]
    if img[mod0:mod0 + 4] != b"MOD0": raise ValueError("MOD0 not found at 0x%x" % mod0)
    rel = struct.unpack_from("<6i", img, mod0 + 4)
    m = dict(zip(["dynamic", "bss_start", "bss_end", "ehfh_start", "ehfh_end", "module_object"],
                 [mod0 + x for x in rel]))
    h["mod0_off"], h["mod0"] = mod0, m
    # dynamic table
    dyn, o = {}, m["dynamic"]
    while o + 16 <= img_end:
        tag, val = struct.unpack_from("<qQ", img, o); o += 16
        if tag == 0: break
        dyn.setdefault(tag, val)
    dyn_size = o - m["dynamic"]
    h["dyn"] = dyn
    ro0 = r["mem_off"]
    symtab = dyn.get(DT["SYMTAB"], ro0 + h["dynsym"][0])
    strtab = dyn.get(DT["STRTAB"], ro0 + h["dynstr"][0])
    strsz = dyn.get(DT["STRSZ"], h["dynstr"][1])
    symsz = h["dynsym"][1] if ro0 + h["dynsym"][0] == symtab and h["dynsym"][1] else strtab - symtab
    nsyms = symsz // 24
    first_global = 1
    for k in range(nsyms):
        if img[symtab + 24 * k + 4] >> 4 == 0: first_global = k + 1
    h["nsyms"] = nsyms
    bss_lo = d["mem_off"] + d["size"]
    bss_hi = max(bss_lo + h["bss_size"], m["bss_end"])
    # named sub-ranges: (name, addr, size, type, flags, link_name, info, entsize, align)
    SHT_PROGBITS, SHT_STRTAB, SHT_RELA, SHT_DYNAMIC, SHT_NOBITS, SHT_DYNSYM = 1, 3, 4, 6, 8, 11
    SHT_INIT_ARRAY, SHT_FINI_ARRAY = 14, 15
    A, W, X = 2, 1, 4
    sub = [(".dynamic", m["dynamic"], dyn_size, SHT_DYNAMIC, A | W, ".dynstr", 0, 16, 8),
           (".dynsym", symtab, nsyms * 24, SHT_DYNSYM, A, ".dynstr", first_global, 24, 8),
           (".dynstr", strtab, strsz, SHT_STRTAB, A, None, 0, 0, 1)]
    if DT["RELA"] in dyn:
        sub.append((".rela.dyn", dyn[DT["RELA"]], dyn.get(DT["RELASZ"], 0), SHT_RELA, A, ".dynsym", 0, 24, 8))
    if DT["JMPREL"] in dyn:
        sub.append((".rela.plt", dyn[DT["JMPREL"]], dyn.get(DT["PLTRELSZ"], 0), SHT_RELA, A, ".dynsym", 0, 24, 8))
    if DT["RELR"] in dyn:
        sub.append((".relr.dyn", dyn[DT["RELR"]], dyn.get(DT["RELRSZ"], 0), 19, A, None, 0, 8, 8))
    if DT["INIT_ARRAY"] in dyn:
        sub.append((".init_array", dyn[DT["INIT_ARRAY"]], dyn.get(DT["INIT_ARRAYSZ"], 0), SHT_INIT_ARRAY, A | W, None, 0, 8, 8))
    if DT["FINI_ARRAY"] in dyn:
        sub.append((".fini_array", dyn[DT["FINI_ARRAY"]], dyn.get(DT["FINI_ARRAYSZ"], 0), SHT_FINI_ARRAY, A | W, None, 0, 8, 8))
    if m["ehfh_end"] > m["ehfh_start"]:
        sub.append((".eh_frame_hdr", m["ehfh_start"], m["ehfh_end"] - m["ehfh_start"], SHT_PROGBITS, A, None, 0, 0, 4))
    sub = [s for s in sub if s[2] > 0]
    sections = []
    for base, seg, flags in ((".text", t, A | X), (".rodata", r, A), (".data", d, A | W)):
        lo, hi = seg["mem_off"], seg["mem_off"] + seg["size"]
        inside = sorted([s for s in sub if lo <= s[1] < hi], key=lambda s: s[1])
        cur, k = lo, 0
        for s in inside:
            if s[1] < cur: print("warning: dropping overlapping %s" % s[0], file=sys.stderr); continue
            if s[1] > cur:
                sections.append((base if k == 0 else "%s.%d" % (base, k), cur, s[1] - cur, SHT_PROGBITS, flags, None, 0, 0, 16)); k += 1
            end = min(s[1] + s[2], hi)
            sections.append(s[:2] + (end - s[1],) + s[3:])
            cur = end
        if cur < hi:
            sections.append((base if k == 0 else "%s.%d" % (base, k), cur, hi - cur, SHT_PROGBITS, flags, None, 0, 0, 16))
    if bss_hi > bss_lo:
        sections.append((".bss", bss_lo, bss_hi - bss_lo, SHT_NOBITS, A | W, None, 0, 0, 16))
    # file assembly
    FOFF = 0x1000
    phdrs = []
    PT_LOAD, PT_DYNAMIC, PT_EH = 1, 2, 0x6474e550
    PF_X, PF_W, PF_R = 1, 2, 4
    phdrs.append((PT_LOAD, PF_R | PF_X, t["mem_off"] + FOFF, t["mem_off"], t["size"], t["size"], 0x1000))
    phdrs.append((PT_LOAD, PF_R, r["mem_off"] + FOFF, r["mem_off"], r["size"], r["size"], 0x1000))
    phdrs.append((PT_LOAD, PF_R | PF_W, d["mem_off"] + FOFF, d["mem_off"], d["size"], bss_hi - d["mem_off"], 0x1000))
    phdrs.append((PT_DYNAMIC, PF_R | PF_W, m["dynamic"] + FOFF, m["dynamic"], dyn_size, dyn_size, 8))
    if m["ehfh_end"] > m["ehfh_start"]:
        sz = m["ehfh_end"] - m["ehfh_start"]
        phdrs.append((PT_EH, PF_R, m["ehfh_start"] + FOFF, m["ehfh_start"], sz, sz, 4))
    shstr = bytearray(b"\0"); names = {}
    allnames = [s[0] for s in sections] + [".shstrtab"]
    for nm in allnames:
        if nm not in names: names[nm] = len(shstr); shstr += nm.encode() + b"\0"
    body_end = FOFF + img_end
    shstr_off = body_end
    shoff = (shstr_off + len(shstr) + 7) & ~7
    idx = {s[0]: k + 1 for k, s in enumerate(sections)}
    shdrs = [bytes(64)]
    for (nm, addr, size, typ, fl, link, info, ent, al) in sections:
        off = FOFF + addr if typ != SHT_NOBITS else FOFF + min(addr, img_end)
        shdrs.append(struct.pack("<IIQQQQIIQQ", names[nm], typ, fl, addr, off, size,
                                 idx.get(link, 0), info, al, ent))
    shdrs.append(struct.pack("<IIQQQQIIQQ", names[".shstrtab"], SHT_STRTAB, 0, 0, shstr_off, len(shstr), 0, 0, 1, 0))
    ehdr = struct.pack("<4sBBBBB7sHHIQQQIHHHHHH", b"\x7fELF", 2, 1, 1, 0, 0, bytes(7),
                       3, 183, 1, 0, 64, shoff, 0, 64, 56, len(phdrs), 64, len(shdrs), len(shdrs) - 1)
    out = bytearray(ehdr)
    for p in phdrs:
        out += struct.pack("<IIQQQQQQ", p[0], p[1], p[2], p[3], p[3], p[4], p[5], p[6])
    out += bytes(FOFF - len(out))
    out += img
    out += shstr
    out += bytes(shoff - len(out))
    for s in shdrs: out += s
    return bytes(out), sections

def main():
    a = [x for x in sys.argv[1:] if not x.startswith("--")]
    if len(a) != 2: sys.exit(__doc__)
    data = open(a[0], "rb").read()
    h = parse_nso(data)
    elf, sections = build_elf(h)
    ro = h["segs"][1]["body"]
    mn = ro[h["modname_off"]:h["modname_off"] + h["modname_size"]] if h["modname_size"] else b""
    print("file: %s (%d bytes) version=%d flags=0x%x" % (a[0], len(data), h["version"], h["flags"]))
    print("build id: %s" % h["build_id"].hex())
    for s in h["segs"]:
        print("  %-6s file_off=0x%08x file_size=0x%08x mem_off=0x%08x size=0x%08x lz4=%s sha256=%s" % (
            s["name"], s["file_off"], s["file_size"], s["mem_off"], s["size"], s["compressed"],
            {None: "not-checked", True: "OK"}[s["hash_ok"]]))
    print("  bss size=0x%x  module name field=%r" % (h["bss_size"], bytes(mn).split(b"\0")[0][-80:]))
    print("  dynstr(ro-rel)=0x%x+0x%x dynsym(ro-rel)=0x%x+0x%x api_info=0x%x+0x%x" % (h["dynstr"] + h["dynsym"] + h["api_info"]))
    print("MOD0 @0x%x: %s" % (h["mod0_off"], ", ".join("%s=0x%x" % kv for kv in h["mod0"].items())))
    print("dynamic tags: %s" % ", ".join("%#x=%#x" % kv for kv in sorted(h["dyn"].items())))
    print("dynsym entries: %d" % h["nsyms"])
    for s in sections: print("  section %-14s 0x%010x size 0x%x" % (s[0], s[1], s[2]))
    open(a[1], "wb").write(elf)
    print("wrote %s (%d bytes)" % (a[1], len(elf)))

if __name__ == "__main__":
    main()
