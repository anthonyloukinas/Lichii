// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <span>
#include <stdexcept>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/common_types.h"
#include "common/container/unordered_map.h"
#include "core/device_memory.h"
#include "video_core/buffer_cache/hot_epoch_order.h"
#include "video_core/buffer_cache/memory_tracker_base.h"
#include "video_core/nx2_hot_pages.h"

// BufferCache::BindHostVertexBuffers runs HotEpochOrder's steps on the real memory tracker. These
// tests run the same templates and tracker with a model of the rest: one vertex slot over one
// page, its host buffer, and for a repacked slot the repacked copy the runtime assembles only when
// the slot's dirty flag is set (vk_buffer_cache.cpp BindVertexBuffers). The model's steps mirror
// BufferCache's (VertexBindSteps, SynchronizeBuffer's own epoch check, MarkVertexBuffersStale,
// RefreshRepackedVertexBuffers); the Vulkan runtime and the rasterizer are not exercised.

namespace {

constexpr u64 PAGE = 4096;
constexpr u64 HIGH_PAGE_SIZE = 1ULL << 22;
constexpr DAddr VERTEX_PAGE = 16 * HIGH_PAGE_SIZE;

class RasterizerInterface {
public:
    void UpdatePagesCachedCount(DAddr addr, size_t size, s32 delta) {
        Apply(addr, size, delta);
    }

    void UpdatePagesCachedBatch(std::span<const std::pair<DAddr, size_t>> ranges, s32 delta) {
        for (const auto& [addr, size] : ranges) {
            Apply(addr, size, delta);
        }
    }

    [[nodiscard]] int Count(DAddr addr) const noexcept {
        const auto it = page_table.find(addr >> Core::DEVICE_PAGEBITS);
        return it == page_table.end() ? 0 : it->second;
    }

private:
    void Apply(DAddr addr, size_t size, s32 delta) {
        const u64 page_start{addr >> Core::DEVICE_PAGEBITS};
        const u64 page_end{(addr + size + Core::DEVICE_PAGESIZE - 1) >> Core::DEVICE_PAGEBITS};
        for (u64 page = page_start; page < page_end; ++page) {
            int& value = page_table[page];
            value += delta;
            if (value == 0) {
                page_table.erase(page);
            } else if (value < 0) {
                throw std::logic_error{"negative page"};
            }
        }
    }

    ::Common::unordered_map<u64, int> page_table;
};

using MemoryTracker = VideoCommon::MemoryTrackerBase<RasterizerInterface>;

struct HotPagesOn {
    HotPagesOn() {
        VideoCommon::Nx2HotPages::g_enabled = true;
    }
    ~HotPagesOn() {
        VideoCommon::Nx2HotPages::g_enabled = false;
    }
};

/// One vertex slot (index 0) over VERTEX_PAGE, bound once and never rebound.
struct VertexModel {
    struct Slot {};

    explicit VertexModel(bool repacked_) : repacked{repacked_} {
        applied = VideoCommon::Nx2HotPages::g_epoch.load();
    }

    // HotEpochOrder steps (BufferCache::VertexBindSteps).
    void ApplyPendingHotEpoch() {
        if (VideoCommon::HotEpochOrder::Pending(tracker, applied)) {
            ApplyHotEpoch();
        }
    }
    void RefreshRepackedVertexBuffers() {
        if (std::exchange(stale, false) && repacked) {
            dirty = true;
        }
    }
    bool Synchronize(const Slot&) {
        // SynchronizeBuffer applies the epoch itself first.
        ApplyHotEpoch();
        bool uploaded = false;
        tracker.ForEachUploadRange(VERTEX_PAGE, PAGE, [&](u64 addr, u64 size) {
            const u64 offset = addr - VERTEX_PAGE;
            std::memcpy(host.data() + offset, guest.data() + offset, size);
            uploaded = true;
        });
        ++syncs;
        uploads += uploaded ? 1 : 0;
        return !uploaded;
    }
    bool IsSynchronized(u32 index) const {
        return tracker.IsSynchronized(index);
    }
    void WatchSynchronized(u32 index, const Slot&) {
        tracker.WatchSynchronized(index, VERTEX_PAGE, PAGE);
    }
    void NoteSync() {}
    void NoteSkip() {
        ++skips;
    }
    void NoteMismatch() {}

    // BufferCache::ApplyHotEpoch with MarkVertexBuffersStale for the one slot.
    void ApplyHotEpoch() {
        epochs_marked += VideoCommon::HotEpochOrder::Apply(tracker, applied, [&](u64 addr, u64 size) {
            if (addr < VERTEX_PAGE + PAGE && VERTEX_PAGE < addr + size) {
                stale = true;
            }
        });
    }

    /// BindHostVertexBuffers for the slot, then what the draw reads.
    std::vector<u8> Draw(bool buffer_skip, bool verify = false) {
        VideoCommon::HotEpochOrder::PrepareVertexBindings(*this);
        VideoCommon::HotEpochOrder::SynchronizeVertexBuffer(*this, 0, Slot{}, buffer_skip,
                                                            verify);
        if (dirty) {
            dirty = false;
            if (repacked) {
                repacked_copy = host;
            }
        }
        return repacked ? repacked_copy : host;
    }

    /// The guest rewrites the page; nothing traps a write to a hot page.
    void GuestWrite(u8 value) {
        std::ranges::fill(guest, value);
    }

    /// The guest writes the page three times after its uploads: the third makes it hot
    /// (memory_tracker.cpp MakeHot), then the guest publishes it, a draw binds the slot and
    /// another one draws with it unchanged (the slot is clean, no stale bit left).
    void BindHot(bool buffer_skip) {
        for (int i = 0; i < 4; ++i) {
            if (i != 0) {
                tracker.MarkRegionAsCpuModified(VERTEX_PAGE + 8, 8);
            }
            tracker.ForEachUploadRange(VERTEX_PAGE, PAGE, [](u64, u64) {});
        }
        REQUIRE(tracker.HasHotPages());
        REQUIRE(rasterizer.Count(VERTEX_PAGE) == 0);
        GuestWrite(0x11);
        VideoCommon::Nx2HotPages::BumpEpoch();
        dirty = true; // a new binding
        REQUIRE(Draw(buffer_skip) == guest);
        REQUIRE(Draw(buffer_skip) == guest);
        REQUIRE_FALSE(VideoCommon::HotEpochOrder::Pending(tracker, applied));
        REQUIRE_FALSE(stale);
        REQUIRE_FALSE(dirty);
    }

    RasterizerInterface rasterizer;
    MemoryTracker tracker{rasterizer};
    std::vector<u8> guest = std::vector<u8>(PAGE, 0);
    std::vector<u8> host = std::vector<u8>(PAGE, 0);
    std::vector<u8> repacked_copy = std::vector<u8>(PAGE, 0);
    u64 applied{};
    bool repacked{};
    bool stale{};
    bool dirty{};
    u64 epochs_marked{};
    int syncs{};
    int uploads{};
    int skips{};
};

} // Anonymous namespace

TEST_CASE("HotEpochOrder: a repacked vertex slot on a rewritten hot page is reassembled at once",
          "[video_core]") {
    HotPagesOn on;
    VertexModel model{true};
    model.BindHot(false);

    // Same bindings and layout: the dirty flag is clean until the epoch makes the slot stale.
    model.GuestWrite(0x22);
    VideoCommon::Nx2HotPages::BumpEpoch();
    REQUIRE(model.Draw(false) == model.guest);
    REQUIRE(model.epochs_marked == 2);

    // No epoch, no write: nothing uploaded or reassembled.
    const int uploads = model.uploads;
    REQUIRE(model.Draw(false) == model.guest);
    REQUIRE(model.uploads == uploads);
}

TEST_CASE("HotEpochOrder: EDEN_NX2_BUFFER_SKIP uploads a rewritten hot vertex page",
          "[video_core]") {
    HotPagesOn on;
    VertexModel model{false};
    model.BindHot(true);

    // Synchronized and no epoch: the upload is skipped (the second draw of BindHot was too).
    const int syncs = model.syncs;
    REQUIRE(model.Draw(true) == model.guest);
    REQUIRE(model.syncs == syncs);
    REQUIRE(model.skips == 2);

    // The guest rewrites it and publishes it: the epoch ends the synchronized range first.
    model.GuestWrite(0x33);
    VideoCommon::Nx2HotPages::BumpEpoch();
    REQUIRE(model.Draw(true) == model.guest);
    REQUIRE(model.skips == 2);
    REQUIRE(model.IsSynchronized(0));
}

TEST_CASE("HotEpochOrder: an epoch applied before the draw (fast draws) is not applied again",
          "[video_core]") {
    HotPagesOn on;
    for (const bool repacked : {false, true}) {
        for (const bool buffer_skip : {false, true}) {
            VertexModel model{repacked};
            model.BindHot(buffer_skip);
            const u64 marked = model.epochs_marked;
            const int uploads = model.uploads;

            // RasterizerVulkan::TakeReusableDraw applies a pending epoch before the draw.
            model.GuestWrite(0x44);
            VideoCommon::Nx2HotPages::BumpEpoch();
            REQUIRE(VideoCommon::HotEpochOrder::Pending(model.tracker, model.applied));
            model.ApplyHotEpoch();
            REQUIRE(model.epochs_marked == marked + 1);
            REQUIRE_FALSE(VideoCommon::HotEpochOrder::Pending(model.tracker, model.applied));

            // The draw marks nothing more and uploads once.
            REQUIRE(model.Draw(buffer_skip) == model.guest);
            REQUIRE(model.epochs_marked == marked + 1);
            REQUIRE(model.uploads == uploads + 1);
        }
    }
}

TEST_CASE("HotEpochOrder: Pending and Apply", "[video_core]") {
    HotPagesOn on;
    RasterizerInterface rasterizer;
    MemoryTracker tracker{rasterizer};
    u64 applied = VideoCommon::Nx2HotPages::g_epoch.load();
    const auto apply = [&] {
        return VideoCommon::HotEpochOrder::Apply(tracker, applied, [](u64, u64) {});
    };

    // No hot pages: nothing pending and the applied epoch stays.
    VideoCommon::Nx2HotPages::BumpEpoch();
    REQUIRE_FALSE(VideoCommon::HotEpochOrder::Pending(tracker, applied));
    const u64 before = applied;
    REQUIRE(apply() == 0);
    REQUIRE(applied == before);

    // A page made hot since is marked at the next apply, once per epoch.
    for (int i = 0; i < 4; ++i) {
        if (i != 0) {
            tracker.MarkRegionAsCpuModified(VERTEX_PAGE + 8, 8);
        }
        tracker.ForEachUploadRange(VERTEX_PAGE, PAGE, [](u64, u64) {});
    }
    REQUIRE(tracker.HasHotPages());
    REQUIRE(VideoCommon::HotEpochOrder::Pending(tracker, applied));
    REQUIRE(apply() == 1);
    REQUIRE(applied == VideoCommon::Nx2HotPages::g_epoch.load());
    REQUIRE_FALSE(VideoCommon::HotEpochOrder::Pending(tracker, applied));
    REQUIRE(apply() == 0);
    VideoCommon::Nx2HotPages::BumpEpoch();
    REQUIRE(VideoCommon::HotEpochOrder::Pending(tracker, applied));
    REQUIRE(apply() == 1);
}
