// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <initializer_list>
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
// tests run the same templates and tracker with a model of the rest: vertex slots over one page
// each, their host buffer, and for a repacked slot the repacked copy the runtime assembles only
// when the slot's dirty flag is set (vk_buffer_cache.cpp BindVertexBuffers). The model's steps
// mirror BufferCache's (VertexBindSteps, SynchronizeBuffer's own epoch check,
// MarkVertexBuffersStale, RefreshRepackedVertexBuffers); the Vulkan runtime and the rasterizer are
// not exercised.

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
    HotPagesOn() : previous{VideoCommon::Nx2HotPages::g_enabled} {
        VideoCommon::Nx2HotPages::g_enabled = true;
    }
    ~HotPagesOn() {
        VideoCommon::Nx2HotPages::g_enabled = previous;
    }
    bool previous;
};

/// Vertex slots bound once and never rebound: slot i over the page at VERTEX_PAGE + i * PAGE.
struct VertexModel {
    struct Slot {
        u32 index;
    };

    explicit VertexModel(std::initializer_list<bool> repacked_slots)
        : repacked{repacked_slots}, dirty(repacked.size()),
          repacked_copy(repacked.size(), std::vector<u8>(PAGE, 0)) {
        applied = VideoCommon::Nx2HotPages::g_epoch.load();
    }

    [[nodiscard]] static DAddr Page(u32 index) noexcept {
        return VERTEX_PAGE + index * PAGE;
    }

    // HotEpochOrder steps (BufferCache::VertexBindSteps).
    void ApplyPendingHotEpoch() {
        VideoCommon::HotEpochOrder::ApplyIfPending(tracker, applied, [this] { ApplyHotEpoch(); });
    }
    void RefreshRepackedVertexBuffers() {
        const u32 mask = std::exchange(stale, 0);
        for (u32 index = 0; index < repacked.size(); ++index) {
            if (((mask >> index) & 1) != 0 && repacked[index]) {
                dirty[index] = true;
            }
        }
    }
    bool Synchronize(const Slot& slot) {
        // SynchronizeBuffer applies the epoch itself first.
        ApplyHotEpoch();
        bool uploaded = false;
        tracker.ForEachUploadRange(Page(slot.index), PAGE, [&](u64 addr, u64 size) {
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
    void WatchSynchronized(u32 index, const Slot& slot) {
        tracker.WatchSynchronized(index, Page(slot.index), PAGE);
    }
    void NoteSync() {}
    void NoteSkip() {
        ++skips;
    }
    void NoteMismatch() {}

    // BufferCache::ApplyHotEpoch with MarkVertexBuffersStale.
    void ApplyHotEpoch() {
        epochs_marked += VideoCommon::HotEpochOrder::Apply(tracker, applied, [&](u64 addr, u64 size) {
            for (u32 index = 0; index < repacked.size(); ++index) {
                if (addr < Page(index) + PAGE && Page(index) < addr + size) {
                    stale |= 1U << index;
                    ever_stale |= 1U << index;
                }
            }
        });
    }

    /// BindHostVertexBuffers' optimized loop. `before_slots` runs between the prologue and the
    /// first slot (an epoch the front publishes while the back binds).
    void Draw(bool buffer_skip, bool verify = false, const std::function<void()>& before_slots = {}) {
        VideoCommon::HotEpochOrder::PrepareVertexBindings(*this);
        if (before_slots) {
            before_slots();
        }
        for (u32 index = 0; index < repacked.size(); ++index) {
            VideoCommon::HotEpochOrder::SynchronizeVertexBuffer(*this, index, Slot{index},
                                                                buffer_skip, verify);
            if (dirty[index]) {
                dirty[index] = false;
                if (repacked[index]) {
                    const auto source = host.begin() + (Page(index) - VERTEX_PAGE);
                    std::copy(source, source + PAGE, repacked_copy[index].begin());
                }
            }
        }
    }

    /// What the last draw read through slot `index`.
    [[nodiscard]] std::vector<u8> Reads(u32 index) const {
        if (repacked[index]) {
            return repacked_copy[index];
        }
        const auto source = host.begin() + (Page(index) - VERTEX_PAGE);
        return {source, source + PAGE};
    }
    /// The guest's bytes under slot `index`.
    [[nodiscard]] std::vector<u8> Guest(u32 index) const {
        const auto source = guest.begin() + (Page(index) - VERTEX_PAGE);
        return {source, source + PAGE};
    }

    /// The guest rewrites slot `index`'s page. Nothing traps a write to a hot page; a write to a
    /// protected one marks it CPU-modified (`trapped`).
    void GuestWrite(u32 index, u8 value, bool trapped = false) {
        const auto begin = guest.begin() + (Page(index) - VERTEX_PAGE);
        std::fill(begin, begin + PAGE, value);
        if (trapped) {
            tracker.MarkRegionAsCpuModified(Page(index), PAGE);
        }
    }

    /// The guest writes slot `index`'s page three times after its uploads: the third makes it hot
    /// (memory_tracker.cpp MakeHot).
    void MakeHot(u32 index) {
        for (int i = 0; i < 4; ++i) {
            if (i != 0) {
                tracker.MarkRegionAsCpuModified(Page(index) + 8, 8);
            }
            tracker.ForEachUploadRange(Page(index), PAGE, [](u64, u64) {});
        }
        REQUIRE(tracker.HasHotPages());
        REQUIRE(rasterizer.Count(Page(index)) == 0);
    }

    /// Slots in `hot` are made hot, the others are uploaded once and stay protected. Then the
    /// guest writes the hot ones and publishes them, a draw binds every slot and another one draws
    /// with them unchanged (all clean, no stale bit left).
    void BindHot(bool buffer_skip, u32 hot = 1) {
        for (u32 index = 0; index < repacked.size(); ++index) {
            if (((hot >> index) & 1) != 0) {
                MakeHot(index);
                GuestWrite(index, 0x11);
            } else {
                tracker.ForEachUploadRange(Page(index), PAGE, [](u64, u64) {});
                REQUIRE(rasterizer.Count(Page(index)) == 1);
            }
        }
        VideoCommon::Nx2HotPages::BumpEpoch();
        std::fill(dirty.begin(), dirty.end(), true); // new bindings
        for (int draw = 0; draw < 2; ++draw) {
            Draw(buffer_skip);
            for (u32 index = 0; index < repacked.size(); ++index) {
                REQUIRE(Reads(index) == Guest(index));
            }
        }
        REQUIRE_FALSE(VideoCommon::HotEpochOrder::Pending(tracker, applied));
        REQUIRE(stale == 0);
        REQUIRE(std::find(dirty.begin(), dirty.end(), true) == dirty.end());
        ever_stale = 0;
    }

    RasterizerInterface rasterizer;
    MemoryTracker tracker{rasterizer};
    std::vector<bool> repacked;
    std::vector<bool> dirty;
    std::vector<std::vector<u8>> repacked_copy;
    std::vector<u8> guest = std::vector<u8>(2 * PAGE, 0);
    std::vector<u8> host = std::vector<u8>(2 * PAGE, 0);
    u64 applied{};
    u32 stale{};
    u32 ever_stale{};
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
    model.GuestWrite(0, 0x22);
    VideoCommon::Nx2HotPages::BumpEpoch();
    model.Draw(false);
    REQUIRE(model.Reads(0) == model.Guest(0));
    REQUIRE(model.epochs_marked == 2);

    // No epoch, no write: nothing uploaded or reassembled.
    const int uploads = model.uploads;
    model.Draw(false);
    REQUIRE(model.Reads(0) == model.Guest(0));
    REQUIRE(model.uploads == uploads);
}

TEST_CASE("HotEpochOrder: a repacked slot on a hot page after a slot on a protected page",
          "[video_core]") {
    // Slot 0 on a protected page the guest also wrote, slot 1 repacked on the next page, hot.
    // Before the fix slot 0's upload applied the epoch, after the refresh had read the stale bits.
    HotPagesOn on;
    for (const bool buffer_skip : {false, true}) {
        VertexModel model{false, true};
        model.BindHot(buffer_skip, 0b10);
        const u64 marked = model.epochs_marked;

        model.GuestWrite(0, 0x55, true);
        model.GuestWrite(1, 0x66);
        VideoCommon::Nx2HotPages::BumpEpoch();
        model.Draw(buffer_skip);
        REQUIRE(model.Reads(0) == model.Guest(0));
        REQUIRE(model.Reads(1) == model.Guest(1));
        REQUIRE(model.epochs_marked == marked + 1);
        // The hot run covers slot 1's page only.
        REQUIRE(model.ever_stale == 0b10);
        REQUIRE(model.rasterizer.Count(VertexModel::Page(0)) == 1);
        REQUIRE(model.rasterizer.Count(VertexModel::Page(1)) == 0);
    }
}

TEST_CASE("HotEpochOrder: an epoch published while the slots bind is caught up by the next draw",
          "[video_core]") {
    // The prologue checks the epoch once. One bumped after it (the front thread, mid-bind) is
    // applied by a later upload or the next draw's prologue: an accepted lag of one draw.
    HotPagesOn on;
    for (const bool repacked : {false, true}) {
        for (const bool buffer_skip : {false, true}) {
            VertexModel model{repacked};
            model.BindHot(buffer_skip);
            const auto old_bytes = model.Guest(0);

            model.Draw(buffer_skip, false, [&] {
                model.GuestWrite(0, 0x77);
                VideoCommon::Nx2HotPages::BumpEpoch();
            });
            // Uploaded at once when the slot's upload runs and nothing is reassembled;
            // otherwise this draw reads the bytes from before the epoch.
            const bool at_once = !repacked && !buffer_skip;
            REQUIRE(model.Reads(0) == (at_once ? model.Guest(0) : old_bytes));

            model.Draw(buffer_skip);
            REQUIRE(model.Reads(0) == model.Guest(0));
            REQUIRE_FALSE(VideoCommon::HotEpochOrder::Pending(model.tracker, model.applied));
            REQUIRE(model.stale == 0);
        }
    }
}

TEST_CASE("HotEpochOrder: EDEN_NX2_BUFFER_SKIP uploads a rewritten hot vertex page",
          "[video_core]") {
    HotPagesOn on;
    VertexModel model{false};
    model.BindHot(true);

    // Synchronized and no epoch: the upload is skipped (the second draw of BindHot was too).
    const int syncs = model.syncs;
    model.Draw(true);
    REQUIRE(model.Reads(0) == model.Guest(0));
    REQUIRE(model.syncs == syncs);
    REQUIRE(model.skips == 2);

    // The guest rewrites it and publishes it: the epoch ends the synchronized range first.
    model.GuestWrite(0, 0x33);
    VideoCommon::Nx2HotPages::BumpEpoch();
    model.Draw(true);
    REQUIRE(model.Reads(0) == model.Guest(0));
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
            model.GuestWrite(0, 0x44);
            VideoCommon::Nx2HotPages::BumpEpoch();
            REQUIRE(VideoCommon::HotEpochOrder::Pending(model.tracker, model.applied));
            model.ApplyHotEpoch();
            REQUIRE(model.epochs_marked == marked + 1);
            REQUIRE_FALSE(VideoCommon::HotEpochOrder::Pending(model.tracker, model.applied));

            // The draw marks nothing more and uploads once.
            model.Draw(buffer_skip);
            REQUIRE(model.Reads(0) == model.Guest(0));
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
