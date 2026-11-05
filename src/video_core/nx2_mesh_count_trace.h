// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include "common/common_types.h"
#include "common/logging.h"

// EDEN_NX2_MESH_COUNT_TRACE=1: why task/mesh draws reach vkCmdDrawMeshTasksEXT with no task
// groups. A zero written to vertex_buffer.count (the task count) with mesh control on is logged
// with the 3D engine's last macro DMA read: its words, whether its source was GPU-modified, and
// how many collected CPU writes the flush before the read applied over it. The back then logs the
// host buffer's words at that source (same seq). Lines are capped; a summary follows every second.
//
// On the split front the macros then run in the interpreter (not the JIT), and for the macro at
// EDEN_NX2_MESH_COUNT_START (hex, default 0x24b) each count write logs the call's inputs:
// parameters and where the pushbuffer had them, register reads, data RAM reads and who wrote
// those words. DMA reads log what the macro did with the words afterwards and whether they are a
// query report the query cache has yet to write. The first zero count from a macro dumps the macro
// code (tools/nx2/mme_dis.py disassembles it).
namespace VideoCommon::Nx2MeshCountTrace {

inline const bool g_enabled = [] {
    const char* value = std::getenv("EDEN_NX2_MESH_COUNT_TRACE");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}();

inline constexpr u32 MaxLines = 200;
inline constexpr u32 MaxDrawLines = 40;
inline constexpr u32 MaxMacroLines = 40;       ///< Per kind (zero, nonzero) for the traced start
inline constexpr u32 MaxReportFlowLines = 60;  ///< DMA reads over a recent report
inline constexpr u32 MaxOtherFlowLines = 20;   ///< Other DMA reads
inline constexpr u32 MaxResolvedLines = 60;
inline constexpr u32 MaxReportLines = 400;   ///< Report request + write lines
inline constexpr u32 MaxReportSlotLines = 4; ///< Request lines per report slot
inline constexpr u32 MaxReportSlots = 64;
inline constexpr u32 MaxZpassLines = 200;    ///< Each of: no-counter, enable, host result
inline constexpr u32 MaxParams = 16;
inline constexpr u32 MaxRegReads = 32;
inline constexpr u32 MaxEmits = 64;
inline constexpr u32 MaxFlows = 8;
inline constexpr u32 MaxShown = 24; ///< Data RAM accesses shown per line
inline constexpr u32 MaxStarts = 16;
inline constexpr size_t DataRamWords = 0xc00;

/// The macro start whose count writes log their inputs (EDEN_NX2_MESH_COUNT_START, hex).
inline const u32 g_traced_start = [] {
    const char* value = std::getenv("EDEN_NX2_MESH_COUNT_START");
    return value != nullptr && value[0] != '\0'
               ? static_cast<u32>(std::strtoul(value, nullptr, 16))
               : 0x24bU;
}();

/// How the DMA pusher fetched the GP entry a method came from.
enum class EntryPath : u8 { Unknown, Prefetch, Submitted, Safe, Copy, Slow, Count };

inline const char* EntryPathName(EntryPath path) {
    constexpr std::array<const char*, static_cast<size_t>(EntryPath::Count)> names{
        "unknown", "prefetch", "submitted", "safe", "copy", "slow"};
    return static_cast<size_t>(path) < names.size() ? names[static_cast<size_t>(path)] : "?";
}

/// The pushbuffer word a method argument came from (GPU VA, 0 when unknown) and its entry's path.
struct PushSource {
    u64 va{};
    EntryPath path{};
};

/// A report_semaphore.query write the split front saw, in order.
struct ReportOp {
    u64 serial{}; ///< 1-based per engine; 0: empty
    u64 gpu{};
    u64 call{};   ///< EngineState::calls when written
    u32 payload{};
    u32 query{};  ///< report_semaphore.query
    u32 start{};  ///< Macro start when a macro wrote it, else ~0
};

/// report_semaphore.query fields: "op=... type=... short=... sub=...".
inline std::string DescribeQuery(u32 query) {
    constexpr std::array<const char*, 4> operations{"release", "acquire", "report_only", "trap"};
    return fmt::format("op={} type={} short={} sub={} loc={}", operations[query & 3],
                       (query >> 23) & 31, (query >> 28) & 1, (query >> 5) & 7, (query >> 12) & 15);
}

enum class QueryStatus : u8 { None, Pending, Written, Immediate, Invalidated, Unsynced, Count };

inline const char* QueryStatusName(QueryStatus status) {
    constexpr std::array<const char*, static_cast<size_t>(QueryStatus::Count)> names{
        "none", "pending", "written", "immediate", "discarded_invalidated",
        "discarded_unsynced"};
    return names[static_cast<size_t>(status)];
}

/// How CounterReport writes a report: memcpy right away (Payload below High accuracy),
/// SignalFence (short fence reports), or SyncOperation (the rest).
enum class QueryPath : u8 { Immediate, Fence, SyncOperation, Count };

inline const char* QueryPathName(QueryPath path) {
    constexpr std::array<const char*, static_cast<size_t>(QueryPath::Count)> names{
        "immediate", "fence", "sync_operation"};
    return names[static_cast<size_t>(path)];
}

/// A report the query cache took (CounterReport) and what became of its guest write.
struct QueryReport {
    u64 id{}; ///< 0: empty
    u64 device{};
    u64 value{};      ///< Written value (status Written/Immediate), or the payload it will write
    u64 watched_by{}; ///< DMA read serial that read the slot while the write was pending
    s64 request_ns{};
    s64 resolve_ns{};
    u32 requested{}; ///< Report type the guest asked for
    u32 effective{}; ///< Type the cache used (Payload with payload 1 when unsupported)
    u32 payload{};
    u32 flags{};      ///< QueryPropertiesFlags
    u64 write_seq{};  ///< Order of the guest write among all reports (0: not written)
    QueryStatus status{};
    QueryPath path{};
    bool timestamp{};
    bool fence{};
    bool fallback{}; ///< No streamer for the type: Payload with payload 1
    bool logged{};   ///< Its request was logged: its write is too
};

/// The writer of a data RAM word: a macro DMA read (serial) or a macro's DWRITE (call).
struct DramOrigin {
    u64 serial{};
    u32 start{};
    u8 kind{}; ///< 0 unknown, 1 DMA read, 2 DWRITE
};

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
    // Split front, for the flow line at the end of the macro call.
    u32 start{};            ///< Macro start that read
    u32 ram{};              ///< Data RAM destination (not fifoed)
    u32 emit_index{};       ///< Methods the macro had emitted, the DMA method included
    u32 dram_write_index{}; ///< The interpreter's data_ram_writes count at the read
    bool report_hit{};      ///< A recent front report_semaphore op targeted the source
    bool report_reached_back{}; ///< The back had executed that op when the words were read
    s64 report_offset{};    ///< Source - report address
    ReportOp report{};
    QueryReport query{};    ///< The query cache's newest report over the source, when read
};

/// The macro call running on the split front, recorded while the trace is on.
struct MacroCall {
    bool recorded{};
    u32 start{};
    u64 call{};
    u32 param_count{};
    std::array<u32, MaxParams> params{};
    PushSource push{};
    u32 push_words{}; ///< Parameter words contiguous at push.va
    u32 reg_read_count{};
    std::array<std::pair<u32, u32>, MaxRegReads> reg_reads{};
    u32 emits{};
    std::array<std::pair<u32, u32>, MaxEmits> emitted{};
    u32 flow_count{};
    u32 flows_dropped{};
    std::array<DmaRead, MaxFlows> flows{};
};

struct EngineState {
    DmaRead dma;
    u64 calls{}; ///< Macro calls run
    u32 macro_method{};
    u32 macro_start{};
    bool in_macro{};
    // Split front: the source of the macro parameters being collected, the call running, data
    // RAM writers, and the recent report_semaphore ops. The back counts the reports it ran once
    // the front owns the macros (one per front op from then on).
    PushSource push{};
    u32 push_words{};
    MacroCall call;
    std::vector<DramOrigin> dram_origin;
    std::array<ReportOp, 32> reports{};
    u64 report_serial{};
    std::atomic<u64> back_report_serial{};
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
    // Macro inputs, DMA flows, report resolutions.
    std::atomic<u32> macro_zero_lines{};
    std::atomic<u32> macro_nonzero_lines{};
    std::atomic<u32> report_flow_lines{};
    std::atomic<u32> other_flow_lines{};
    std::atomic<u32> resolved_lines{};
    std::atomic<u64> macro_lines_seq{};
    std::atomic<bool> code_dumped{};
    // Count writes from macros by start: key start + 1 (0: free); zero and nonzero with mesh
    // control on, and any with it off.
    std::array<std::atomic<u32>, MaxStarts> start_keys{};
    std::array<std::atomic<u64>, MaxStarts> start_zero{};
    std::array<std::atomic<u64>, MaxStarts> start_nonzero{};
    std::array<std::atomic<u64>, MaxStarts> start_off{};
    std::atomic<u64> start_overflow{};
    std::atomic<u64> dma_report_hits{};
    std::atomic<u64> dma_report_pending{};      ///< Over a report whose write was pending
    std::atomic<u64> dma_report_not_reached{};  ///< Over a report the back had not run yet
    std::atomic<u64> queries_requested{};
    std::atomic<u64> queries_written{};
    std::atomic<u64> queries_discarded{};
    std::atomic<u64> queries_substituted{}; ///< Unsupported type: Payload 1 written instead
    std::atomic<u64> queries_out_of_order{}; ///< Written after a later-requested report
    std::mutex query_mutex;
    std::array<QueryReport, 512> queries{};
    u64 query_ids{};
    u64 write_seq{};
    u64 max_written_id{};
    std::array<std::pair<u64, u32>, MaxReportSlots> slot_lines{}; ///< (device, request lines)
    std::atomic<u32> report_lines{};
    // ZPassPixelCount64 (SamplesStreamer): reports with no host query running (final value 0),
    // enable transitions at the rasterizer, host results.
    std::atomic<u32> zpass_no_counter_lines{};
    std::atomic<u32> zpass_enable_lines{};
    std::atomic<u32> zpass_result_lines{};
    std::atomic<u64> zpass_no_counter{};
    std::atomic<u64> zpass_enable_on{};
    std::atomic<u64> zpass_enable_off{};
    std::atomic<u64> zpass_results_zero{};
    std::atomic<u64> zpass_results_nonzero{};
    std::atomic<u64> zpass_host_begins{};
    std::atomic<bool> zpass_host_active{};
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
    std::string starts;
    for (size_t i = 0; i < MaxStarts; ++i) {
        const u32 key = s.start_keys[i].load(std::memory_order_relaxed);
        if (key == 0) {
            break;
        }
        const u64 z = take(s.start_zero[i]);
        const u64 nz = take(s.start_nonzero[i]);
        const u64 off = take(s.start_off[i]);
        if (z + nz + off != 0) {
            starts += fmt::format("{}{:#x}=z{}/nz{}/off{}", starts.empty() ? "" : ",", key - 1,
                                  z, nz, off);
        }
    }
    LOG_INFO(HW_GPU,
             "NX2 mesh count summary: mesh draws zero={} nonzero={} zero_count_writes={} | macro "
             "DMA reads={} gpu_modified={} flushed={} flush_applied_cpu_write={} "
             "flush_applied_over_source={} over_report={} over_pending_report={} "
             "over_report_not_on_back={} | count writes by start [{}] other_starts={} | queries "
             "requested={} written={} discarded={} unsupported_type={} written_after_later_request={} | "
             "zpass no_counter_reports={} enable_on={} enable_off={} host_query_begins={} "
             "host_results zero={} nonzero={} | lines {}/{}",
             zero, nonzero, zero_writes, reads, dirty, flushed, applied, overlap,
             take(s.dma_report_hits), take(s.dma_report_pending), take(s.dma_report_not_reached),
             starts, take(s.start_overflow), take(s.queries_requested), take(s.queries_written),
             take(s.queries_discarded), take(s.queries_substituted),
             take(s.queries_out_of_order), take(s.zpass_no_counter), take(s.zpass_enable_on),
             take(s.zpass_enable_off), take(s.zpass_host_begins), take(s.zpass_results_zero),
             take(s.zpass_results_nonzero), std::min(s.lines.load(std::memory_order_relaxed), MaxLines), MaxLines);
}

/// A macro wrote vertex_buffer.count: counted by its start for the summary.
inline void CountStart(u32 start, u32 value, bool mesh_control) {
    State& s = Get();
    for (size_t i = 0; i < MaxStarts; ++i) {
        u32 key = s.start_keys[i].load(std::memory_order_relaxed);
        if (key == 0 && s.start_keys[i].compare_exchange_strong(key, start + 1,
                                                                std::memory_order_relaxed)) {
            key = start + 1;
        }
        if (key == start + 1) {
            (!mesh_control ? s.start_off[i] : value == 0 ? s.start_zero[i] : s.start_nonzero[i])
                .fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    s.start_overflow.fetch_add(1, std::memory_order_relaxed);
}

/// True when a line from the budget `counter` (of `max`) may be logged.
inline bool TakeBudget(std::atomic<u32>& counter, u32 max) {
    return counter.load(std::memory_order_relaxed) < max &&
           counter.fetch_add(1, std::memory_order_relaxed) < max;
}

/// "addr:value,..." for up to MaxShown pairs, with "+N" when there were more.
inline std::string JoinPairs(std::span<const std::pair<u32, u32>> pairs) {
    std::string out;
    const size_t shown = std::min<size_t>(pairs.size(), MaxShown);
    for (size_t i = 0; i < shown; ++i) {
        out += fmt::format("{}{:#x}:{:#x}", i == 0 ? "" : ",", pairs[i].first, pairs[i].second);
    }
    if (pairs.size() > shown) {
        out += fmt::format(",+{}", pairs.size() - shown);
    }
    return out.empty() ? std::string{"-"} : out;
}

/// Records the report_semaphore op the front just saw.
inline void NoteReportOp(EngineState& engine, u64 gpu, u32 payload, u32 query, bool from_macro) {
    const u64 serial = ++engine.report_serial;
    engine.reports[serial % engine.reports.size()] = ReportOp{
        .serial = serial,
        .gpu = gpu,
        .call = engine.calls,
        .payload = payload,
        .query = query,
        .start = from_macro ? engine.macro_start : ~0U,
    };
}

/// The newest recent front report op whose 16-byte slot overlaps [begin, end), or null.
inline const ReportOp* FindReportOp(const EngineState& engine, u64 begin, u64 end) {
    const ReportOp* found = nullptr;
    for (const ReportOp& op : engine.reports) {
        if (op.serial != 0 && op.gpu < end && begin < op.gpu + 16 &&
            (found == nullptr || op.serial > found->serial)) {
            found = &op;
        }
    }
    return found;
}

/// Under query_mutex: true while the report slot at `device` has request lines left.
inline bool TakeSlotLine(State& s, u64 device) {
    for (auto& [slot, lines] : s.slot_lines) {
        if (lines == 0 || slot == device) {
            slot = device;
            return lines++ < MaxReportSlotLines && TakeBudget(s.report_lines, MaxReportLines);
        }
    }
    return false;
}

/// Query cache (CounterReport): a report was taken. Returns its id for NoteQueryWrite. The first
/// requests to each slot log "NX2 mesh count report request" (and their writes a write line).
inline u64 NoteQueryRequest(u64 gpu, u64 device, u32 requested, u32 effective, u32 payload,
                            u32 flags, bool timestamp, bool fence, bool fallback,
                            QueryPath path) {
    State& s = Get();
    s.queries_requested.fetch_add(1, std::memory_order_relaxed);
    if (fallback) {
        s.queries_substituted.fetch_add(1, std::memory_order_relaxed);
    }
    u64 id;
    bool logged;
    {
        std::scoped_lock lock{s.query_mutex};
        id = ++s.query_ids;
        logged = TakeSlotLine(s, device);
        s.queries[id % s.queries.size()] = QueryReport{
            .id = id,
            .device = device,
            .value = payload,
            .request_ns = NowNs(),
            .requested = requested,
            .effective = effective,
            .payload = payload,
            .flags = flags,
            .status = QueryStatus::Pending,
            .path = path,
            .timestamp = timestamp,
            .fence = fence,
            .fallback = fallback,
            .logged = logged,
        };
    }
    if (logged) {
        LOG_INFO(HW_GPU,
                 "NX2 mesh count report request id={} gpu={:#x} device={:#x} type={} "
                 "effective_type={} flags={:#x} payload={:#x} path={} fallback_payload1={} "
                 "timestamp={} fence={}",
                 id, gpu, device, requested, effective, flags, payload, QueryPathName(path),
                 fallback, timestamp, fence);
    }
    return id;
}

/// Query cache: the report's guest write ran (or was discarded).
inline void NoteQueryWrite(u64 id, u64 value, QueryStatus status) {
    State& s = Get();
    const bool written = status == QueryStatus::Written || status == QueryStatus::Immediate;
    (written ? s.queries_written : s.queries_discarded).fetch_add(1, std::memory_order_relaxed);
    QueryReport report;
    u64 later_written{};
    {
        std::scoped_lock lock{s.query_mutex};
        if (written) {
            if (id < s.max_written_id) {
                later_written = s.max_written_id;
                s.queries_out_of_order.fetch_add(1, std::memory_order_relaxed);
            }
            s.max_written_id = std::max(s.max_written_id, id);
        }
        QueryReport& entry = s.queries[id % s.queries.size()];
        if (entry.id != id) {
            return;
        }
        if (written) {
            entry.value = value;
            entry.write_seq = ++s.write_seq;
        }
        entry.status = status;
        entry.resolve_ns = NowNs();
        report = entry;
    }
    if (report.logged) {
        LOG_INFO(HW_GPU,
                 "NX2 mesh count report write id={} write_seq={} device={:#x} type={} "
                 "effective_type={} path={} status={} value={:#x} after_us={} "
                 "later_request_written_first={}",
                 report.id, report.write_seq, report.device, report.requested, report.effective,
                 QueryPathName(report.path), QueryStatusName(report.status), report.value,
                 (report.resolve_ns - report.request_ns) / 1000, later_written);
    }
    if (report.watched_by == 0 || !TakeBudget(s.resolved_lines, MaxResolvedLines)) {
        return;
    }
    LOG_INFO(HW_GPU,
             "NX2 mesh count report resolved id={} device={:#x} type={} effective_type={} "
             "status={} value={:#x} payload={:#x} timestamp={} fence={} watched_by_dma={} "
             "after_us={}",
             report.id, report.device, report.requested, report.effective,
             QueryStatusName(report.status), report.value, report.payload, report.timestamp,
             report.fence, report.watched_by, (report.resolve_ns - report.request_ns) / 1000);
}

/// SamplesStreamer::WriteCounter with no host query running: the report's value is final at 0
/// (plus the streamer's amend value when it is written).
inline void NoteZpassNoCounter(u64 device, bool timestamp, bool started) {
    State& s = Get();
    s.zpass_no_counter.fetch_add(1, std::memory_order_relaxed);
    if (TakeBudget(s.zpass_no_counter_lines, MaxZpassLines)) {
        LOG_INFO(Render_Vulkan,
                 "NX2 mesh count zpass no-counter device={:#x} timestamp={} counter_started={} "
                 "host_query_active={} -> final value 0 + amend",
                 device, timestamp, started,
                 s.zpass_host_active.load(std::memory_order_relaxed));
    }
}

/// SamplesStreamer began (true) or ended (false) a host occlusion query.
inline void NoteZpassHost(bool active) {
    State& s = Get();
    s.zpass_host_active.store(active, std::memory_order_relaxed);
    if (active) {
        s.zpass_host_begins.fetch_add(1, std::memory_order_relaxed);
    }
}

/// The rasterizer passed `enable` to CounterEnable(ZPassPixelCount64) at `site`; logs the
/// transitions. `index` counts the calls in `frame`.
inline void NoteZpassEnable(bool& last, bool enable, const char* site, u64 frame, u32 index) {
    if (enable == last) {
        return;
    }
    last = enable;
    State& s = Get();
    (enable ? s.zpass_enable_on : s.zpass_enable_off).fetch_add(1, std::memory_order_relaxed);
    if (TakeBudget(s.zpass_enable_lines, MaxZpassLines)) {
        LOG_INFO(Render_Vulkan,
                 "NX2 mesh count zpass enable site={} enable={} host_query_active={} frame={} "
                 "call={}",
                 site, enable, s.zpass_host_active.load(std::memory_order_relaxed), frame, index);
    }
}

/// SamplesStreamer::PopUnsyncedQueries: the summed host result of a report landed.
inline void NoteZpassHostResult(u64 device, u64 value, size_t slots) {
    State& s = Get();
    (value == 0 ? s.zpass_results_zero : s.zpass_results_nonzero)
        .fetch_add(1, std::memory_order_relaxed);
    if (TakeBudget(s.zpass_result_lines, MaxZpassLines)) {
        LOG_INFO(Render_Vulkan,
                 "NX2 mesh count zpass host-result device={:#x} value={:#x} host_slots={}",
                 device, value, slots);
    }
}

/// The newest query report whose slot overlaps [begin, end) (device addresses); a pending one
/// is marked as read by DMA read `dma_serial` (its write then logs a resolved line).
inline QueryReport WatchQuery(u64 begin, u64 end, u64 dma_serial) {
    State& s = Get();
    std::scoped_lock lock{s.query_mutex};
    QueryReport* found = nullptr;
    for (QueryReport& entry : s.queries) {
        const u64 size = entry.timestamp ? 16 : 4;
        if (entry.id != 0 && entry.device < end && begin < entry.device + size &&
            (found == nullptr || entry.id > found->id)) {
            found = &entry;
        }
    }
    if (found == nullptr) {
        return {};
    }
    if (found->status == QueryStatus::Pending && found->watched_by == 0) {
        found->watched_by = dma_serial;
    }
    return *found;
}

/// "status=... type=A->B value=..." for a line.
inline std::string DescribeQueryReport(const QueryReport& q) {
    if (q.id == 0) {
        return "query=none";
    }
    const bool host_value = q.status == QueryStatus::Pending && q.effective != 0;
    return fmt::format("query id={} device={:#x} status={} type={}->{} {}={} timestamp={} "
                       "fence={} age_us={}",
                       q.id, q.device, QueryStatusName(q.status), q.requested, q.effective,
                       q.status == QueryStatus::Pending ? "will_write" : "wrote",
                       host_value ? std::string{"host"} : fmt::format("{:#x}", q.value),
                       q.timestamp, q.fence, (NowNs() - q.request_ns) / 1000);
}

/// Dumps the macro code (native_code words as uploaded), the data RAM and the start positions to
/// <EDEN_NX2_MESH_COUNT_TRACE_DIR or /tmp>/mme_code.bin, mme_data_ram.bin, mme_positions.txt.
inline void DumpMacroCode(std::span<const u32> code, std::span<const u32> data_ram,
                          std::span<const u32> positions, u32 start, u64 generation) {
    if (Get().code_dumped.exchange(true, std::memory_order_relaxed)) {
        return;
    }
    const char* env = std::getenv("EDEN_NX2_MESH_COUNT_TRACE_DIR");
    const std::string dir = env != nullptr && env[0] != '\0' ? env : "/tmp";
    const auto write = [&dir](const char* name, std::span<const u32> words) {
        const std::string path = dir + "/" + name;
        std::FILE* file = std::fopen(path.c_str(), "wb");
        if (file == nullptr) {
            LOG_WARNING(HW_GPU, "NX2 mesh count dump: cannot write {}", path);
            return;
        }
        std::fwrite(words.data(), sizeof(u32), words.size(), file);
        std::fclose(file);
    };
    write("mme_code.bin", code);
    write("mme_data_ram.bin", data_ram);
    const std::string path = dir + "/mme_positions.txt";
    if (std::FILE* file = std::fopen(path.c_str(), "w")) {
        std::fprintf(file, "# Macro start positions (instruction index) by macro slot\n");
        std::fprintf(file, "# zero_count_start=0x%x code_generation=%llu code_words=%zu\n", start,
                     static_cast<unsigned long long>(generation), code.size());
        for (size_t i = 0; i < positions.size(); ++i) {
            std::fprintf(file, "%zu 0x%x\n", i, positions[i]);
        }
        std::fclose(file);
    }
    LOG_INFO(HW_GPU, "NX2 mesh count dump: macro code, data RAM and positions in {} (start={:#x})",
             dir, start);
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
        if (r.report_hit) {
            s.dma_report_hits.fetch_add(1, std::memory_order_relaxed);
            s.dma_report_not_reached.fetch_add(r.report_reached_back ? 0 : 1,
                                               std::memory_order_relaxed);
        }
        s.dma_report_pending.fetch_add(r.query.status == QueryStatus::Pending ? 1 : 0,
                                       std::memory_order_relaxed);
        MacroCall& call = engine.call;
        if (call.recorded && engine.in_macro) {
            if (call.flow_count < call.flows.size()) {
                call.flows[call.flow_count++] = r;
            } else {
                ++call.flows_dropped;
            }
        }
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

    /// Split front: the reading macro's state and the reports over the source, once the wait for
    /// the back is done (the query cache's state is the one the read sees).
    void Front(u32 start, u32 ram, u32 dram_write_index) {
        DmaRead& r = engine.dma;
        r.start = start;
        r.ram = ram;
        r.emit_index = engine.call.emits;
        r.dram_write_index = dram_write_index;
        const u64 end = r.source + u64{r.words} * sizeof(u32);
        if (const ReportOp* op = FindReportOp(engine, r.source, end)) {
            r.report_hit = true;
            r.report = *op;
            r.report_offset = static_cast<s64>(r.source - op->gpu);
            r.report_reached_back =
                engine.back_report_serial.load(std::memory_order_acquire) >= op->serial;
        }
        if (r.device != 0) {
            r.query = WatchQuery(r.device, r.device + u64{r.words} * sizeof(u32), r.serial);
        }
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

/// Split front: `words` macro parameter words arrived from `source`; `first` when they start
/// the call's parameters.
inline void NoteParameters(EngineState& engine, bool first, PushSource source, u32 words) {
    if (source.path == EntryPath::Prefetch) {
        source.va = 0; // The pusher's address is not the prefetched list's
    }
    if (first) {
        engine.push = source;
        engine.push_words = words;
    } else if (engine.push.va != 0 &&
               source.va == engine.push.va + u64{engine.push_words} * sizeof(u32)) {
        engine.push_words += words;
    }
}

/// Split front, before a macro call runs: records its inputs from here on.
inline void BeginCall(EngineState& engine, u32 start, std::span<const u32> parameters) {
    MacroCall& call = engine.call;
    call = MacroCall{};
    call.recorded = true;
    call.start = start;
    call.call = engine.calls;
    call.param_count = static_cast<u32>(parameters.size());
    std::copy_n(parameters.begin(), std::min<size_t>(parameters.size(), MaxParams),
                call.params.begin());
    call.push = engine.push;
    call.push_words = engine.push_words;
    engine.push = {};
    engine.push_words = 0;
    if (engine.dram_origin.empty()) {
        engine.dram_origin.resize(DataRamWords);
    }
}

inline void NoteRegisterRead(EngineState& engine, u32 address, u32 value) {
    MacroCall& call = engine.call;
    if (call.reg_read_count < MaxRegReads) {
        call.reg_reads[call.reg_read_count++] = {address, value};
    }
}

inline void NoteEmit(EngineState& engine, u32 address, u32 value) {
    MacroCall& call = engine.call;
    if (call.emits < MaxEmits) {
        call.emitted[call.emits] = {address, value};
    }
    ++call.emits;
}

/// Who wrote a data RAM word: "d<dma serial>@<start>", "w<call>@<start>", "self" (this call's
/// DWRITE) or "?".
inline std::string DescribeOrigin(const EngineState& engine, u32 address,
                                  std::span<const std::pair<u32, u32>> own_writes) {
    if (std::ranges::find(own_writes, address, &std::pair<u32, u32>::first) != own_writes.end()) {
        return "self";
    }
    if (address >= engine.dram_origin.size()) {
        return "?";
    }
    const DramOrigin& origin = engine.dram_origin[address];
    switch (origin.kind) {
    case 1:
        return fmt::format("d{}@{:#x}", origin.serial, origin.start);
    case 2:
        return fmt::format("w{}@{:#x}", origin.serial, origin.start);
    default:
        return "?";
    }
}

/// True when a macro input line of this kind may be logged.
inline bool TakeMacroLine(bool zero) {
    State& s = Get();
    return TakeBudget(zero ? s.macro_zero_lines : s.macro_nonzero_lines, MaxMacroLines);
}

/// The traced macro wrote `value` to the count: its inputs so far (`dram_reads` and
/// `dram_writes` are the interpreter's for this call; `push` describes the parameters' source).
inline void LogMacroCount(const EngineState& engine, u32 value, bool mesh_control, u64 zero_seq,
                          u64 task, std::span<const std::pair<u32, u32>> dram_reads,
                          std::span<const std::pair<u32, u32>> dram_writes,
                          const std::string& push) {
    const MacroCall& call = engine.call;
    std::string dram;
    const size_t shown = std::min<size_t>(dram_reads.size(), MaxShown);
    for (size_t i = 0; i < shown; ++i) {
        const auto [address, word] = dram_reads[i];
        dram += fmt::format("{}{:#x}:{:#x}<{}", i == 0 ? "" : ",", address, word,
                            DescribeOrigin(engine, address, dram_writes));
    }
    if (dram_reads.size() > shown) {
        dram += fmt::format(",+{}", dram_reads.size() - shown);
    }
    const u64 n = Get().macro_lines_seq.fetch_add(1, std::memory_order_relaxed) + 1;
    LOG_INFO(HW_GPU,
             "NX2 mesh count macro n={} kind={} value={:#x} start={:#x} call={} emit={} "
             "mesh_control={} task={:#x} zero_seq={} params({})={:08x} reg_reads={} "
             "dram_reads={} dram_writes={} dma_reads_in_call={} | {}",
             n, value == 0 ? "zero" : "nonzero", value, call.start, call.call,
             call.emits == 0 ? 0 : call.emits - 1, mesh_control, task, zero_seq,
             call.param_count,
             fmt::join(std::span{call.params.data(), std::min(call.param_count, MaxParams)}, ","),
             JoinPairs(std::span{call.reg_reads.data(), call.reg_read_count}),
             dram.empty() ? std::string{"-"} : dram, JoinPairs(dram_writes), call.flow_count,
             push);
}

/// Split front, at the end of a macro call: what the call did with the words of each DMA read
/// (data RAM writes and methods after the read) and the report state over its source.
inline void LogFlows(const EngineState& engine, std::span<const std::pair<u32, u32>> dram_writes) {
    State& s = Get();
    const MacroCall& call = engine.call;
    for (u32 i = 0; i < call.flow_count; ++i) {
        const DmaRead& r = call.flows[i];
        const bool over_report = r.report_hit || r.query.id != 0;
        if (!TakeBudget(over_report ? s.report_flow_lines : s.other_flow_lines,
                        over_report ? MaxReportFlowLines : MaxOtherFlowLines)) {
            continue;
        }
        const size_t write_index = std::min<size_t>(r.dram_write_index, dram_writes.size());
        const u32 recorded = std::min(call.emits, MaxEmits);
        const u32 emit_index = std::min(r.emit_index, recorded);
        std::string emits =
            JoinPairs(std::span{call.emitted.data() + emit_index, recorded - emit_index});
        if (call.emits > MaxEmits) {
            emits += fmt::format(" (+{} unrecorded)", call.emits - MaxEmits);
        }
        const std::string report =
            r.report_hit
                ? fmt::format("report serial={} gpu={:#x} offset={} payload={:#x} {} "
                              "reached_back={} calls_ago={} by={}",
                              r.report.serial, r.report.gpu, r.report_offset, r.report.payload,
                              DescribeQuery(r.report.query), r.report_reached_back,
                              call.call - r.report.call,
                              r.report.start == ~0U ? std::string{"direct"}
                                                    : fmt::format("{:#x}", r.report.start))
                : std::string{"report=none"};
        LOG_INFO(HW_GPU,
                 "NX2 mesh count dma flow serial={} start={:#x} call={} gpu={:#x} device={:#x} "
                 "words={} fifoed={} ram={:#x} first={:08x} | {} | {} | dram_writes_after={} "
                 "emits_after={}{}",
                 r.serial, r.start, call.call, r.source, r.device, r.words, r.fifoed, r.ram,
                 fmt::join(std::span{r.first.data(), r.sampled}, ","), report,
                 DescribeQueryReport(r.query), JoinPairs(dram_writes.subspan(write_index)), emits,
                 call.flows_dropped != 0 && i + 1 == call.flow_count
                     ? fmt::format(" (+{} dma reads unrecorded)", call.flows_dropped)
                     : std::string{});
    }
}

/// Split front, after a macro call: its DWRITEs become the writers of those data RAM words.
inline void EndCall(EngineState& engine, std::span<const std::pair<u32, u32>> dram_writes) {
    for (const auto& [address, value] : dram_writes) {
        if (address < engine.dram_origin.size()) {
            engine.dram_origin[address] =
                DramOrigin{.serial = engine.call.call, .start = engine.call.start, .kind = 2};
        }
    }
    engine.call.recorded = false;
}

/// Split front: a non-fifoed DMA read wrote data RAM [ram, ram + words).
inline void NoteDmaDataRam(EngineState& engine, u32 ram, u32 words) {
    for (u32 i = 0; i < words && ram + i < engine.dram_origin.size(); ++i) {
        engine.dram_origin[ram + i] =
            DramOrigin{.serial = engine.dma.serial, .start = engine.dma.start, .kind = 1};
    }
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
