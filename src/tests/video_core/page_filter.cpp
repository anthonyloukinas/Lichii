// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/page_filter.h"

namespace {

using VideoCommon::PageFilter;

constexpr u64 Page = 0x1000;
constexpr u64 Limit = u64{1} << 36; // The filter's device address space

} // Anonymous namespace

TEST_CASE("PageFilter marks whole pages", "[video_core][page_filter]") {
    PageFilter filter;
    REQUIRE_FALSE(filter.Any(0, Limit));
    filter.Mark(5 * Page + 0x10, 1);
    REQUIRE(filter.Any(5 * Page, 1));
    REQUIRE(filter.Any(6 * Page - 1, 1));
    REQUIRE_FALSE(filter.Any(4 * Page, Page));
    REQUIRE(filter.Any(4 * Page, Page + 1));
    REQUIRE_FALSE(filter.Any(6 * Page, Page));
    REQUIRE(filter.Any(0, Limit));
}

TEST_CASE("PageFilter ignores empty ranges", "[video_core][page_filter]") {
    PageFilter filter;
    filter.Mark(3 * Page, 0);
    REQUIRE_FALSE(filter.Any(3 * Page, Page));
    filter.Mark(3 * Page, 1);
    REQUIRE_FALSE(filter.Any(3 * Page, 0));
}

TEST_CASE("PageFilter ranges cross 64-page words", "[video_core][page_filter]") {
    PageFilter filter;
    // Pages 63 and 64: the last bit of word 0 and the first of word 1.
    filter.Mark(63 * Page + Page / 2, Page);
    REQUIRE_FALSE(filter.Any(62 * Page, Page));
    REQUIRE(filter.Any(63 * Page, 1));
    REQUIRE(filter.Any(64 * Page, 1));
    REQUIRE_FALSE(filter.Any(65 * Page, Page));
    // Pages 130 to 300: a partial word, whole words and a partial word again.
    filter.Mark(130 * Page, 171 * Page);
    REQUIRE_FALSE(filter.Any(65 * Page, 65 * Page));
    for (const u64 page : {130u, 191u, 192u, 255u, 256u, 300u}) {
        CAPTURE(page);
        REQUIRE(filter.Any(page * Page, 1));
    }
    REQUIRE_FALSE(filter.Any(301 * Page, 1000 * Page));
    // A query over unmarked pages that ends on a marked one.
    REQUIRE(filter.Any(66 * Page, 64 * Page + 1));
    REQUIRE_FALSE(filter.Any(66 * Page, 64 * Page));
}

TEST_CASE("PageFilter leaves ranges past its space untracked", "[video_core][page_filter]") {
    PageFilter filter;
    // Ranges reaching past the space may hold anything.
    REQUIRE(filter.Any(Limit, 1));
    REQUIRE(filter.Any(Limit - Page, 2 * Page));
    REQUIRE_FALSE(filter.Any(Limit - Page, Page));
    // A stand-in address of an image without CPU backing (TextureCache::ImageCpuAddress) is
    // ignored, and doesn't make the rest of the space report objects.
    filter.Mark(~(u64{1} << 40) + 0xf0000, 0x6b600);
    filter.Mark(Limit + Page, Page);
    REQUIRE_FALSE(filter.Any(0, Limit));
    REQUIRE(filter.Any(Limit + Page, 1));
    filter.Mark(Limit - Page, Page);
    REQUIRE(filter.Any(Limit - 1, 1));
    REQUIRE_FALSE(filter.Any(0, Limit - Page));
}
