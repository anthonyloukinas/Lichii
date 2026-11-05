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

Both knobs below are experiments, read once at startup from the environment. Neither has a
settings entry.

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
- `write_mark`: the active `EDEN_NX2_COMPUTE_WRITE_MARK` (`full` when unset).

If `pool_bytes` alternates from frame to frame in step with the flicker, the mechanism is confirmed.
When unset, the cost is one branch on a constant per collected write.
