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
  skinning pool seen in the dump (a fixed range, only meaningful for that scene).
- `invalidations`: `InvalidateGPUCache` calls (the points where collected writes are applied).
- `applied_ranges` / `applied_bytes` / `applied_pool_bytes`: collected writes applied by those calls.
- `write_mark`: the active `EDEN_NX2_COMPUTE_WRITE_MARK` (`full` when unset).

If `pool_bytes` alternates from frame to frame in step with the flicker, the mechanism is confirmed.
When unset, the cost is one branch on a constant per collected write.
