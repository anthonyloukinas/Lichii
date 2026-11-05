// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/split_write_log.h"

using VideoCommon::SplitWriteLog;

TEST_CASE("A read waits only for the logged writes it overlaps", "[video_core][split_write_log]") {
    SplitWriteLog log;
    log.Note(0x1000, 0x100, 10);
    log.Note(0x8000, 0x10, 20);
    log.Note(0x3000, 0x40, 30);
    REQUIRE(log.NewestPosition() == 30);
    // Disjoint: the floor (unknown writers) stays the target.
    REQUIRE(log.Target(0x5000, 0x18, 4, 0) == 4);
    // The newest overlapping write sets it, older or newer ones elsewhere don't.
    bool overlap = false;
    REQUIRE(log.Target(0x8000, 4, 4, 0, &overlap) == 20);
    REQUIRE(overlap);
    REQUIRE(log.Target(0x10fc, 8, 4, 0) == 10);
    // Ends are exclusive.
    REQUIRE(log.Target(0x1100, 4, 4, 0) == 4);
    REQUIRE(log.Target(0x0ffc, 4, 4, 0) == 4);
    // A higher floor wins; writes the back already ran don't count.
    REQUIRE(log.Target(0x1000, 4, 25, 0, &overlap) == 25);
    REQUIRE_FALSE(overlap);
    REQUIRE(log.Target(0x1000, 4, 4, 10) == 4);
    REQUIRE(log.Target(0x8000, 4, 4, 10) == 20);
}

TEST_CASE("Contiguous uploads join one range at the latest position",
          "[video_core][split_write_log]") {
    SplitWriteLog log;
    for (u64 i = 0; i < 64; ++i) {
        log.Note(0x10000 + i * 4, 4, 100 + i);
    }
    REQUIRE(log.Target(0x10000, 4, 0, 0) == 163);
    REQUIRE(log.Target(0x10000 + 63 * 4, 4, 0, 0) == 163);
    REQUIRE(log.Target(0x10000 + 64 * 4, 4, 0, 0) == 0);
    // Joined ranges stop growing at MaxMerged: a long stream doesn't cover everything after it.
    SplitWriteLog stream;
    for (u64 i = 0; i < SplitWriteLog::MaxMerged / 0x100 * 3; ++i) {
        stream.Note(0x100000 + i * 0x100, 0x100, 1 + i);
    }
    const u64 last = SplitWriteLog::MaxMerged / 0x100 * 3;
    REQUIRE(stream.Target(0x100000, 4, 0, 0) < last);
    REQUIRE(stream.Target(0x100000 + (last - 1) * 0x100, 4, 0, 0) == last);
}

TEST_CASE("Writes that fell out of the log bound every read", "[video_core][split_write_log]") {
    SplitWriteLog log;
    for (u64 i = 0; i < SplitWriteLog::Capacity + 8; ++i) {
        // Not contiguous: each write takes an entry.
        log.Note(0x100000 + i * 0x1000, 0x10, 1 + i);
    }
    bool overlap = false;
    // The 8 oldest fell out (positions 1-8): a read anywhere waits for them...
    REQUIRE(log.Target(0x1, 4, 0, 0, &overlap) == 8);
    REQUIRE(overlap);
    // ...until the back ran them.
    REQUIRE(log.Target(0x1, 4, 0, 8, &overlap) == 0);
    REQUIRE_FALSE(overlap);
    // Kept entries still count.
    REQUIRE(log.Target(0x100000 + (SplitWriteLog::Capacity + 7) * 0x1000, 4, 0, 0) ==
            SplitWriteLog::Capacity + 8);
    log.Reset();
    REQUIRE(log.Target(0x1, 4, 0, 0) == 0);
    REQUIRE(log.NewestPosition() == 0);
}
