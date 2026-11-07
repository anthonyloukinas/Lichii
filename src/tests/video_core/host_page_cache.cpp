// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/host_page_cache.h"

namespace {

using VideoCommon::HostPageCache;

constexpr GPUVAddr BASE = 0x4000'0000;

/// GPU pages backed by a host buffer each; `mapped` false makes every lookup fail.
struct FakePages {
    std::array<std::array<u8, 0x1000>, 32> host{};
    bool mapped = true;
    std::vector<GPUVAddr> lookups;

    const u8* Lookup(GPUVAddr page) {
        lookups.push_back(page);
        if (!mapped || page < BASE || page >= BASE + host.size() * 0x1000) {
            return nullptr;
        }
        return host[(page - BASE) >> 12].data();
    }
};

} // Anonymous namespace

TEST_CASE("HostPageCache looks a page up once and returns the offset within it",
          "[host_page_cache][memory_manager]") {
    FakePages pages;
    HostPageCache cache;
    const auto lookup = [&](GPUVAddr page) { return pages.Lookup(page); };
    bool hit = true;
    REQUIRE(cache.Get(7, 1, 1, BASE + 0x2010, lookup, hit) == pages.host[2].data() + 0x10);
    REQUIRE_FALSE(hit);
    REQUIRE(cache.Get(7, 1, 1, BASE + 0x2ffc, lookup, hit) == pages.host[2].data() + 0xffc);
    REQUIRE(hit);
    REQUIRE(cache.Get(7, 1, 1, BASE + 0x2000, lookup, hit) == pages.host[2].data());
    REQUIRE(hit);
    REQUIRE(pages.lookups == std::vector<GPUVAddr>{BASE + 0x2000});
}

TEST_CASE("HostPageCache drops its pointers when the serials or the owner change",
          "[host_page_cache][memory_manager]") {
    FakePages pages;
    HostPageCache cache;
    const auto lookup = [&](GPUVAddr page) { return pages.Lookup(page); };
    bool hit{};
    (void)cache.Get(7, 1, 1, BASE + 0x1000, lookup, hit);
    (void)cache.Get(7, 1, 1, BASE + 0x1004, lookup, hit);
    REQUIRE(hit);
    // A GPU map or unmap (MemoryManager map_serial).
    (void)cache.Get(7, 2, 1, BASE + 0x1004, lookup, hit);
    REQUIRE_FALSE(hit);
    // A device memory Map or Unmap (translation generation).
    (void)cache.Get(7, 2, 2, BASE + 0x1004, lookup, hit);
    REQUIRE_FALSE(hit);
    // Another manager, even with the same serials.
    (void)cache.Get(8, 2, 2, BASE + 0x1004, lookup, hit);
    REQUIRE_FALSE(hit);
    (void)cache.Get(8, 2, 2, BASE + 0x1008, lookup, hit);
    REQUIRE(hit);
    REQUIRE(pages.lookups.size() == 4);
    // Owner 0 with zero serials is not mistaken for the empty cache.
    HostPageCache fresh;
    (void)fresh.Get(0, 0, 0, BASE + 0x1000, lookup, hit);
    REQUIRE_FALSE(hit);
    (void)fresh.Get(0, 0, 0, BASE + 0x1000, lookup, hit);
    REQUIRE(hit);
}

TEST_CASE("HostPageCache keeps no unmapped pages and replaces colliding slots",
          "[host_page_cache][memory_manager]") {
    FakePages pages;
    HostPageCache cache;
    const auto lookup = [&](GPUVAddr page) { return pages.Lookup(page); };
    bool hit{};
    pages.mapped = false;
    REQUIRE(cache.Get(1, 1, 1, BASE, lookup, hit) == nullptr);
    pages.mapped = true;
    REQUIRE(cache.Get(1, 1, 1, BASE + 4, lookup, hit) == pages.host[0].data() + 4);
    REQUIRE_FALSE(hit);
    // Pages 16 apart share a slot: each lookup replaces the other.
    const GPUVAddr other = BASE + HostPageCache::Slots * 0x1000;
    REQUIRE(cache.Get(1, 1, 1, other, lookup, hit) == pages.host[HostPageCache::Slots].data());
    REQUIRE_FALSE(hit);
    REQUIRE(cache.Get(1, 1, 1, BASE + 8, lookup, hit) == pages.host[0].data() + 8);
    REQUIRE_FALSE(hit);
    // The other 15 slots hold their pages meanwhile.
    for (u64 page = 1; page < HostPageCache::Slots; ++page) {
        (void)cache.Get(1, 1, 1, BASE + page * 0x1000, lookup, hit);
    }
    for (u64 page = 0; page < HostPageCache::Slots; ++page) {
        REQUIRE(cache.Get(1, 1, 1, BASE + page * 0x1000 + 12, lookup, hit) ==
                pages.host[page].data() + 12);
        REQUIRE(hit);
    }
    REQUIRE(pages.lookups.size() == 4 + HostPageCache::Slots - 1);
}
