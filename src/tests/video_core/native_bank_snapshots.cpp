// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstring>

#include <catch2/catch_test_macros.hpp>

#include "video_core/buffer_cache/native_bank_snapshots.h"

namespace {

using VideoCommon::NativeBankSnapshots;
constexpr u32 PAGE = NativeBankSnapshots::PAGE_SIZE;

std::array<u8, PAGE> Page(u8 value) {
    std::array<u8, PAGE> page;
    page.fill(value);
    return page;
}

} // namespace

TEST_CASE("Native bank snapshots hold what was uploaded and nothing else",
          "[video_core][native_bank_snapshots]") {
    NativeBankSnapshots snapshots;
    auto guest = Page(0x11);
    REQUIRE_FALSE(snapshots.Unchanged(0x10000, 1, 0, PAGE, guest.data()));
    const u32 slot = snapshots.Record(0x10000, 1, 0x100, 0x800, guest.data() + 0x100);
    REQUIRE(snapshots.Bytes(slot)[0x100] == 0x11);
    REQUIRE(snapshots.Unchanged(0x10000, 1, 0x100, 0x800, guest.data() + 0x100));
    REQUIRE(snapshots.Unchanged(0x10000, 1, 0x200, 0x300, guest.data() + 0x200));
    // Bytes the last upload did not send are not known to be on the host.
    REQUIRE_FALSE(snapshots.Unchanged(0x10000, 1, 0, 0x800, guest.data()));
    REQUIRE_FALSE(snapshots.Unchanged(0x10000, 1, 0x100, 0x801, guest.data() + 0x100));
    // Another host buffer holding the page.
    REQUIRE_FALSE(snapshots.Unchanged(0x10000, 2, 0x100, 0x800, guest.data() + 0x100));
    // A changed byte.
    guest[0x7ff] = 0x22;
    REQUIRE_FALSE(snapshots.Unchanged(0x10000, 1, 0x100, 0x800, guest.data() + 0x100));
    snapshots.Record(0x10000, 1, 0x100, 0x800, guest.data() + 0x100);
    REQUIRE(snapshots.Unchanged(0x10000, 1, 0x100, 0x800, guest.data() + 0x100));
}

TEST_CASE("Native bank snapshots join touching ranges of one buffer",
          "[video_core][native_bank_snapshots]") {
    NativeBankSnapshots snapshots;
    const auto guest = Page(0x33);
    snapshots.Record(0x10000, 1, 0x800, PAGE, guest.data() + 0x800);
    snapshots.Record(0x10000, 1, 0x400, 0x800, guest.data() + 0x400);
    REQUIRE(snapshots.Unchanged(0x10000, 1, 0x400, PAGE, guest.data() + 0x400));
    // A range apart from the held one replaces it.
    snapshots.Record(0x10000, 1, 0, 0x100, guest.data());
    REQUIRE(snapshots.Unchanged(0x10000, 1, 0, 0x100, guest.data()));
    REQUIRE_FALSE(snapshots.Unchanged(0x10000, 1, 0x400, PAGE, guest.data() + 0x400));
    // So does another buffer's.
    snapshots.Record(0x10000, 2, 0x100, 0x200, guest.data() + 0x100);
    REQUIRE_FALSE(snapshots.Unchanged(0x10000, 1, 0, 0x100, guest.data()));
    REQUIRE(snapshots.Unchanged(0x10000, 2, 0x100, 0x200, guest.data() + 0x100));
}

TEST_CASE("Native bank snapshots drop the pages other writes reach",
          "[video_core][native_bank_snapshots]") {
    NativeBankSnapshots snapshots;
    const auto guest = Page(0x44);
    for (DAddr page = 0x10000; page < 0x14000; page += PAGE) {
        snapshots.Record(page, 1, 0, PAGE, guest.data());
    }
    REQUIRE(snapshots.Size() == 4);
    // Outside the held pages.
    snapshots.Invalidate(0x8000, 0x8000);
    snapshots.Invalidate(0x14000, 0x1000);
    REQUIRE(snapshots.Size() == 4);
    // One byte at the end of a page drops that page only.
    snapshots.Invalidate(0x10fff, 1);
    REQUIRE(snapshots.Size() == 3);
    REQUIRE_FALSE(snapshots.Unchanged(0x10000, 1, 0, PAGE, guest.data()));
    REQUIRE(snapshots.Unchanged(0x11000, 1, 0, PAGE, guest.data()));
    // A range wider than the entries walks the entries instead of the pages.
    snapshots.Invalidate(0x11800, 1ULL << 40);
    REQUIRE(snapshots.Size() == 0);
    // Freed slots are reused.
    snapshots.Record(0x20000, 1, 0, PAGE, guest.data());
    REQUIRE(snapshots.Unchanged(0x20000, 1, 0, PAGE, guest.data()));
}

TEST_CASE("Native bank snapshots drop everything at their cap",
          "[video_core][native_bank_snapshots]") {
    NativeBankSnapshots snapshots;
    const auto guest = Page(0x55);
    REQUIRE_FALSE(snapshots.Reserve(NativeBankSnapshots::MAX_PAGES));
    for (size_t i = 0; i < NativeBankSnapshots::MAX_PAGES; ++i) {
        snapshots.Record(i * PAGE, 1, 0, 16, guest.data());
    }
    REQUIRE_FALSE(snapshots.Reserve(0));
    REQUIRE(snapshots.Reserve(1));
    REQUIRE(snapshots.Size() == 0);
    REQUIRE_FALSE(snapshots.Unchanged(0, 1, 0, 16, guest.data()));
}
