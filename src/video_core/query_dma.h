// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>

#include <fmt/ranges.h>

#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/memory_manager.h"
#include "video_core/rasterizer_interface.h"

namespace VideoCommon::QueryDma {

/// EDEN_NX2_QUERY_DMA_TRACE=1: logs MME DMA reads of reports whose host query results are not
/// in guest memory yet. Below High GPU accuracy, counter reports are written when a later fence
/// pops, so such a read returns an older count.
inline bool TraceEnabled() {
    static const bool enabled = std::getenv("EDEN_NX2_QUERY_DMA_TRACE") != nullptr;
    return enabled;
}

/// EDEN_NX2_QUERY_REPORT_WAIT: what a macro DMA read does about reports whose host query
/// results have not landed in guest memory yet (FlushPending, the wait of #204/#215).
enum class ReportWaitMode : u8 {
    Off,    ///< =0: no flush and no wait (the read can return an older count)
    Cached, ///< default: on the split's front, only when the range holds a cached report
    All,    ///< =all: at every read, as #204/#215 do (each split-front read then drains)
};
inline ReportWaitMode ReportWait() {
    static const ReportWaitMode mode = [] {
        const char* value = std::getenv("EDEN_NX2_QUERY_REPORT_WAIT");
        if (value == nullptr || value[0] == '\0') {
            return ReportWaitMode::Cached;
        }
        if (value[0] == '0') {
            return ReportWaitMode::Off;
        }
        return value[0] == 'a' ? ReportWaitMode::All : ReportWaitMode::Cached;
    }();
    return mode;
}

/// Whether [source, source + size) may hold a report the query cache still tracks (pending or
/// not yet unregistered). Callable on the split's front: reads only the query cache's address
/// map, under its lock. Run after the front's memory-writer wait, so every report the front
/// sent over the range has been registered by the back.
inline bool MayHoldReports(const Tegra::MemoryManager& memory,
                           VideoCore::RasterizerInterface& rasterizer, GPUVAddr source,
                           size_t size) {
    constexpr GPUVAddr page_size = 0x1000;
    const GPUVAddr end = source + size;
    for (GPUVAddr begin = source; begin < end;) {
        const GPUVAddr page_end = (std::min)((begin & ~(page_size - 1)) + page_size, end);
        if (const auto device = memory.GpuToCpuAddress(begin);
            device && rasterizer.HasCachedQueries(*device, page_end - begin)) {
            return true;
        }
        begin = page_end;
    }
    return false;
}

/// Whether [source, source + size) holds reports still waiting for host query results. Runs
/// where the rasterizer's caches are owned (the back of a split GPU thread).
inline bool HasPendingQueries(const Tegra::MemoryManager& memory,
                              VideoCore::RasterizerInterface& rasterizer, GPUVAddr source,
                              size_t size) {
    constexpr GPUVAddr page_size = 0x1000;
    const GPUVAddr end = source + size;
    for (GPUVAddr begin = source; begin < end;) {
        const GPUVAddr page_end = (std::min)((begin & ~(page_size - 1)) + page_size, end);
        if (const auto device = memory.GpuToCpuAddress(begin);
            device && rasterizer.HasPendingQueries(*device, page_end - begin)) {
            return true;
        }
        begin = page_end;
    }
    return false;
}

/// Writes the reports in [source, source + size) to guest memory, waiting for pending host
/// results. Runs where the rasterizer's caches are owned.
inline void FlushPending(const Tegra::MemoryManager& memory,
                         VideoCore::RasterizerInterface& rasterizer, GPUVAddr source, size_t size) {
    constexpr GPUVAddr page_size = 0x1000;
    const GPUVAddr end = source + size;
    for (GPUVAddr begin = source; begin < end;) {
        const GPUVAddr page_end = (std::min)((begin & ~(page_size - 1)) + page_size, end);
        if (const auto device = memory.GpuToCpuAddress(begin)) {
            rasterizer.FlushQueryResults(*device, page_end - begin);
        }
        begin = page_end;
    }
}

/// Logs a read of pending reports with the words it reads, and the same words once the next
/// fence has popped and the results have landed.
inline void Trace(const Tegra::MemoryManager& memory, VideoCore::RasterizerInterface& rasterizer,
                  u32 macro, u32 method, GPUVAddr source, size_t size) {
    static constexpr u64 max_reads = 4000;
    static std::atomic<u64> reads{};
    const u64 read = reads.fetch_add(1, std::memory_order_relaxed) + 1;
    if (read > max_reads) {
        if (read % 10000 == 0) {
            LOG_INFO(HW_GPU, "Query DMA read of pending reports ({} so far)", read);
        }
        return;
    }
    std::array<u32, 4> words{};
    const size_t bytes = (std::min)(size, sizeof(words));
    memory.ReadBlockUnsafe(source, words.data(), bytes);
    LOG_INFO(HW_GPU,
             "Query DMA read #{} of pending reports: macro={:#x} method={:#x} gpu={:#x} "
             "bytes={:#x} words={:08x}",
             read, macro, method, source, size, fmt::join(words, ","));
    rasterizer.SyncOperation([&memory, source, bytes, read] {
        std::array<u32, 4> landed{};
        memory.ReadBlockUnsafe(source, landed.data(), bytes);
        LOG_INFO(HW_GPU, "Query DMA read #{} landed words={:08x}", read, fmt::join(landed, ","));
    });
}

/// EDEN_NX2_QUERY_DMA_TRACE with the report wait on: a read whose reports were still pending
/// after FlushPending (the read returns an older count despite the wait).
inline void TraceAfterWait(const Tegra::MemoryManager& memory, u32 macro, u32 method,
                           GPUVAddr source, size_t size) {
    static constexpr u64 max_reads = 4000;
    static std::atomic<u64> reads{};
    const u64 read = reads.fetch_add(1, std::memory_order_relaxed) + 1;
    if (read > max_reads) {
        if (read % 10000 == 0) {
            LOG_INFO(HW_GPU, "Query DMA read still pending after the report wait ({} so far)",
                     read);
        }
        return;
    }
    std::array<u32, 4> words{};
    memory.ReadBlockUnsafe(source, words.data(), (std::min)(size, sizeof(words)));
    LOG_INFO(HW_GPU,
             "Query DMA read #{} still pending after the report wait: macro={:#x} method={:#x} "
             "gpu={:#x} bytes={:#x} words={:08x}",
             read, macro, method, source, size, fmt::join(words, ","));
}

} // namespace VideoCommon::QueryDma
