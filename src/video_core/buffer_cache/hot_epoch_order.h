// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>

#include "common/common_types.h"
#include "video_core/nx2_hot_pages.h"

// Per draw and per vertex slot: keep them inlined into BindHostVertexBuffers' loop.
#ifdef _MSC_VER
#define HOT_EPOCH_ORDER_INLINE __forceinline
#else
#define HOT_EPOCH_ORDER_INLINE inline __attribute__((always_inline))
#endif

/// EDEN_NX2_HOT_PAGES: when the buffer cache applies an epoch (marks the hot pages written) and
/// the vertex binding steps that read what it changes. Templates over the tracker and the steps so
/// the tests run the same order (src/tests/video_core/hot_epoch_order.cpp).
namespace VideoCommon::HotEpochOrder {

/// Whether the guest passed an epoch since `applied` while pages are hot. When nothing changed:
/// one acquire load and a compare.
template <typename Tracker>
[[nodiscard]] HOT_EPOCH_ORDER_INLINE bool Pending(const Tracker& tracker, u64 applied) noexcept {
    return Nx2HotPages::g_epoch.load(std::memory_order_acquire) != applied &&
           tracker.HasHotPages();
}

/// Marks the hot pages written if the guest passed an epoch since `applied`, calling
/// on_run(address, size) for each run of them. Returns the pages marked. With no hot pages
/// `applied` stays: a page that becomes hot later is marked at the next call.
template <typename Tracker, typename OnRun>
u64 Apply(Tracker& tracker, u64& applied, OnRun&& on_run) {
    if (!tracker.HasHotPages()) {
        return 0;
    }
    const u64 epoch = Nx2HotPages::g_epoch.load(std::memory_order_acquire);
    if (epoch == applied) {
        return 0;
    }
    applied = epoch;
    return tracker.MarkHotPagesWritten(on_run);
}

/// Before BufferCache::BindHostVertexBuffers looks at any slot.
/// Steps: RefreshRepackedVertexBuffers().
template <typename Steps>
HOT_EPOCH_ORDER_INLINE void PrepareVertexBindings(Steps& steps) {
    steps.RefreshRepackedVertexBuffers();
}

/// Uploads vertex slot `index` (`slot`: its buffer and binding) before its bind.
/// EDEN_NX2_BUFFER_SKIP skips a slot whose range nothing wrote since its upload (verify uploads it
/// anyway and counts a mismatch). Steps: Synchronize(slot) -> true when nothing was uploaded,
/// IsSynchronized(index), WatchSynchronized(index, slot), and the counters NoteSync(),
/// NoteSkip(), NoteMismatch().
template <typename Steps, typename Slot>
HOT_EPOCH_ORDER_INLINE void SynchronizeVertexBuffer(Steps& steps, u32 index, const Slot& slot,
                                                    bool buffer_skip, bool verify) {
    if (!buffer_skip) {
        steps.Synchronize(slot);
        return;
    }
    steps.NoteSync();
    if (steps.IsSynchronized(index)) {
        steps.NoteSkip();
        if (verify && !steps.Synchronize(slot)) {
            steps.NoteMismatch();
        }
        return;
    }
    steps.Synchronize(slot);
    steps.WatchSynchronized(index, slot);
}

} // namespace VideoCommon::HotEpochOrder

#undef HOT_EPOCH_ORDER_INLINE
