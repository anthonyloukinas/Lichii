// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <span>
#include <string>
#include <utility>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include "common/common_types.h"
#include "common/logging.h"

// EDEN_NX2_MESH_COUNT_TRACE=1: why task/mesh draws reach vkCmdDrawMeshTasksEXT with no task
// groups. A zero written to vertex_buffer.count (the task count) with mesh control on is logged
// with the 3D engine's last macro DMA read: its words, whether its source was GPU-modified, and
// how many collected CPU writes the flush before the read applied over it. The back then logs the
// host buffer's words at that source (same seq). Lines are capped; a summary follows every second.
namespace VideoCommon::Nx2MeshCountTrace {

inline const bool g_enabled = [] {
    const char* value = std::getenv("EDEN_NX2_MESH_COUNT_TRACE");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}();

inline constexpr u32 MaxLines = 200;
inline constexpr u32 MaxDrawLines = 40;

/// The last macro DMA read (MME_DMA_READ, MME_DMA_READ_FIFOED) of a 3D engine.
struct DmaRead {
    u64 serial{}; ///< 0: none yet
    u64 call{};   ///< EngineState::calls when it was read
    u64 source{};
    u64 device{}; ///< 0: unmapped
    u32 words{};
    u32 sampled{}; ///< valid words in `first`
    std::array<u32, 8> first{};
    bool fifoed{};
    bool dirty{};    ///< IsMemoryDirty(BufferCache): the buffer cache held GPU-written bytes
    bool flushed{};
    bool gpu_page{}; ///< The first page is marked GPU-written (Nx2IsGpuWritten)
    u32 applied_wait{};    ///< Collected CPU writes applied over the source before the flush
    u32 applied_flush{};   ///< Collected CPU writes the flush applied, anywhere
    u32 applied_overlap{}; ///< Of those, the ones over the source
};

struct EngineState {
    DmaRead dma;
    u64 calls{}; ///< Macro calls run
    u32 macro_method{};
    u32 macro_start{};
    bool in_macro{};
};

struct State {
    std::atomic<u32> lines{};
    std::atomic<u32> draw_lines{};
    std::atomic<u64> sequence{};
    std::atomic<u64> last_zero_seq{};
    std::atomic<u64> reads{};
    std::atomic<u64> mesh_zero{};
    std::atomic<u64> mesh_nonzero{};
    std::atomic<u64> zero_writes{};
    std::atomic<u64> dma_dirty{};
    std::atomic<u64> dma_flushed{};
    std::atomic<u64> dma_applied{};
    std::atomic<u64> dma_applied_overlap{};
    // The source of the DMA read in progress (device addresses) and collected CPU writes the
    // GPU thread applied since the session began, and the ones over that source.
    std::atomic<u64> watch_begin{};
    std::atomic<u64> watch_end{};
    std::atomic<u64> applied{};
    std::atomic<u64> applied_overlap{};
    std::atomic<s64> next_summary_ns{};
};

inline State& Get() {
    static State state;
    return state;
}

inline bool TakeLine() {
    return Get().lines.fetch_add(1, std::memory_order_relaxed) < MaxLines;
}

inline s64 NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

inline void Summary() {
    State& s = Get();
    const s64 now = NowNs();
    s64 next = s.next_summary_ns.load(std::memory_order_relaxed);
    if (now < next) {
        return;
    }
    if (next == 0) {
        s.next_summary_ns.compare_exchange_strong(next, now + 1'000'000'000LL,
                                                  std::memory_order_relaxed);
        return;
    }
    if (!s.next_summary_ns.compare_exchange_strong(next, now + 1'000'000'000LL,
                                                   std::memory_order_relaxed)) {
        return;
    }
    const auto take = [](std::atomic<u64>& counter) {
        return counter.exchange(0, std::memory_order_relaxed);
    };
    const u64 zero = take(s.mesh_zero);
    const u64 nonzero = take(s.mesh_nonzero);
    const u64 zero_writes = take(s.zero_writes);
    const u64 reads = take(s.reads);
    const u64 dirty = take(s.dma_dirty);
    const u64 flushed = take(s.dma_flushed);
    const u64 applied = take(s.dma_applied);
    const u64 overlap = take(s.dma_applied_overlap);
    LOG_INFO(HW_GPU,
             "NX2 mesh count summary: mesh draws zero={} nonzero={} zero_count_writes={} | macro "
             "DMA reads={} gpu_modified={} flushed={} flush_applied_cpu_write={} "
             "flush_applied_over_source={} | lines {}/{}",
             zero, nonzero, zero_writes, reads, dirty, flushed, applied, overlap,
             std::min(s.lines.load(std::memory_order_relaxed), MaxLines), MaxLines);
}

/// GPU thread (InvalidateGPUCache): a collected CPU write was applied to the GPU caches.
inline void NoteApplied(u64 address, u64 size) {
    State& s = Get();
    s.applied.fetch_add(1, std::memory_order_relaxed);
    const u64 begin = s.watch_begin.load(std::memory_order_relaxed);
    const u64 end = s.watch_end.load(std::memory_order_relaxed);
    if (address < end && begin < address + size) {
        s.applied_overlap.fetch_add(1, std::memory_order_relaxed);
    }
}

/// Fills `engine.dma` for one macro DMA read, around its wait, flush and read.
class DmaReadScope {
public:
    DmaReadScope(EngineState& engine_, u64 source, u64 device, u32 words, bool fifoed,
                 bool gpu_page)
        : engine{engine_} {
        State& s = Get();
        DmaRead& r = engine.dma;
        r = DmaRead{.serial = r.serial + 1,
                    .call = engine.calls,
                    .source = source,
                    .device = device,
                    .words = words,
                    .fifoed = fifoed,
                    .gpu_page = gpu_page};
        if (device != 0) {
            s.watch_begin.store(device, std::memory_order_relaxed);
            s.watch_end.store(device + u64{words} * sizeof(u32), std::memory_order_relaxed);
        }
        Snapshot();
    }

    ~DmaReadScope() {
        State& s = Get();
        s.watch_begin.store(0, std::memory_order_relaxed);
        s.watch_end.store(0, std::memory_order_relaxed);
        const DmaRead& r = engine.dma;
        s.reads.fetch_add(1, std::memory_order_relaxed);
        s.dma_dirty.fetch_add(r.dirty ? 1 : 0, std::memory_order_relaxed);
        s.dma_flushed.fetch_add(r.flushed ? 1 : 0, std::memory_order_relaxed);
        s.dma_applied.fetch_add(r.applied_flush != 0 ? 1 : 0, std::memory_order_relaxed);
        s.dma_applied_overlap.fetch_add(r.applied_overlap != 0 ? 1 : 0,
                                        std::memory_order_relaxed);
        Summary();
    }

    DmaReadScope(const DmaReadScope&) = delete;
    DmaReadScope& operator=(const DmaReadScope&) = delete;

    /// The wait for the work before the read is done; the flush (if any) follows.
    void Waited() {
        engine.dma.applied_wait += static_cast<u32>(Snapshot().second);
    }

    /// The first IsMemoryDirty check and the flush it led to (call once per check).
    void Checked(bool dirty) {
        const auto [applied, overlap] = Snapshot();
        DmaRead& r = engine.dma;
        if (!checked) {
            r.dirty = dirty;
            checked = true;
        }
        r.flushed |= dirty;
        r.applied_flush += static_cast<u32>(applied);
        r.applied_overlap += static_cast<u32>(overlap);
    }

    void Words(std::span<const u32> words) {
        DmaRead& r = engine.dma;
        r.sampled = static_cast<u32>(std::min(words.size(), r.first.size()));
        std::copy_n(words.begin(), r.sampled, r.first.begin());
    }

private:
    /// Applied writes since the previous snapshot: (anywhere, over the source).
    std::pair<u64, u64> Snapshot() {
        State& s = Get();
        const u64 applied = s.applied.load(std::memory_order_relaxed);
        const u64 overlap = s.applied_overlap.load(std::memory_order_relaxed);
        const std::pair<u64, u64> delta{applied - last_applied, overlap - last_overlap};
        last_applied = applied;
        last_overlap = overlap;
        return delta;
    }

    EngineState& engine;
    u64 last_applied{};
    u64 last_overlap{};
    bool checked{};
};

/// A zero was written to vertex_buffer.count with mesh control on. Returns the line's sequence
/// number when it was logged (the caller then probes the DMA source on the back), else 0.
inline u64 NoteZeroCount(const EngineState& engine, bool from_macro, bool split_front, u64 task,
                         u64 mesh) {
    State& s = Get();
    s.zero_writes.fetch_add(1, std::memory_order_relaxed);
    Summary();
    if (!TakeLine()) {
        return 0;
    }
    const u64 seq = s.sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    s.last_zero_seq.store(seq, std::memory_order_relaxed);
    const DmaRead& r = engine.dma;
    LOG_INFO(HW_GPU,
             "NX2 mesh count zero seq={} thread={} path={} macro={:#x} start={:#x} task={:#x} "
             "mesh={:#x} | last dma serial={} same_macro={} gpu={:#x} device={:#x} words={} "
             "fifoed={} gpu_page={} dirty={} flushed={} applied_wait_over_source={} "
             "applied_flush={} applied_flush_over_source={} first={:08x}",
             seq, split_front ? "front" : "back", from_macro ? "macro" : "direct",
             from_macro ? engine.macro_method : 0, from_macro ? engine.macro_start : 0, task, mesh,
             r.serial, r.serial != 0 && from_macro && r.call == engine.calls, r.source, r.device,
             r.words, r.fifoed, r.gpu_page, r.dirty, r.flushed, r.applied_wait, r.applied_flush,
             r.applied_overlap, fmt::join(std::span{r.first.data(), r.sampled}, ","));
    return seq;
}

/// The back's check of a logged zero's DMA source, at the write's place in the command stream.
inline void LogProbe(u64 seq, u64 device, u32 bytes, bool gpu_modified, bool host_found,
                     std::span<const u32> host, std::span<const u32> guest) {
    if (!TakeLine()) {
        return;
    }
    LOG_INFO(HW_GPU,
             "NX2 mesh count probe seq={} device={:#x} bytes={} gpu_modified={} host={} "
             "guest={:08x}",
             seq, device, bytes, gpu_modified,
             host_found ? fmt::format("{:08x}", fmt::join(host, ",")) : std::string{"none"},
             fmt::join(guest, ","));
}

/// The rasterizer recorded a task/mesh draw of `groups` task workgroups. True when the caller
/// should log the draw's details (zero groups, within the line budget).
inline bool NoteMeshDraw(u32 groups) {
    State& s = Get();
    (groups == 0 ? s.mesh_zero : s.mesh_nonzero).fetch_add(1, std::memory_order_relaxed);
    Summary();
    return groups == 0 && s.draw_lines.load(std::memory_order_relaxed) < MaxDrawLines &&
           s.draw_lines.fetch_add(1, std::memory_order_relaxed) < MaxDrawLines && TakeLine();
}

inline u64 LastZeroSeq() {
    return Get().last_zero_seq.load(std::memory_order_relaxed);
}

} // namespace VideoCommon::Nx2MeshCountTrace
