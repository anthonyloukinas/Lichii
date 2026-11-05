// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstdlib>
#include <vector>

#include "common/common_types.h"

/// EDEN_NX2_HOT_PAGES (default on): pages the guest CPU keeps rewriting stay writable.
///
/// The buffer cache write-protects a page once it has uploaded it; the guest's next write faults,
/// takes the slow path, and the page is unprotected until the next upload protects it again.
/// For a page rewritten every frame that is two mprotect calls (one on the GPU thread) and a fault
/// per frame. A page whose protection came off PromoteAfter times while not GPU-modified becomes
/// hot: uploads leave it writable (the buffer cache's count on it stays dropped).
///
/// Nothing notices a write to a hot page, so the buffer cache treats each one as written whenever
/// the guest may have written it for the GPU work that follows: at every guest submission and every
/// semaphore acquire the front gets past (g_epoch). Before the next upload, and before a draw
/// reuses its bindings, it marks every hot page CPU-modified again with a CPU write's side effects
/// (BufferCache::ApplyHotEpoch). Writes before the submission (or the semaphore release) are
/// visible to the upload that follows, as they were to the fault.
///
/// A hot page is never GPU-modified: GPU-modified state is ignored on unprotected pages. A GPU write
/// to a hot page protects it first and keeps it CPU-modified (uploads skip GPU-written bytes).
/// Every DemoteFrames frames all hot pages are protected again and have to earn the state anew.
namespace VideoCommon::Nx2HotPages {

inline bool g_enabled = [] { // not const: tests switch it
    const char* value = std::getenv("EDEN_NX2_HOT_PAGES");
    return value == nullptr || value[0] != '0';
}();

inline constexpr u32 PromoteAfter = 3;
inline constexpr size_t MaxPages = 2048;
inline constexpr u64 DemoteFrames = 600;
/// Protection losses older than this many frames are forgotten (PromoteAfter counts in a window).
inline constexpr u64 DecayFrames = 120;

/// Points the guest's CPU writes so far must be visible to GPU work processed after them.
inline std::atomic<u64> g_epoch{1};

/// Call after the guest's writes it publishes (a submission) or after reading the semaphore
/// word that met an acquire.
inline void BumpEpoch() {
    g_epoch.fetch_add(1, std::memory_order_acq_rel);
}

struct Counters {
    // Host protection changes (MaxwellDeviceMethods::MarkRegionCaching): calls and pages,
    // protect (cached) and unprotect, and the protects issued from the split's back.
    std::atomic<u64> protect_calls{};
    std::atomic<u64> protect_pages{};
    std::atomic<u64> unprotect_calls{};
    std::atomic<u64> unprotect_pages{};
    std::atomic<u64> back_protect_calls{};
    // Hot pages: promoted, cooled by a GPU write or an unmark, demoted, uploads that left one
    // writable, epochs applied, pages marked written by them.
    std::atomic<u64> promoted{};
    std::atomic<u64> cooled{};
    std::atomic<u64> demoted{};
    std::atomic<u64> kept{};
    std::atomic<u64> epochs{};
    std::atomic<u64> remarked{};
};
inline Counters g_counters;

inline void Count(std::atomic<u64>& counter, u64 value) {
    counter.fetch_add(value, std::memory_order_relaxed);
}

/// Device addresses of hot pages (some may have cooled since: the page's Hot bit decides).
/// Changed under the buffer cache's lock; `size` may be read without it.
struct HotList {
    std::vector<u64> pages;
    std::atomic<size_t> size{};
};

} // namespace VideoCommon::Nx2HotPages
