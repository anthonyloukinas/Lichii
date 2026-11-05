# Ocarina of Time texture gather rejections

Program ID: `0400486024DEC000`.

A play session log (`eden_log-20261105-012535.txt`) rejects four native programs, all at a
TLD4 (texture gather, native opcode `0xb63`). One is a fragment shader that does
percentage-closer shadow filtering. Its draws are skipped every frame. The other three are
compute shaders. Their dispatches are dropped. All four now lower. Every program that
lowered before produces byte-identical Maxwell code.

The shader changes are three commits from `origin/fix/oot-fog`, cherry-picked unchanged
(`2185d56086`, `9bf2966746`, `375c4a472b`). They were ported from
`upstream/scratch/oot-shader-lab`. One more change in this branch admits the fourth
program. Whether the visible lighting defect goes away still needs an in-game check.

## The four rejections

| Address | Kind | Log reason | Native form | Fix |
|---|---|---|---|---|
| `0x11492d8200` (`d5e90d353c783c23`) | fragment, `0x1650` | native texture gather mode | `TLD4.DC` 2D, R channel, F32, no offset | new lowering (`2185d56086`) |
| `0x11489bae00` (`9387db798781f081`) | compute, `0x2290` | per-tap gather control-flow proof | `TLD4.PTP` 2D, F32, under guard `!P1`, offsets from guarded `IMAD.MOV` | proof too conservative (`375c4a472b`) |
| `0x11489d5300` (`6f9367bd41dc0b58`) | compute, `0x340` | native texture gather mode | `TLD4.PTP.F16` 2D, R, G and B gathers | new lowering (`9bf2966746`) |
| `0x11489edc00` (`5bb12e1ef721b75f`) | compute, `0x2e0` | native texture gather mode | `TLD4.PTP.F16` 2D, as above | `9bf2966746`, then this branch's LEA rule for the gathers at `0x680` |

Field decoding follows the lowering's own layout. Bits 76-77 are the offset mode (1 is
AOFFI, 2 is PTP). Bit 78 is depth compare, bit 79 is F16 results, bits 87-88 select the
gathered component, and bits 81-83 hold the sparse predicate (7 means none).

### Shadow filter: `TLD4.DC` (fragment `d5e90d353c783c23`)

`180c7b63 30800e1c 001e4f0e 000f4200`: R12–R13 and R14–R15 ← gather(R24, R25), with bit 78 set,
descriptor `0x0e` in bank 2, and the reference in R28. The pre-change check
`insn.Bits(77, 2) && !per_tap` rejects any form with bit 78 set. The program has nine of
these gathers, in three branches of three, two and four gathers. All nine use R28 as the
reference. R28 = R8 × MUFU.RCP(R9) at `0x14d0`/`0x1550`, a perspective divide. The gathers
feed bilinear weights from `0x1700` on. This looks like ordinary PCF on a shadow map.

The 2D DC operand list (x, y, reference) packs 2+1, the same layout the TEX DC lowering
already uses: x and y in Ra and Ra+1, the reference in Rb. Maxwell TLD4 has DC at bit 50,
and the reference is the meta register after any offsets. The commit therefore copies Ra
and Ra+1 to R250 and R251 and Rb to R246, and sets bit 50. DC is rejected together with
bindless, array, AOFFI, PTP, F16, or an unpacked layout (bit 60 clear). None of those
combinations appears in the corpus.

### Directional per-tap gathers (compute `9387db798781f081`)

`06189b63 30007c10 001e2f0e 000f4600` is `@!P1 TLD4.PTP` (mode 2, F32, mask 0xf). The
taps are in R16 and R17, which are written immediately before by
`@!P1 IMAD.MOV.U32 R16, RZ, RZ, 0xfffeffff` and `@!P1 IMAD.MOV.U32 R17, RZ, RZ, 0xfffcfffd`.
The old proof requires the whole program to be straight-line, using 13 allowed opcodes.
This program fails on its first instruction (`0xab9`), and it has 82 branches. The
backward offset scan also accepted only unpredicated `MOV` (`0x802`) definitions.

PTP offsets must be constants because the SPIR-V backend emits them as `ConstOffsets`.
If they are not immediate, it logs "Not all arguments in PTP are immediate, ignoring" and
drops them, which produces wrong results without an error. `375c4a472b` replaces the
whole-program rule with a local proof. Walking backward from the gather:

- every instruction between the definition and the gather must not be a control-flow
  entry (`control_entries`), and the program needs `bounded_control`;
- every instruction in the walk must have a known register write count, which rules out
  clobbers, and a gather in the walk is checked against its packed or unpacked destinations;
- the offsets must come from `MOV` or `IMAD.MOV` (RZ, RZ, literal). They are either
  unguarded, or under the gather's own guard with no write to that predicate in between.
  Unknown opcodes count as a predicate write.

The proven literals are written to R248 and R249 without a predicate. If they were
predicated, the IR would merge them with the previous register value and they would stop
being immediates. The F32 path still runs the old straight-line check when the offsets
come from `MOV`. Its 24 gathers decode to 4-texel lines in eight directions, for example
taps (−1,−1)…(−4,−1) and (0,1)…(0,4), on components R and G.

Once the gathers lower, the program reaches two more native forms. `375c4a472b` ports
both from upstream. The first is `LEA.HI` with a negated base (`01017211 80000000 078ff0ff`,
bit 63), lowered as Maxwell IADD with neg_b. The second is `SURED.OR` (`0400159f 60008400
0310e000`); its op field (bits 87-89) is passed to Maxwell SURED bits 24-26. Both changes
only admit more encodings. Encodings that lowered before emit the same bits.

### F16 per-tap gathers (compute `6f9367bd41dc0b58`, `5bb12e1ef721b75f`)

`0a0c7b63 30003000 001eaf0d 000fc200` is TLD4.PTP with bit 79 set: four F32 results
rounded and packed as halves into Rd and the second destination. The old mode check
rejected `half && per_tap` outright. `9bf2966746` reuses the existing F16 packer (the
same one TEX/TLD4.F16 use) after a Maxwell TLD4.PTP. The offsets are R0/R1 (or R2/R3)
= `IMAD.MOV` 0x00010000 / 0x01010100, which gives taps (0,0), (1,0), (0,1), (1,1).

`5bb12e1ef721b75f` still failed after the cherry-picks, now at `0x680` with "per-tap gather
offset proof". Its second group of gathers reuses R2/R3, which are defined at `0x1e0` and
`0x230`. The backward walk reaches `LEA R8, R8, R7` (`08087211 00000007 078ff8ff`) at
`0x5a0`. `write_count` has no entry for `0x211`. LEA writes only Rd, so this branch adds
`0x211` with a count of 1 to the scan's own table. Nothing else uses that table. A carry
predicate that LEA might write is already treated as a guard clobber, because `0x211` is
not in the predicate table. A new test covers the captured LEA and gather, checks that the
offset vectors reach the IR as immediates (0,0,1,0 / 0,1,1,1), and checks that a LEA
writing R2 or R3 is still rejected.

## Sweep

Every native program in the session's dump, frozen at the moment of the run (1,155
programs; 988 have graphics headers), was lowered with `shader-sweep --lower-only`, before
and after. Graphics programs with a captured header used `--format native-header`.

| | Before (`77d4e19313`) | After |
|---|---:|---:|
| Lowered | 1,128 | 1,132 |
| Rejected: `native texture gather mode` | 3 | 0 |
| Rejected: `native per-tap gather control-flow proof` | 1 | 0 |
| Offline-only: `native tessellation stage buffer read mode` | 23 | 23 |
| Previously lowered programs whose output changed | | 0 (1,128 byte-identical) |

The 23 tessellation cases have tess-eval headers, and offline lowering does not get the
runtime's tessellation options. They do not appear in the game log. Without a header and
stage, all 23 lower, and their output is byte-identical before and after.

The intermediate run with only the cherry-picks lowered 1,131 programs and left
`5bb12e1ef721b75f` at `0x680`. Full SPIR-V compilation of the four new programs needs
captured context: the compute programs lack storage-pointer classifications and the
fragment program lacks constant-buffer values. None of the four was cached, because all
four were rejected. SPIR-V emission is covered by the unit tests below, not by this sweep.

The shader cache version is not changed. No previously lowered program in this corpus
changed, and the local v50 cache holds no lowered PTP gathers. The only emitted-code
change for a form that lowered before is a predicated F32 PTP gather with `MOV` offsets
in a straight-line program. Its offsets are now written without the predicate, so they
stay immediate. Such a gather's old cached code ignored its offsets, as the backend
warning shows. Keeping that cached code is therefore no worse than before.

## Tests

`tests` built in a separate build tree. The upstream `sensor_info.cpp` and
`texture_handles.cpp` objects were compiled by hand with their `-Werror` warnings relaxed;
those files are unchanged. Results: the `[ampere]` tag passes 231 cases, `[gather]` passes
10, and the new LEA case passes. The full run passes 637 of 639 cases. The two failures are
the `nx2_video_render.cpp` decode comparisons, which use no shader code.

## Not established

- In-game rendering. The PCF fragment shader's draws were skipped before this change. If
  that pass shades the shadowed lighting, it is a strong candidate for the defect. The
  compute programs were dropped dispatches: a 4-tap directional search and two 2×2 RGB
  F16 gathers. They may also feed lighting.
- The `format_lookup_table` (45) and `tic_limit` (22) assertions occur while pipelines
  are built (`ReadTextureInfo`), mostly from 241 to 254 s, the same loading burst as the
  rejections. They fall back to A8B8G8R8 and to reading past the limit. As in the
  [Kirby investigation](kirby-white-world.md), format 0 is the descriptor's INVALID
  encoding. Those asserts are not shown to cause a lighting fault.
