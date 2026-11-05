# OoT (Switch 2) main -> Ghidra: reproduction notes

Inputs (read-only): `/home/dlynch/Downloads/OOT/extract/exefs/{main,rtld,sdk,subsdk0,main.npdm}`.
Ghidra 12.2 PUBLIC at `/home/dlynch/opt/ghidra_12.2_PUBLIC`, project `/home/dlynch/ghidra-projects/oot-nx2`.

## 1. Route A: NSO -> ELF (`nso2elf.py`, stdlib only)

    cd /home/dlynch/Downloads/OOT/ghidra
    for f in rtld subsdk0 sdk main; do
      python3 -I nso2elf.py ../extract/exefs/$f elf/$f.elf | tee convert-$f.txt
    done
    readelf -hlSd elf/sdk.elf

Works for rtld, subsdk0, sdk (LZ4 segments, SHA256 verified). **Fails for main**: main's
NSO flags are 0xff (bits 6/7 are new) and each segment starts with magic `ZBIC` instead of
being raw LZ4. ZBIC (firmware 22.0.0+) is zstd with a different magic and FSE normalized-count
tables encoded with Binary Interpolative Coding (see ZbicUtil.java in Ghidra-Switch-Loader).
Not ported to Python in this pass -> Route B for main.

DT_RELR: the modules use packed relative relocs (tags 0x23/0x24/0x25); nso2elf emits a .relr.dyn (SHT_RELR) section for them.

## 2. Route B: Ghidra-Switch-Loader extension

    gh release download -R Adubbz/Ghidra-Switch-Loader \
      -p 'SwitchLoader-1.6.1-0f38957-Ghidra_12.1.2.zip' -D /home/dlynch/Downloads/OOT/ghidra/dl/
    # sha256 c37b8e8360a5071b47f7a56e2bf467b00e4a6eef884cc3dede17b5ad65298f96
    unzip -q dl/SwitchLoader-1.6.1-0f38957-Ghidra_12.1.2.zip -d dl/unpacked
    mkdir -p ~/.config/ghidra/ghidra_12.2_PUBLIC/Extensions
    cp -r dl/unpacked/SwitchLoader ~/.config/ghidra/ghidra_12.2_PUBLIC/Extensions/

extension.properties says version=12.1.2; Ghidra 12.2 headless loaded it unmodified
(test import of rtld with `-loader SwitchLoader` succeeded). No edit made.
In the GUI, if it is not active: File > Configure > Extensions, tick SwitchLoader, restart.

## 3. Import + analysis (main first, one at a time, nice'd)

main is imported directly from the NSO by SwitchLoader (Route B) -- there is no main.elf.
SwitchLoader decompresses ZBIC itself (ZbicUtil: rewrites ZBIC frames into standard zstd
frames, then zstd-jni decompresses). Image base 0x7100000000 (loader default).

Run 1 (02:37, GHIDRA_HEADLESS_MAXMEM=16G, no cpu cap) died at ~03:17 in the system-wide OOM
reboot; nothing saved. Log kept as headless-main.crash1.log. Its 30 "Unable to read bytes"
decompiler warnings are at 0x7104e2xxxx (inside the uninitialized .bss block
0x7104D46B80-0x7104E9A4C8, which the decompiler cannot read by design) plus a few bogus
constant pointers (ram:00001000, 0x7303ed31e6) -- not a mapping bug.

Run 2 (capped). Note: analyzeHeadless honours GHIDRA_HEADLESS_MAXMEM / GHIDRA_MAXMEM, not a
bare MAXMEM (MAXMEM_DEFAULT is hard-coded 2G in the script); both are set below.
Check first that the game is not running: `pgrep -af '[b]in/lichii'` must print nothing.

    MAXMEM=6G GHIDRA_MAXMEM=6G GHIDRA_HEADLESS_MAXMEM=6G nice -n 19 \
      /home/dlynch/opt/ghidra_12.2_PUBLIC/support/analyzeHeadless \
      /home/dlynch/ghidra-projects oot-nx2 \
      -import /home/dlynch/Downloads/OOT/extract/exefs/main -loader SwitchLoader \
      -max-cpu 4 -analysisTimeoutPerFile 5400 \
      -scriptPath /home/dlynch/Downloads/OOT/ghidra/scripts \
      -postScript OotAnchors.java /home/dlynch/Downloads/OOT/ghidra/anchors-main.txt \
      -log /home/dlynch/Downloads/OOT/ghidra/headless-main.log

Post-script scripts/OotAnchors.java writes counts + keyword strings + nvn*/nn:: symbols.

### Run 2 result (2026-11-05 03:24-04:16)
exit 0, wall 52:02, peak RSS 4.3 GB (time-main.txt), analysis Total Time 3090 s, no timeout.
"Analysis succeeded", "Post-analysis succeeded", "Save succeeded for: /main (oot-nx2:/main)".
Counts (anchors-main.txt): 185,400 functions (2,633 named), 1,146 external functions,
66,059 defined strings, 947 keyword string hits. Project dir 1.2 GB.
Log noise: 3x "Failed to find .got section" (NSO0Adapter, harmless), 6x VarnodeContext
"out of address spaces", 85 DecompileCallback warnings, 11 DemangledAddressTable warnings.
NVN is not imported by name: main imports only nvnBootstrapLoader; the rest are resolved at
runtime through nvnDeviceGetProcAddress using the 738 nvn* name strings in .rodata.

Open in GUI: /home/dlynch/opt/ghidra_12.2_PUBLIC/ghidraRun -> File > Open Project ->
/home/dlynch/ghidra-projects/oot-nx2.gpr -> double-click "main". (SwitchLoader must be
enabled in the GUI too: File > Configure > Extensions.)

## 4. Targeted RE pass (2026-11-05, NVN table / storage sizes / DLSS / frame rate)
Host rules: `-process main -noanalysis`, 6G heap, nice 19, -max-cpu 4, one JVM, game idle.
Scripts: scripts/NvnTable.java (slot recovery + labels), scripts/OotProbe.java (spec-driven
xref/decompile probe; specs in specs/*.txt). Outputs in out/ (decomp/<entry>.c per function).

Run 1:

    pgrep -f '[b]in/lichii'   # must be empty (the [b] stops pgrep matching its own shell)
    GHIDRA_HEADLESS_MAXMEM=6G GHIDRA_MAXMEM=6G nice -n 19 \
      /home/dlynch/opt/ghidra_12.2_PUBLIC/support/analyzeHeadless \
      /home/dlynch/ghidra-projects oot-nx2 -process main -noanalysis -max-cpu 4 \
      -scriptPath /home/dlynch/Downloads/OOT/ghidra/scripts \
      -postScript NvnTable.java /home/dlynch/Downloads/OOT/ghidra/out \
      -postScript OotProbe.java /home/dlynch/Downloads/OOT/ghidra/specs/run1.txt /home/dlynch/Downloads/OOT/ghidra/out \
      -log /home/dlynch/Downloads/OOT/ghidra/out/run1.log > out/run1.stdout 2>&1

Run 1 result: 1:13 wall, 2.8 GB RSS. Table recovery mis-paired (the store for name N comes
after the string load of name N+1) -> only 1 slot. Fixed in NvnTable.java (pair the store
with the string loaded before the call), plus GOT-entry labels (`got_pfn_*`): the loader and
every caller reach the pfn global through a GOT entry (adrp/ldr x8,[x8,#got]; ldr/str [x8]).
String/symbol probes from run 1 are in out/probe-run1.txt.

Run 2: same command with specs/run2.txt and -log out/run2.log (> out/run2.stdout).

Run 2 result: 735/735 resolvable names mapped (the 3 others: nvnBootstrapLoader and two
debug-layer names used by FUN_710346be50). Table = 735 contiguous 8-byte slots
0x7104d46f00.., labelled pfn_<name>; each slot's GOT entry labelled got_pfn_<name>.
Probes: out/probe-run2.txt, out/readers.tsv (awk over probe-run2.txt).

Run 3: specs/run3.txt (OotProbe gained `ref` and `strre` commands), 12G/-max-cpu 12 when
the game is idle (caps raised by Daniel 2026-11-05), log out/run3.log.

Run 3 result: out/probe-run3.txt, ~530 decompiles in out/decomp. Finding: most NVN calls are
untyped, so the decompiler drops arguments of calls through 20-byte wrappers.
Run 4: -postScript NvnTypes.java (prototypes for key pfn slots/GOT entries + 3 engine wrappers),
then OotProbe specs/run4.txt into out2/ (fresh decomp dir so typed output replaces untyped).
Helper: tools/calls.py <name-regex> <files> lists got_pfn_ calls with arguments.
Run 4 first attempt: NvnTypes ClassCastException (addDataType returns FunctionDefinitionDB); fixed, out2-untyped kept; rerun into out2.
Run 5: specs/run5.txt -> out2 (present interval, oe handler, ndsr callers, Ssbo names).
Run 6: specs/run6.txt -> out2 (transient GPU ring allocator users, frame-rate setter callers, network vtables).

### Pass summary (2026-11-05 04:20-05:10)
Runs 1-6 each 1-2 min wall, peak RSS <= 3 GB; game idle throughout (pgrep check before each).
Run 1-3: 6G/4 cpu; runs 4-6: 12G/12 cpu (raised caps).
Persistent project changes: 735 `pfn_*` + 735 `got_pfn_*` labels, typed slots (NvnTypes), 3 wrapper
signatures (0x71009cba40, 0x710129d7f0, 0x710129e080), data type category /nvn. Nothing else modified.
Findings: findings-nvn-table.md, findings-storage-sizes.md, findings-dlss.md,
findings-framerate-quality.md, findings-indirect-mesh.md. Slot map: nvn-slots.txt.
