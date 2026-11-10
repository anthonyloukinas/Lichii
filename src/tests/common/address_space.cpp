// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <iterator>
#include <map>

#include <catch2/catch_test_macros.hpp>

#include "common/address_space.h"
#include "common/address_space.inc"
#include "common/common_types.h"

namespace {

constexpr u64 Page = 0x1000;
constexpr u64 Start = Page;
/// Room for four pages after Start
constexpr u64 Limit = Start + 4 * Page;

using Allocator = Common::FlatAllocator<u64, 0, 36>;

/// Fills the four pages, which ends the allocator's linear pass.
struct Full {
    Allocator allocator{Start, Limit};
    u64 a{allocator.Allocate(Page)};
    u64 b{allocator.Allocate(Page)};
    u64 c{allocator.Allocate(Page)};
    u64 d{allocator.Allocate(Page)};
};

} // namespace

TEST_CASE("FlatAllocator: the linear pass fills the address space", "[common]") {
    Full full;
    REQUIRE(full.a == Start);
    REQUIRE(full.b == Start + Page);
    REQUIRE(full.c == Start + 2 * Page);
    REQUIRE(full.d == Start + 3 * Page);
    REQUIRE(full.allocator.Allocate(Page) == 0);
}

TEST_CASE("FlatAllocator: a range between a free and a mapped neighbour is freed", "[common]") {
    Full full;
    full.allocator.Free(full.b, Page);
    // The block before c is free now and d is still mapped
    full.allocator.Free(full.c, Page);
    REQUIRE(full.allocator.Allocate(2 * Page) == full.b);
    REQUIRE(full.allocator.Allocate(Page) == 0);
}

TEST_CASE("FlatAllocator: the span before the first allocation is found", "[common]") {
    Full full;
    full.allocator.Free(full.a, Page);
    REQUIRE(full.allocator.Allocate(Page) == full.a);
}

TEST_CASE("FlatAllocator: the span after the last allocation is found", "[common]") {
    Full full;
    full.allocator.Free(full.d, Page);
    full.allocator.Free(full.c, Page);
    REQUIRE(full.allocator.Allocate(2 * Page) == full.c);
    REQUIRE(full.allocator.Allocate(Page) == 0);
}

TEST_CASE("FlatAllocator: an emptied address space is usable again", "[common]") {
    Full full;
    full.allocator.Free(full.d, Page);
    full.allocator.Free(full.c, Page);
    full.allocator.Free(full.b, Page);
    full.allocator.Free(full.a, Page);
    REQUIRE(full.allocator.Allocate(4 * Page) == Start);
    REQUIRE(full.allocator.Allocate(Page) == 0);
}

TEST_CASE("FlatAllocator: a freed range is not handed out again straight away", "[common]") {
    Full full;
    full.allocator.Free(full.a, Page);
    full.allocator.Free(full.c, Page);
    const u64 first{full.allocator.Allocate(Page)};
    REQUIRE(first == full.a);
    full.allocator.Free(first, Page);
    // The search goes on after `first`, then wraps
    REQUIRE(full.allocator.Allocate(Page) == full.c);
    REQUIRE(full.allocator.Allocate(Page) == full.a);
    REQUIRE(full.allocator.Allocate(Page) == 0);
}

TEST_CASE("FlatAllocator: allocations never overlap and space is only lost to fragmentation",
          "[common]") {
    constexpr u64 limit{Start + 256 * Page};
    Allocator allocator{Start, limit};
    std::map<u64, u64> live;
    u64 state{0x9E3779B97F4A7C15ULL};
    const auto next{[&state] {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    }};
    const auto largest_gap{[&] {
        u64 best{}, cursor{Start};
        for (const auto& [address, size] : live) {
            best = std::max(best, address - cursor);
            cursor = address + size;
        }
        return std::max(best, limit - cursor);
    }};
    for (int op = 0; op < 20000; ++op) {
        if (live.empty() || next() % 100 < 55) {
            const u64 size{(1 + next() % 8) * Page};
            const u64 address{allocator.Allocate(size)};
            if (address == 0) {
                REQUIRE(largest_gap() < size);
                continue;
            }
            REQUIRE(address >= Start);
            REQUIRE(address + size <= limit);
            auto after{live.lower_bound(address)};
            REQUIRE((after == live.end() || after->first >= address + size));
            if (after != live.begin()) {
                const auto before{std::prev(after)};
                REQUIRE(before->first + before->second <= address);
            }
            live.emplace(address, size);
        } else {
            auto victim{live.begin()};
            std::advance(victim, next() % live.size());
            allocator.Free(victim->first, victim->second);
            live.erase(victim);
        }
    }
}
