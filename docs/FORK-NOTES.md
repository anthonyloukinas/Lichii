# Fork notes (daniel-lynch/Lichii)

Working notes for this fork of [zortos293/Lichii](https://github.com/zortos293/Lichii), kept
while chasing Ocarina of Time (`0400486024DEC000`) performance and rendering on desktop
NVIDIA. Upstream's default branch is `nx2`; ours mirrors it and carries nothing of our own.

## Branches

| Branch | What it is |
|---|---|
| `nx2` | clean mirror of upstream; sync with `git fetch upstream && git merge --ff-only upstream/nx2` |
| `feat/shader-precompile` | "Build all cached pipelines at boot" setting, "Precompile Shader Cache" game-list action, `--precompile-shaders` for both binaries |
| `fix/gather-lowering` | lowers the four texture-gather forms Ocarina used that were rejected (three cherry-picked from upstream `fix/oot-fog`, one LEA rule of ours); see `docs/oot-gather-lowering.md` |
| `fix/pipeline-eviction-setting` | "Evict unused pipelines" setting + `EDEN_NX2_PIPELINE_EVICT=0`; eviction re-translates pipelines and shows as cycling pop-in |
| `feat/shader-translation-cache` | per-stage translated-SPIR-V cache ("Cache translated shaders", on by default): boot preload 2.1 s → 0.13 s, evicted-pipeline rebuild 12 ms → 0.4 ms; see `docs/shader-translation-cache.md` |
| `fix/compute-storage-write-mark` | `EDEN_NX2_COMPUTE_WRITE_MARK=<bytes>` clamps how much of a native compute storage binding is marked GPU-written (upstream marks 8 MiB); `EDEN_NX2_DEFERRED_WRITE_LOG=1` per-frame traffic counters; see `docs/oot-white-petals.md` |
| `feat/frame-dump-burst` | `EDEN_NX2_FRAME_DUMP_COUNT=N` captures N consecutive frames; file-mode trigger re-arms. Since the merge of upstream #111 it lives in `FrameDumpSchedule` (`Step::Next`, tested in `frame_dump_schedule.cpp`) and the debug server's `capture_frame` takes `count` |
| `feat/frame-dump-buffer-cap` | `EDEN_NX2_FRAME_DUMP_BUFFER_CAP=<bytes>` caps host-buffer readbacks in a frame dump (the 2 GiB budget otherwise goes to 8 MiB storage bindings) |
| `test/oot-local` | integration of the above plus upstream `fix/oot-fog`, on top of `nx2`, for play builds |

Rules: one topic per branch, based on `nx2`; never force-push a shared branch; if the
integration branch has to be rebuilt it gets a new name or a forward merge. Upstream has its
own in-flight Ocarina branches (`fix/oot-boot`, `fix/oot-bloom`, `fix/oot-fog`,
`perf/oot-fast-draw`); check `git diff upstream/nx2...upstream/<branch> -- <file>` before editing
anything under `src/video_core`, and prefer additive hunks.

## Building on Fedora 44

```
sudo dnf install qt6-qtbase-devel qt6-qtbase-private-devel qt6-qtcharts-devel \
  autoconf automake libtool nasm ffmpeg-devel libdrm-devel libva-devel glslang libusb1-devel
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DUSE_CCACHE=ON \
  -DYUZU_CMD=ON -DYUZU_ROOM=OFF -DUSE_DISCORD_PRESENCE=OFF -DENABLE_UPDATE_CHECKER=OFF \
  -DYUZU_USE_BUNDLED_QT=OFF -DYUZU_USE_BUNDLED_FFMPEG=OFF -DYUZU_USE_BUNDLED_SDL3=ON \
  -DENABLE_LIBUSB=ON -DGLSLANGVALIDATOR=/usr/bin/glslangValidator \
  -DVulkanHeaders_FORCE_BUNDLED=ON -DVulkanUtilityLibraries_FORCE_BUNDLED=ON \
  -DYUZU_TESTS=OFF -DBUILD_TESTING=OFF
ninja -C build lichii eden-cli
```

Two upstream test files fail GCC `-Werror` (`src/tests/hid_core/sensor_info.cpp`,
`src/tests/video_core/texture_handles.cpp`); build with tests off or compile those two
objects by hand with `-Wno-sign-compare -Wno-unused-but-set-variable`.

**Portable mode trap:** a `user/` directory next to the binary makes it ignore
`~/.config/eden`. Running `eden-cli` from any other directory, including the repo root,
re-saves `~/.config/eden/sdl2-config.ini`. Run binaries from inside `build/bin/`, and keep a
test profile as `build/bin/user.portable-test/`, renaming it to `user/` only for a test.

## Settings that mattered for Ocarina (per game: right-click the title, Configure Game)

| Tab | Setting | Value |
|---|---|---|
| Adv. Graphics | GPU Mode | Fast |
| Adv. Graphics | Enable Reactive Flushing | off |
| Adv. Graphics | Build all cached pipelines at boot | on |
| Adv. Graphics | Evict unused pipelines | off |
| Ext. Graphics (Hacks) | Enable asynchronous shader compilation | on |

Each row has a use-global checkbox that must be unticked before the value takes.

Measured on an RTX 3080 Ti / i9-12900K: defaults gave 1013 mid-play pipeline compiles in
6.5 min costing 9.1 s on the GPU thread; with the settings above and 907 pipelines preloaded,
254 compiles costing 3.0 s. Steady state is about 36 fps median at 1x docked, with GPU ~66 %
and CPU ~37 % busy: the limit is the emulated GPU thread, which upstream's `perf/oot-fast-draw`
targets. Asynchronous shaders only defers Vulkan pipeline creation; the Ampere-to-Maxwell
lowering and SPIR-V translation still run on the GPU thread (~12 ms per pipeline), which is
what a translation cache (`feat/shader-translation-cache`, in progress) is for.

## Ocarina: what we know so far (2026-11-05)

- **White every-other-frame particle flicker and the PCIe flood are one bug.** A native compute
  program's storage pointer has no size, so the buffer cache binds `min(mapping, 8 MiB)` and marks all
  8 MiB GPU-written per dispatch (~130 skinning dispatches/frame). CPU writes into those pages are
  deferred to the next fence (stale per-object constants → white leaves) and the marks drive fence
  readbacks that saturated PCIe at 9–27 GB/s GPU→host. `EDEN_NX2_COMPUTE_WRITE_MARK=65536` removed
  the leaf flicker and cut the link traffic to ~0.2 GB/s. Upstream PR #104 attacks the fence side;
  nobody upstream has the write-mark finding yet. Proper fix: bound the mark from the shader's store
  extent (sketch in `docs/oot-white-petals.md`).
- GPU Mode Accurate + reactive flushing hides the flicker (fence timing) at ~20 fps cost, and shows
  bogus canopy shadows indoors instead — same stale-buffer class.
- The four rejected gather shaders are lowered three ways upstream now (`fix/oot-fog`, PR #102, ours);
  keep ours local.
- Shaders live on disk in BNSH containers and pools map contiguously into GPU memory
  (`tools/oot/` once published); lowering is <1 % of translation time, so pre-lowering from disk is
  not worth it — the translation cache is.

## Logging a session

Default `log_filter=*:Info` already records every pipeline build, the 10 s runtime compile
summary, the boot pipeline count and rejected shaders. The useful extras:

- `EDEN_NX2_SHADER_DUMP=<dir>` writes every captured native program (named by key); the
  corpus for offline lowering sweeps (`shader-sweep`, build with `-DENABLE_DEBUG_TOOLS=ON`).
- `EDEN_NX2_REJECT_HASH=<hash,...>` rejects programs by hash for A/B bisects; upstream
  issue #101 lists a three-hash set that removes the white glow at the cost of bloom.
- MangoHud for frame times: `MANGOHUD=1 MANGOHUD_CONFIG="fps,frametime,gpu_stats,cpu_stats,output_folder=<dir>,autostart_log=5,log_duration=0,log_interval=100"`.

The emulator keeps only the current log and one previous (`~/.local/share/eden/log/`); copy
`eden_log.txt` out after each session.

## Upstream issues to read first

#101 (frame-dump findings: overlay, bloom, missing vegetation, the gather rejections),
#98 (haze in docked mode), #99 (infinite launch screen; upstream `fix/oot-boot`),
#96 (Adreno pipeline builders), #87 (the merged boot/render PR that added the debug tooling).
We are not filing issues or PRs upstream for now.
