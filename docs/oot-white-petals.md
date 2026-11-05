# Ocarina of Time: white falling leaves (experiment knobs)

The falling leaves outside Link's house flash pure white every other frame at GPU Mode Fast with
reactive flushing off. Accurate plus reactive flushing hides it at about 20 fps. The suspected
mechanism (supported by a frame dump, not proven):

1. A native (Switch 2) compute program's storage pointer has no size word, so the buffer cache binds
   `min(layout, 8 MiB)` (`BufferCache::StorageBufferBinding`).
2. `BindHostComputeStorageBuffers` marks the whole binding GPU-written, although the skinning
   dispatches write only 17–45 KB.
3. The CPU then writes per-object constants into the over-marked pages of the same skinning pool.
   `BufferCache::OnCPUWrite` sees GPU-modified pages and the write is only collected
   (`core/memory.cpp`); it reaches the host buffer at the next fence, MemOpB or invalidate
   (`GPU::InvalidateGPUCache`). Draws recorded before that read the old host copy.
4. Which frames' data lands in marked pages depends on pool parity, hence every other frame.

Field results (2026-11-05):

- With PR #103 (inline page writes on by default) at Fast with reactive flushing off, the
  flicker is still there. That fits: #103 changes how a collected write is applied, not when.
- A second symptom, probably the same class of stale buffer: at Accurate with reactive flushing
  on, the leaves are fine but bogus dappled "canopy" shadows cover the floor and root wall inside
  Link's tree house. Switching to Fast live removes the shadows and brings the flicker back. The
  shadow filter is pixel shader `d5e90d353c783c23` (a depth-compare gather reading a 4096²
  shadow map). Accurate also disables the GPU split, delays fences, makes DMA reads safe and
  refreshes macro parameters, so different pages go stale under each mode.
- Transfer volume: `nvidia-smi dmon` over about 16 s of gameplay at Fast shows `txpci`
  (GPU to host) at 9–27 GB/s (PCIe 4.0 x16 saturated) against 0.24–0.65 GB/s `rxpci`, and
  5–30 MB/s when idle. Run (a) below rules out buffer-cache downloads as the source; the
  staging and native-dispatch counters are there to find it.

Both knobs below are experiments, read once at startup from the environment. Neither has a
settings entry.

## The PCIe readback (performance)

Run (b), `EDEN_NX2_COMPUTE_WRITE_MARK=65536` at Fast, 6,632 frames at about 35 fps, field verdict
"runs way better":

| | full mark (run a) | 64 KiB mark (run b) |
|---|---|---|
| `txpci` (GPU to host), nvidia-smi | 9–27 GB/s | mean 0.17 GB/s, max 0.93 |
| pool bytes collected / applied per frame | 136 / 96 KB (40 KB short) | 131 / 125 KB (6 KB short) |
| fence downloads written back per frame | 16 MB | 2.8 MB |
| pool share of those | 4.9 MB | 1.5 MB |

The chain, as far as it is measured:

1. 8 MiB mark: each native dispatch's storage binding is marked GPU-written over up to 8 MiB
   (about 130 dispatches a frame, 17–45 KB really written each). Measured.
2. Pages counted GPU-written: `gpu_modified_ranges` and the tracker's GPU bits cover the CPU's
   own data in the pool. Measured in the dump.
3. GPU-to-host traffic: every range in a download-at-fence region is copied by the GPU into
   host staging at each fence (`CommitAsyncFlushesHigh`). That copy is GPU-to-host and shows as
   `txpci`. **Leading candidate, not yet measured:** the next dispatch's 8 MiB mark subtracts
   the range from `pending_download_ranges` again, so `PopAsyncBuffers` writes back little (the
   16 MB above) while the GPU has copied far more. `fence_copy_bytes` measures the copy itself.
   The other GPU-to-host candidate is the CPU reading host-visible device-local (ReBAR) buffers
   directly in `DownloadBufferMemory`'s mapped branch (`mapped_read_bytes`); run (a) had sync
   downloads, which include that branch, near 0, so it is the weaker candidate.
4. Link saturated: 9–27 GB/s on PCIe 4.0 x16. The clamp cuts it by about 100x. Measured.

Upstream PR #104 (`0bf0f834b9`, "download only the Preflushable pages of a GPU write at fences",
`EDEN_NX2_EXACT_PREFLUSH=0` reverts it) attacks step 3 from the download side: a GPU write that
touched any Preflushable page used to be queued whole, so an 8 MiB mark with one CPU-read page
was copied back at every fence. It now queues only the Preflushable pages. The clamp attacks
step 1, the write mark itself, which also decides which CPU writes are deferred (the leaves). The
two are complementary. With both in, what is left to characterise is Daniel's residual: leaves
fixed, occasional flashing remains, fog better but blown out.

The `DeviceLocal` staging requests in `vk_compute_pass.cpp` and the BC re-encode in
`vk_texture_cache.cpp` are GPU-only scratch (a shader writes, a later GPU pass reads) and do not
cross PCIe.

## Toward a proper fix

The clamp is a guess about how far a dispatch writes. What a real bound could come from:

- **The guest binding.** NVN binds storage with an address and a size, but a native Switch 2
  program reads a bare 64-bit pointer from its constant buffer; the word after it is another
  parameter, not a size (`StorageBufferBinding`). No size is available here.
- **BNSH reflection.** `tools/bnsh.py` and `shader-index.json` locate the control section and the
  code; their uniform and sampler tables are known. A storage-block size table has not been found.
  In nn::gfx reflection (Switch 1), storage ("unordered access") buffers carry names and slots
  but no sizes, so even if found it would not bound them. Unverified for NVN2.
- **The shader IR (recommended).** `global_memory_to_storage_buffer_pass.cpp` already tracks
  each global access back to its constant-buffer pointer and folds immediate offsets
  (`imm_offset`), but `StorageBufferDescriptor` keeps only `cbuf_index`, `cbuf_offset`, `count`
  and `is_written`. For each written storage buffer, record a store extent:
  `max_const_offset + access_size` when every store's offset is constant, or
  `stride * invocation_index + const` when it is affine in the global invocation id, else
  unbounded. At dispatch, the QMD's grid and block sizes turn that into bytes, and
  `BindHostComputeStorageBuffers` marks `min(binding, extent)`. Skinning writes one palette per
  invocation, which is affine. Anything unbounded keeps the 8 MiB mark, so nothing regresses.
- **A per-game heuristic** (this knob as a setting) is the fallback if the IR route stalls. It
  is wrong for any title whose native compute really writes further than the cap.

## Evidence and related work

The evidence, so this page stands alone. It comes from a frame dump of Ocarina of Time
(`0400486024DEC000`, the path outside Link's house, frame 1740: 182 dispatches, 2,062 draws):

- All 98 skinning palette outputs are bound with `bound_size = 8388608`. Each writes about
  17–45 KB.
- 39 compute inputs that the CPU wrote are already flagged GPU-modified when bound. The only
  thing covering them is an earlier dispatch's 8 MiB mark. 9 of them carry the flag over from the
  previous frame.
- At draw time, 36–95% of the 32-bit words the draws read from the pool are the CPU's
  `0x6f6f6f6f` fill (7.41e28 as a float).

Related upstream work:

- PR #104 (merged into nx2) is the nearest. It narrows fence downloads to Preflushable pages but
  leaves the write mark as it is (see above).
- PR #103 (merged) changes how a collected write is applied, not when. The leaf flicker survived
  it in the field.
- PR #102 (open) lowers depth-compare gathers. It is relevant to the indoor shadow shader's
  translation, not to the write mark.

No upstream change bounds the write mark of a native compute storage binding. That is the part
this experiment adds.

## `EDEN_NX2_COMPUTE_WRITE_MARK=<bytes>`

Limits how much of a **native** compute storage binding is marked GPU-written to
`min(binding size, <bytes>)`. The binding size itself does not change: the shader can still read
and write all of it, only the cache's claim about what was written shrinks. Multi-range bindings
spend the limit across their segments in order. Maxwell (Switch 1) programs and graphics bindings
are untouched. Accepts decimal or `0x` hex.

- Unset: current behaviour (the whole binding, up to 8 MiB).
- `65536`: mark at most 64 KiB from the binding's base.
- `0`: do not mark native compute storage bindings written at all. This breaks anything that
  reads compute output back on the CPU or relies on the cache knowing the GPU owns those bytes:
  stale CPU uploads can overwrite compute results, and CPU reads of compute output see old data.

If the leaves stop flashing with a small limit on Fast settings, the over-mark is the cause.

A/B ladder for the residual, on a build with PR #104 (Fast, reactive flushing off,
`EDEN_NX2_DEFERRED_WRITE_LOG=1` on every run):

| Mark | Reading |
|---|---|
| unset | baseline with PR #104 alone |
| `262144` | if the residual flashing goes away compared with 64 KiB, some dispatches write past 64 KiB: under-marking (the CPU reads stale data from pages the GPU really wrote) |
| `65536` | run (b) |
| `16384` | if flashing gets worse than at 64 KiB, under-marking; if it stays the same, the residual is leftover deferral (`pool_bytes` greater than `applied_pool_bytes`) |
| `0` | nothing marked: everything that relies on compute output read back by the CPU breaks |

Under-marking shows as worse flashing and broken compute-fed geometry as the mark shrinks.
Leftover deferral does not change with the mark below about 64 KiB, and the collected-minus-applied
pool bytes stay above zero.

## `EDEN_NX2_DEFERRED_WRITE_LOG=1`

Once per presented frame, logs one line (`Render_Vulkan`, `NX2 deferred writes ...`) with the
totals since the previous line:

- `collected_writes` / `collected_bytes`: guest CPU writes that were only collected (deferred).
- `pool_writes` / `pool_bytes`: the part of those inside device `0x2b300000–0x2b900000`, the
  skinning pool seen in the dump (only meaningful for that scene).
  `EDEN_NX2_DEFERRED_WRITE_RANGE=<begin>-<end>` (device addresses, hex or decimal) buckets another
  range instead, for example the light-matrix uniform buffer the shadow draw reads, taken from a
  frame dump's `device_addr`. The shadow map itself is an image and is not counted here unless
  it is written by the CPU.
- `invalidations`: `InvalidateGPUCache` calls (the points where collected writes are applied).
- `applied_ranges` / `applied_bytes` / `applied_pool_bytes`: collected writes applied by those calls.
- `fence_downloads` and `sync_downloads` (`ranges/bytes/pool_bytes`): buffer-cache bytes copied
  from host buffers back to guest memory. Fence downloads land when a fence pops;
  synchronous ones (`BufferCache::DownloadMemory`, the page-download branch of `WriteMemory`, a CPU
  read of GPU-written memory) stall on the host GPU, and `sync_download_calls` counts those
  stalls. An 8 MiB write mark makes every CPU read or write of a marked page a candidate for a
  download, so the clamp may cut transfer volume as well as fix the leaves. Texture-cache
  downloads are not counted.
- `staging(requests/bytes) upload= download= device_local=`: every `StagingBufferPool::Request`,
  which carries all buffer- and texture-cache staging (texture uploads and downloads included).
  It does not see CPU reads or writes through mapped device-local (ReBAR) memory: the buffer
  cache's own buffers, and the stream buffer.
- `buffer_upload_bytes`: buffer-cache uploads of CPU-modified pages (`SynchronizeBuffer`);
  `native_storage_upload_bytes`: the part done while binding native compute storage buffers.
- `native_dispatches`, `native_storage_bindings`, `native_bound_bytes`, `native_marked_bytes`:
  native compute dispatches this frame, their storage bindings, the bytes those bindings cover
  (up to 8 MiB each) and the bytes marked GPU-written for them after the clamp.
- `fence_copies`, `fence_copy_bytes`, `fence_copy_pool_bytes`: fence downloads as the GPU
  copies them into host staging, before superseded ranges are dropped.
- `mapped_read_bytes`: bytes the CPU read straight from host-visible device-local buffers in
  `DownloadBufferMemory` (also counted in `sync_downloads`).
- `write_mark`: the active `EDEN_NX2_COMPUTE_WRITE_MARK` (`full` when unset).

Field run (a), 3,808 frames at Fast: collected writes every frame, not alternating (about 290 KB
per frame, 136 KB in the pool, even and odd frames the same); `applied_pool_bytes` about 96 KB per
frame; fence downloads about 16 MB per frame and sync downloads about 0. So the 20–27 GB/s
`txpci` is not buffer-cache downloads.

If `pool_bytes` alternates from frame to frame in step with the flicker, the mechanism is confirmed.
When unset, the cost is one branch on a constant per collected write.
