// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <iterator>
#include <map>
#include <utility>
#include <vector>

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

/// Random allocations, fixed mappings and frees, checked against the live ranges: an allocation
/// stays inside the address space and off every live range, and only fails when nothing fits.
template <typename Va, size_t Bits>
void CheckAtRandom(u64 start, u64 limit, u64 unit) {
    Common::FlatAllocator<Va, 0, Bits> allocator{static_cast<Va>(start), static_cast<Va>(limit)};
    std::map<u64, u64> live;
    u64 state{0x9E3779B97F4A7C15ULL};
    const auto next{[&state] {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    }};
    const auto largest_gap{[&] {
        u64 best{}, cursor{start};
        for (const auto& [address, size] : live) {
            best = std::max(best, address - cursor);
            cursor = address + size;
        }
        return std::max(best, limit - cursor);
    }};
    // A free range picked at random, or {0, 0} when everything is taken
    const auto free_range{[&]() -> std::pair<u64, u64> {
        std::vector<std::pair<u64, u64>> gaps;
        u64 cursor{start};
        for (const auto& [address, size] : live) {
            if (address > cursor) {
                gaps.emplace_back(cursor, address - cursor);
            }
            cursor = address + size;
        }
        if (limit > cursor) {
            gaps.emplace_back(cursor, limit - cursor);
        }
        if (gaps.empty()) {
            return {};
        }
        const auto [address, size]{gaps[next() % gaps.size()]};
        const u64 units{size / unit};
        const u64 skipped{next() % units};
        return {address + skipped * unit, (1 + next() % std::min<u64>(units - skipped, 8)) * unit};
    }};
    for (int op = 0; op < 20000; ++op) {
        if (const auto [fixed_address, fixed_size]{free_range()};
            fixed_size != 0 && next() % 100 < 10) {
            allocator.AllocateFixed(static_cast<Va>(fixed_address), static_cast<Va>(fixed_size));
            live.emplace(fixed_address, fixed_size);
        } else if (live.empty() || next() % 100 < 50) {
            const u64 size{(1 + next() % 8) * unit};
            const u64 address{allocator.Allocate(static_cast<Va>(size))};
            if (address == 0) {
                REQUIRE(largest_gap() < size);
                continue;
            }
            REQUIRE(address >= start);
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
            allocator.Free(static_cast<Va>(victim->first), static_cast<Va>(victim->second));
            live.erase(victim);
        }
    }
}

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
    CheckAtRandom<u64, 36>(Start, Start + 256 * Page, Page);
    // In units of pages, as the GPU address spaces use it
    CheckAtRandom<u32, 32>(0x10, 0x110, 1);
    CheckAtRandom<u32, 32>(0xFFFFFE00, 0xFFFFFFFF, 1);
}

TEST_CASE("FlatAllocator: a failed allocation works after a free", "[common]") {
    Full full;
    REQUIRE(full.allocator.Allocate(Page) == 0);
    full.allocator.Free(full.b, Page);
    REQUIRE(full.allocator.Allocate(2 * Page) == 0);
    REQUIRE(full.allocator.Allocate(Page) == full.b);
}

TEST_CASE("FlatAllocator: the search goes on after an allocation from a gap", "[common]") {
    Allocator allocator{1, 11};
    REQUIRE(allocator.Allocate(4) == 1);
    allocator.Free(1, 4);
    // Six units are left after the first allocation, so this one wraps
    REQUIRE(allocator.Allocate(7) == 1);
    REQUIRE(allocator.Allocate(1) == 8);
    REQUIRE(allocator.Allocate(2) == 9);
    REQUIRE(allocator.Allocate(1) == 0);
}

TEST_CASE("FlatAllocator: the search goes on inside a span that was freed around it", "[common]") {
    Allocator allocator{1, 11};
    REQUIRE(allocator.Allocate(3) == 1);
    REQUIRE(allocator.Allocate(3) == 4);
    allocator.Free(4, 3);
    allocator.Free(1, 3);
    REQUIRE(allocator.Allocate(2) == 7);
    REQUIRE(allocator.Allocate(2) == 9);
    REQUIRE(allocator.Allocate(6) == 1);
    REQUIRE(allocator.Allocate(1) == 0);
}

TEST_CASE("FlatAllocator: parts of allocations can be freed", "[common]") {
    Allocator allocator{1, 9};
    REQUIRE(allocator.Allocate(4) == 1);
    REQUIRE(allocator.Allocate(4) == 5);
    // The end of the first allocation and the start of the second
    allocator.Free(4, 2);
    REQUIRE(allocator.Allocate(3) == 0);
    REQUIRE(allocator.Allocate(2) == 4);
    // The middle of the first allocation
    allocator.Free(2, 1);
    REQUIRE(allocator.Allocate(1) == 2);
    REQUIRE(allocator.Allocate(1) == 0);
}

TEST_CASE("FlatAllocator: a fixed mapping where the search starts is skipped", "[common]") {
    Allocator allocator{1, 16};
    REQUIRE(allocator.Allocate(2) == 1);
    allocator.AllocateFixed(3, 2);
    REQUIRE(allocator.Allocate(1) == 5);
}

TEST_CASE("FlatAllocator: an allocation does not run into a fixed mapping", "[common]") {
    Allocator allocator{1, 9};
    REQUIRE(allocator.Allocate(2) == 1);
    allocator.AllocateFixed(4, 1);
    // One unit is free in front of the fixed mapping
    REQUIRE(allocator.Allocate(2) == 5);
    REQUIRE(allocator.Allocate(2) == 7);
    REQUIRE(allocator.Allocate(2) == 0);
    REQUIRE(allocator.Allocate(1) == 3);
    REQUIRE(allocator.Allocate(1) == 0);
}

TEST_CASE("FlatAllocator: a gap between fixed mappings is only used when it fits", "[common]") {
    Allocator allocator{1, 12};
    REQUIRE(allocator.Allocate(2) == 1);
    allocator.AllocateFixed(5, 1);
    allocator.AllocateFixed(6, 1);
    allocator.Free(5, 1);
    REQUIRE(allocator.Allocate(3) == 3);
    REQUIRE(allocator.Allocate(2) == 7);
    REQUIRE(allocator.Allocate(4) == 0);
    REQUIRE(allocator.Allocate(3) == 9);
}

TEST_CASE("FlatAllocator: an allocation after a wrap does not land on a fixed mapping",
          "[common]") {
    Allocator allocator{1, 16};
    allocator.AllocateFixed(10, 1);
    REQUIRE(allocator.Allocate(8) == 1);
    allocator.Free(1, 8);
    // Neither side of the fixed mapping has nine units from here on, so this one wraps
    REQUIRE(allocator.Allocate(9) == 1);
    REQUIRE(allocator.Allocate(1) == 11);
    REQUIRE(allocator.Allocate(4) == 12);
    REQUIRE(allocator.Allocate(1) == 0);
}

TEST_CASE("FlatAllocator: a 32-bit address space is usable up to its limit", "[common]") {
    constexpr u32 start{0xFFFFFFF0};
    Common::FlatAllocator<u32, 0, 32> allocator{start};
    // The limit is the last address, so fifteen units are left
    REQUIRE(allocator.Allocate(16) == 0);
    REQUIRE(allocator.Allocate(0xFFFFFFFF) == 0);
    REQUIRE(allocator.Allocate(8) == start);
    REQUIRE(allocator.Allocate(8) == 0);
    REQUIRE(allocator.Allocate(7) == start + 8);
    REQUIRE(allocator.Allocate(1) == 0);
    allocator.Free(start, 8);
    allocator.AllocateFixed(start + 2, 2);
    REQUIRE(allocator.Allocate(4) == start + 4);
    REQUIRE(allocator.Allocate(3) == 0);
    REQUIRE(allocator.Allocate(2) == start);
    REQUIRE(allocator.Allocate(1) == 0);
}

TEST_CASE("FlatAllocator: the start of a mapped block can be freed after a free range",
          "[common]") {
    Allocator allocator{1, 5};
    REQUIRE(allocator.Allocate(1) == 1);
    REQUIRE(allocator.Allocate(2) == 2);
    // Frees the first allocation and half of the second; its other half stays mapped
    allocator.Free(1, 2);
    REQUIRE(allocator.Allocate(1) == 4);
    REQUIRE(allocator.Allocate(2) == 1);
    REQUIRE(allocator.Allocate(1) == 0);
}

TEST_CASE("FlatAllocator: a free can span mapped and free ranges", "[common]") {
    Allocator allocator{1, 11};
    for (u64 address = 1; address < 11; address += 2) {
        REQUIRE(allocator.Allocate(2) == address);
    }
    allocator.Free(3, 2);
    allocator.Free(7, 2);
    // From inside the first allocation to inside the last one
    allocator.Free(2, 8);
    REQUIRE(allocator.Allocate(9) == 0);
    REQUIRE(allocator.Allocate(8) == 2);
    REQUIRE(allocator.Allocate(1) == 0);
    allocator.Free(1, 10);
    REQUIRE(allocator.Allocate(10) == 1);
}
