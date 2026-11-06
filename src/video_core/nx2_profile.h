// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#ifdef _MSC_VER
#include <intrin.h>
#elif defined(ARCHITECTURE_x86_64)
#include <x86intrin.h>
#endif
#include <chrono>
#include <cstdlib>
#include <string>
#include <unordered_set>

#include "common/common_types.h"
#include "video_core/gpu_split.h"
#include "common/logging.h"
#include "common/cpu_features.h"
#include "common/thread_cpu_clock.h"

// EDEN_NX2_GPU_PROFILE: time where the GPU thread spends its time. Each zone counts its own
// (exclusive) time, nested zones excluded; every 10 s the thread logs each zone's share.
namespace VideoCommon::Nx2Profile {

enum class Zone : u32 {
    Other, ///< GPU thread time outside every zone below (register writes, command parsing)
    Idle,
    PendingSleep,
    ResumePending,
    Push,
    Tick,
    Run,
    FlushInvalidate,
    Macro,
    Draw,
    Clear,
    Compute,
    FlushWork,
    PipelineLookup,
    Configure,
    SyncDescriptors,
    ReadHandles,
    FillImageViews,
    TextureBuffers,
    UpdateBuffers,
    DescriptorAcquire,
    BindStageBuffers,
    PushDescriptors,
    RenderTargets,
    ConfigureDraw,
    DynamicState,
    DrawRecord,
    TextureUpload,
    Fence,        ///< Fences made (SignalFence)
    FenceSkipped, ///< Empty fences not made
    Method,       ///< Engine method calls (what a split GPU thread's back half would run)
    SplitDrain,   ///< Split GPU thread: the front waits for the back (gpu_split.h)
    SplitWait,    ///< Split GPU thread: the back waits for work
    WriterWait,
    ListSubmitWait,
    ListGpuWait,
    Count,
};

inline constexpr std::array<const char*, static_cast<size_t>(Zone::Count)> zone_names{
    "other",         "idle",          "pending sleep",  "resume pending", "push",
    "tick",          "run",           "flush/inval",    "macro",          "draw",
    "clear",         "compute",       "flush work",     "pipeline lookup", "configure",
    "sync descriptors", "read handles", "fill image views", "texture buffers", "update buffers",
    "descriptor acquire", "bind stage buffers", "push descriptors", "render targets",
    "configure draw", "dynamic state", "draw record", "texture upload", "fence",
    "fence skipped", "method", "split drain", "split wait",
    "writer wait", "list submit wait", "list GPU wait",
};

// Read once at startup: Enabled() runs several times per method write (no guard to check).
// Off while EDEN_NX2_SAMPLE_PROFILE samples (common/nx2_sample_profile.h): the zone timers would
// show up in the samples.
inline const bool g_enabled = std::getenv("EDEN_NX2_GPU_PROFILE") != nullptr &&
                              std::getenv("EDEN_NX2_SAMPLE_PROFILE") == nullptr;

inline bool Enabled() {
    return g_enabled;
}

// Nanoseconds; from the host's own counter where the wall clock reads it directly (the zones
// run ~150,000 times a second, and QueryPerformanceCounter cost ~5% of the GPU thread).
inline u64 Now() {
    return static_cast<u64>(Common::g_wall_clock.GetTimeNS().count());
}

/// Amounts counted on the GPU thread, logged per second with the zones.
enum class Counter : u32 {
    DownloadBytes,  ///< Buffer data copied back from the host GPU at fences
    DownloadCopies, ///< Buffer ranges copied back
    DmaReadBytes,   ///< Guest memory the native MME read with DMA
    DownloadBytesOnDmaPages, ///< Of DownloadBytes: on pages the MME has DMA-read
    MethodWrites,   ///< 3D register writes (Maxwell3D::CallMethod below the macro range)
    SameValueWrites, ///< Of MethodWrites: the register already held the value
    AcquireChecks,  ///< Semaphore acquires evaluated
    AcquireWaits,   ///< Of AcquireChecks: not met yet (the channel waits)
    // Macro DMA reads with EDEN_NX2_DMA_READ_RANGES (GpuSplit::WaitForMemoryRange), against the
    // wait for every memory write before.
    RangeWaitFree,      ///< Neither waits
    RangeWaitSkipped,   ///< The old wait would have waited, this one doesn't
    RangeWaitShortened, ///< Both wait, this one for less
    RangeWaitSame,      ///< Both wait as long
    RangeWaitByWriter,  ///< Waits that compute or copy work sets (in MemoryReason order)
    RangeWaitBySync,    ///< ... the last host-level wait
    RangeWaitByUnranged, ///< ... an unlogged memory write
    RangeWaitByRange,   ///< ... an overlapping logged write
    RangeVerifyReads,   ///< EDEN_NX2_WAIT_VERIFY: reads repeated after the old wait
    RangeVerifyDiffs,   ///< ... whose words differed
    // Semaphore acquires with EDEN_NX2_ACQUIRE_RANGES (puller.cpp MetBeforeDrain).
    AcquireMetNoDrain,     ///< Met without a drain
    AcquireResumedNoDrain, ///< A suspended channel's acquire met without a drain
    AcquireMetAfterWait,   ///< Of both: after waiting for the semaphore's possible writers
    AcquireDrained,        ///< Not met yet: drained as before
    AcquireVerifyChecks,   ///< EDEN_NX2_WAIT_VERIFY: met acquires checked again after a drain
    AcquireVerifyDiffs,    ///< ... not met any more
    // EDEN_NX2_STEP_WRITER_SKIP (DmaPusher::SkipWriterWait).
    StepWriterSkips,  ///< Entries read without waiting for queued compute and copy work
    StepWriterMisses, ///< ... that the work wrote after all (back thread)
    // GPU-written command list copies (BufferCacheRuntime::WaitSubmittedTick, front; and
    // RasterizerVulkan::SubmitNx2ListCopies, back).
    ListWaits,          ///< Reads that waited for the host GPU's copy
    ListWaitNs,         ///< ... their total wait (ns)
    ListWaitSpinMet,    ///< ... copies done within the spin (EDEN_NX2_LIST_WAIT_SPIN_US)
    ListWaitUnsubmitted, ///< ... copies the back hadn't submitted yet
    ListCopySubmitNow,  ///< Copies submitted at once because the front waited (back)
    ListCopyDeferred,   ///< Copies whose submission was deferred (back)
    // RasterizerVulkan::MustFlushRegion's buffer cache check (EDEN_NX2_GPU_MODIFIED_BITMAP).
    MustFlushLockFree,     ///< Answered from the bitmap, no mutex
    MustFlushLocked,       ///< Looked up under the mutex
    MustFlushBitmapMisses, ///< EDEN_NX2_WAIT_VERIFY: bitmap said no, the ranges said yes
    Count,
};
// What the zones and drain waits measure in: the time stamp counter without the fences the wall
// clock reads it with (those fences cost ~10% of the GPU back thread with ~45,000 draws a
// second). Report converts it with the wall clock.
inline u64 Ticks() {
#ifdef ARCHITECTURE_x86_64
    return __rdtsc();
#else
    return Now();
#endif
}

struct ThreadData {
    std::array<u64, static_cast<size_t>(Zone::Count)> exclusive_ns{};
    std::array<u64, static_cast<size_t>(Zone::Count)> count{};
    std::array<u64, static_cast<size_t>(Counter::Count)> counters{}; ///< See Counter
    std::array<u64, static_cast<size_t>(DrainSite::Count)> drain_ns{};
    std::array<u64, static_cast<size_t>(DrainSite::Count)> drain_count{};
    u64 child_ns{};    ///< Time of the zones nested in the current one
    u64 window_start{};
    u64 window_start_ticks{};
    std::optional<u64> cpu_start_ns;
};

inline thread_local ThreadData t_data;

class Scope {
public:
    explicit Scope(Zone zone_) : zone{zone_}, enabled{Enabled()} {
        if (!enabled) {
            return;
        }
        parent_child_ns = t_data.child_ns;
        t_data.child_ns = 0;
        start = Ticks();
    }

    ~Scope() {
        if (!enabled) {
            return;
        }
        const u64 elapsed = Ticks() - start;
        auto& data = t_data;
        const auto index = static_cast<size_t>(zone);
        data.exclusive_ns[index] += elapsed - data.child_ns;
        ++data.count[index];
        data.child_ns = parent_child_ns + elapsed;
    }

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Zone zone;
    bool enabled;
    u64 start{};
    u64 parent_child_ns{};
};


/// Method calls on this thread by method (3D registers on the back, macros on the front), and
/// same-value writes among them; MME DMA reads by the macro that made them. The most frequent
/// are logged with the zones.
inline thread_local std::array<u32, 0x2000> t_method_count{};
inline thread_local std::array<u32, 0x2000> t_method_same{};
inline thread_local std::array<u32, 0x2000> t_dma_reads{};

inline void CountMethod(u32 method, bool same) {
    if (Enabled() && method < t_method_count.size()) {
        ++t_method_count[method];
        t_method_same[method] += same ? 1 : 0;
    }
}

inline void CountDmaRead(u32 macro) {
    if (Enabled() && macro < t_dma_reads.size()) {
        ++t_dma_reads[macro];
    }
}

inline std::string TopMethods(const std::array<u32, 0x2000>& counts,
                              const std::array<u32, 0x2000>* same, u64 window) {
    std::array<u32, 0x2000> order;
    for (u32 i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    constexpr size_t top = 24;
    std::partial_sort(order.begin(), order.begin() + top, order.end(),
                      [&counts](u32 a, u32 b) { return counts[a] > counts[b]; });
    std::string text;
    for (size_t i = 0; i < top && counts[order[i]] != 0; ++i) {
        const u32 method = order[i];
        text += fmt::format(" {:#x} {:.0f}/s", method,
                            static_cast<double>(counts[method]) * 1e9 / window);
        if (same != nullptr && (*same)[method] != 0) {
            text += fmt::format(" ({:.0f} same)", static_cast<double>((*same)[method]) * 1e9 / window);
        }
        text += ',';
    }
    if (!text.empty()) {
        text.pop_back();
    }
    return text;
}

/// Device pages (4 KiB) the native MME has DMA-read since start (GPU thread only; filled only
/// while profiling): which buffer downloads anything on the GPU thread reads back.
inline std::unordered_set<u64>& DmaReadPages() {
    static std::unordered_set<u64> pages;
    return pages;
}

inline void Add(Counter counter, u64 amount) {
    if (Enabled()) {
        t_data.counters[static_cast<size_t>(counter)] += amount;
    }
}

/// The front waited for the back since `start` (Ticks) for `site`.
inline void AddDrain(DrainSite site, u64 start) {
    if (Enabled()) {
        const auto index = static_cast<size_t>(site);
        t_data.drain_ns[index] += Ticks() - start;
        ++t_data.drain_count[index];
    }
}

/// Called by the GPU thread (and the split's back thread) between commands: logs and resets
/// the totals every 10 s.
inline void Report(const char* thread_name = "GPU thread") {
    if (!Enabled()) {
        return;
    }
    auto& data = t_data;
    const u64 now = Now();
    if (data.window_start == 0) {
        data.window_start = now;
        data.window_start_ticks = Ticks();
        data.cpu_start_ns = Common::CurrentThreadCpuTimeNs();
        data.child_ns = 0;
        return;
    }
    const u64 window = now - data.window_start;
    if (window < 10'000'000'000ULL) {
        return;
    }
    const auto cpu_now = Common::CurrentThreadCpuTimeNs();
    const u64 now_ticks = Ticks();
    const u64 window_ticks = (std::max<u64>)(now_ticks - data.window_start_ticks, 1);
    // Time in no zone at all is the wall time minus the zones' totals.
    u64 zoned{};
    for (size_t i = 1; i < data.exclusive_ns.size(); ++i) {
        zoned += data.exclusive_ns[i];
    }
    data.exclusive_ns[0] = window_ticks > zoned ? window_ticks - zoned : 0;
    std::string line;
    for (size_t i = 0; i < data.exclusive_ns.size(); ++i) {
        if (data.exclusive_ns[i] == 0 && data.count[i] == 0) {
            continue;
        }
        // Per second of wall time: milliseconds spent and calls made.
        const double ms = static_cast<double>(data.exclusive_ns[i]) * 1000.0 / window_ticks;
        const double calls = static_cast<double>(data.count[i]) * 1e9 / window;
        line += fmt::format(" | {} {:.1f} ms ({:.0f}/s)", zone_names[i], ms, calls);
    }
    line += fmt::format(" | buffer downloads {:.0f} KiB in {:.0f} copies ({:.0f} KiB on pages "
                        "MME DMA reads) | MME DMA reads {:.0f} KiB",
                        static_cast<double>(data.counters[0]) * 1e9 / 1024.0 / window,
                        static_cast<double>(data.counters[1]) * 1e9 / window,
                        static_cast<double>(data.counters[3]) * 1e9 / 1024.0 / window,
                        static_cast<double>(data.counters[2]) * 1e9 / 1024.0 / window);
    line += fmt::format(" | 3D register writes {:.0f} ({:.0f} same value)",
                        static_cast<double>(data.counters[4]) * 1e9 / window,
                        static_cast<double>(data.counters[5]) * 1e9 / window);
    line += fmt::format(" | acquires {:.0f} ({:.0f} waited)",
                        static_cast<double>(data.counters[6]) * 1e9 / window,
                        static_cast<double>(data.counters[7]) * 1e9 / window);
    const auto per_second = [&](Counter counter) {
        return static_cast<double>(data.counters[static_cast<size_t>(counter)]) * 1e9 / window;
    };
    if (data.counters[static_cast<size_t>(Counter::RangeWaitFree)] +
            data.counters[static_cast<size_t>(Counter::RangeWaitSkipped)] +
            data.counters[static_cast<size_t>(Counter::RangeWaitShortened)] +
            data.counters[static_cast<size_t>(Counter::RangeWaitSame)] !=
        0) {
        line += fmt::format(
            " | macro dma read waits free/skipped/shortened/same {:.0f}/{:.0f}/{:.0f}/{:.0f} "
            "(set by writer {:.0f}, sync {:.0f}, unranged {:.0f}, range {:.0f}; verify {:.0f} "
            "reads, {:.0f} differ)",
            per_second(Counter::RangeWaitFree), per_second(Counter::RangeWaitSkipped),
            per_second(Counter::RangeWaitShortened), per_second(Counter::RangeWaitSame),
            per_second(Counter::RangeWaitByWriter), per_second(Counter::RangeWaitBySync),
            per_second(Counter::RangeWaitByUnranged), per_second(Counter::RangeWaitByRange),
            per_second(Counter::RangeVerifyReads), per_second(Counter::RangeVerifyDiffs));
    }
    if (data.counters[static_cast<size_t>(Counter::AcquireMetNoDrain)] +
            data.counters[static_cast<size_t>(Counter::AcquireResumedNoDrain)] +
            data.counters[static_cast<size_t>(Counter::AcquireDrained)] !=
        0) {
        line += fmt::format(
            " | acquires met without a drain {:.0f} (resumes {:.0f}, {:.0f} after a short wait), "
            "drained {:.0f} (verify {:.0f}, {:.0f} differ)",
            per_second(Counter::AcquireMetNoDrain), per_second(Counter::AcquireResumedNoDrain),
            per_second(Counter::AcquireMetAfterWait), per_second(Counter::AcquireDrained),
            per_second(Counter::AcquireVerifyChecks), per_second(Counter::AcquireVerifyDiffs));
    }
    if (data.counters[static_cast<size_t>(Counter::StepWriterSkips)] +
            data.counters[static_cast<size_t>(Counter::StepWriterMisses)] !=
        0) {
        line += fmt::format(" | entries read without the writer wait {:.0f} ({:.0f} too early)",
                            per_second(Counter::StepWriterSkips),
                            per_second(Counter::StepWriterMisses));
    }
    if (data.counters[static_cast<size_t>(Counter::ListWaits)] != 0) {
        const auto waits = data.counters[static_cast<size_t>(Counter::ListWaits)];
        line += fmt::format(
            " | list copy waits {:.1f}/s, {:.0f} us/s, {:.0f} us each ({:.1f}/s met spinning, "
            "{:.1f}/s unsubmitted)",
            per_second(Counter::ListWaits), per_second(Counter::ListWaitNs) / 1000.0,
            static_cast<double>(data.counters[static_cast<size_t>(Counter::ListWaitNs)]) /
                1000.0 / static_cast<double>(waits),
            per_second(Counter::ListWaitSpinMet), per_second(Counter::ListWaitUnsubmitted));
    }
    if (data.counters[static_cast<size_t>(Counter::ListCopySubmitNow)] +
            data.counters[static_cast<size_t>(Counter::ListCopyDeferred)] !=
        0) {
        line += fmt::format(" | list copies submitted at once {:.1f}/s, deferred {:.1f}/s",
                            per_second(Counter::ListCopySubmitNow),
                            per_second(Counter::ListCopyDeferred));
    }
    if (data.counters[static_cast<size_t>(Counter::MustFlushLockFree)] +
            data.counters[static_cast<size_t>(Counter::MustFlushLocked)] !=
        0) {
        line += fmt::format(" | flush checks lock-free {:.0f}/s, locked {:.0f}/s ({:.0f} missed)",
                            per_second(Counter::MustFlushLockFree),
                            per_second(Counter::MustFlushLocked),
                            per_second(Counter::MustFlushBitmapMisses));
    }
    std::string drains;
    for (size_t i = 0; i < data.drain_count.size(); ++i) {
        if (data.drain_count[i] != 0) {
            drains += fmt::format(" {} {:.1f} ms ({:.0f}/s),", drain_site_names[i],
                                  static_cast<double>(data.drain_ns[i]) * 1000.0 / window_ticks,
                                  static_cast<double>(data.drain_count[i]) * 1e9 / window);
        }
    }
    if (!drains.empty()) {
        drains.pop_back();
        line += " | waits for the back by site:" + drains;
    }
    if (cpu_now && data.cpu_start_ns && *cpu_now >= *data.cpu_start_ns) {
        line += fmt::format(" | host_cpu_busy_ms_per_s={:.3f} window_wall_ms={:.3f}",
                            static_cast<double>(*cpu_now - *data.cpu_start_ns) * 1000.0 / window,
                            static_cast<double>(window) / 1e6);
    } else {
        line += fmt::format(" | host_cpu_busy_ms_per_s=unavailable window_wall_ms={:.3f}",
                            static_cast<double>(window) / 1e6);
    }
    LOG_INFO(HW_GPU, "{} per second:{}", thread_name, line);
    if (const std::string methods = TopMethods(t_method_count, &t_method_same, window); !methods.empty()) {
        LOG_INFO(HW_GPU, "{} methods per second:{}", thread_name, methods);
    }
    if (const std::string reads = TopMethods(t_dma_reads, nullptr, window); !reads.empty()) {
        LOG_INFO(HW_GPU, "{} MME DMA reads per second by macro:{}", thread_name, reads);
    }
    t_method_count = {};
    t_method_same = {};
    t_dma_reads = {};
    data = {};
    data.window_start = now;
    data.window_start_ticks = now_ticks;
    data.cpu_start_ns = cpu_now;
}

} // namespace VideoCommon::Nx2Profile
