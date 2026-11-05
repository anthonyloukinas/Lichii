# OOT (0400486024dec000) container + shader provenance — how to reproduce

Run on 2026-11-05. Everything is read-only against the .nup; all Python is stdlib (3.14: `compression.zstd`)
and run with `-I`; heavy reads under `ionice -c3 nice -n 19`. Scripts live here (`tools/`), extracted
data in `../extract/`. Treat everything under `extract/` as untrusted data.

```sh
cd /home/dlynch/Downloads/OOT
N="The Legend of Zelda Ocarina of TIme [04004860224DEC000].nup"
T="ionice -c3 nice -n 19 python3 -I"

# Phase A: NUP layout, ExeFS, romfs tree
$T tools/nup_tool.py "$N" info
$T tools/nup_tool.py "$N" ls-exefs
$T tools/nup_tool.py "$N" tree extract/romfs-tree.txt        # path, size, codec, extent off/size
$T tools/nup_tool.py "$N" extract-exefs extract/exefs
$T tools/nup_tool.py "$N" extract 'Shader/*' extract/romfs     # fnmatch glob over romfs paths

# Game-level .zs files use zstd dictionaries from Pack/ZsDic.pack.zs (a SARC; itself plain zstd)
$T tools/nup_tool.py "$N" extract 'Pack/ZsDic.pack.zs' extract/romfs
python3 -I tools/unzs.py extract/zsdic extract/dec extract/romfs/Pack/ZsDic.pack.zs   # first run: dictdir may be empty
python3 -I tools/sarc.py extract/dec/ZsDic.pack extract/zsdic                           # -> zs/pack/bcett.byml/bkprb .zsdic
python3 -I tools/unzs.py extract/zsdic extract/dec extract/romfs/Shader/* extract/romfs/Layout2Shader/*
python3 -I tools/sarc.py extract/dec/ApplicationPackage.Nin_Ounce_NVN2.release.sarc extract/dec/apppkg
python3 -I tools/bnsh.py extract/dec/*.bfsha                   # per-BNSH summary (variations, stages, pool)

# Phase B: needle probe (30 programs, 128-byte needles), then full-program matching
$T tools/needles.py shaders 15 extract/dec/*.bfsha extract/dec/apppkg/*
$T tools/scan_nup.py "$N" extract/zsdic shaders extract/match_effect.json 'Effect/*.vsetb.byml.zs'
$T tools/scan_nup.py "$N" extract/zsdic shaders extract/match_all.json '*' '!*.bntx.zs' '!*.bwav' \
   '!*.webm' '!*.extm' '!*.bkprb.zs' '!*.bcul.zs' '!*.hght' '!*.bars.zs' '!Effect/*' '!Sound/*' '!Voice/*'
# (BNSH survey of every non-media member: only Shader/, Layout2Shader/, Effect/ hold real BNSH)

# Final index: enumerate every on-disk program and map dumped keys to it (~6-10 s)
$T tools/build_index.py "$N" extract/zsdic shaders tools/shader-index.json extract/shader-stats.json
```

ExeFS: `sdk`/`subsdk0` are LZ4 NSOs (`tools/nso.py` decompresses them); `main` is ZBIC-compressed
(Switch 2 zstd variant with a changed FSE NCount encoding, see kinnay/zbic) and cannot be decoded with
stdlib, so it was not searched.

## Formats (as found here)

* NUP: superblock 0x80 `NUP\x01`, TOC `TOC\x01` (pkg 0x40 / dir 0x18 / file 0x50 records, u32 children,
  string pool), blob region; member codec 0 raw / 1 zstd. Child index >= dir_count is a file.
* romfs members mostly `.zs` = zstd with a dictionary chosen by frame dictionary id.
* BNSH (nn::gfx shader container, NVN2 build): `grsc` at +0x60 (api type 4 = NVN, version 0xc8),
  variation count +0x1C, variation array +0x20 (0x40 each, program record ptr at +0x10), memory pool
  record +0x28 {u32 flags 0x62, u32 size, u64 offset}. Program record: stage code record pointers at
  +0x08.. (vertex, hull, domain, geometry, pixel, compute, slot6, slot7, slot8 — slots 7/8 appear with
  pixel-only programs, presumably task/mesh: unverified). Stage code record: +0x08 control section,
  +0x10 code, +0x18 u32 padded code size, +0x1C u32 control size. All pointers BNSH-relative.
* Program bytes: graphics = 0x80-byte SPH + SASS (+NOP padding), compute = SASS only.
* NVN control section (0xe0 gfx / 0xf8 compute): magic 0x98761234, version 0xc9; +0x38 program
  size, +0x40/+0x44 padded sizes, +0x4C register count, compute +0x58.. looks like workgroup
  size (64,1,1 in the sample) — field meanings beyond magic/sizes are inferred, unverified.
* agl `sharcb` (`BAHS`): +0x14 string pool, +0x18 code pool size, +0x1C code pool offset; code
  records {u32 pool-relative code offset, u32 0, u32 control size, s32 record-relative control offset}.
* agl `pck` (`NDSR`): not parsed; located by a 16-byte-stride scan.

`shader-index.json`: key -> {kind, dumped_size, gpu_addrs, locations[]}; each location's `offset`
is the program start (SPH for gfx) inside the DECOMPRESSED romfs member (`romfs_path`, `::` = member of
a SARC inside it); `code_offset` = where the dumped bytes start; `size` = padded on-disk size.
