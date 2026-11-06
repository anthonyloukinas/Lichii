// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <span>
#include <catch2/catch_test_macros.hpp>

#include "video_core/texture_cache/feedback_loop_cache.h"

namespace {
using VideoCommon::FeedbackLoopCache;
using VideoCommon::ImageViewId;

constexpr ImageViewId View(u32 index) {
    return ImageViewId{index};
}
} // namespace

TEST_CASE("Feedback loop cache answers only for the key it was worked out for",
          "[texture_cache][feedback_loop]") {
    FeedbackLoopCache cache;
    const std::array views{View(3), View(7)};
    CHECK_FALSE(cache.Hit(0, 0, 0, {}, true));
    CHECK_FALSE(cache.Hit(0, 0, 0, {}, false));

    cache.Store(5, 9, 2, views, true);
    CHECK(cache.Hit(5, 9, 2, views, true));
    CHECK(cache.Answer());
    // Render target or texture table serial changes.
    CHECK_FALSE(cache.Hit(6, 9, 2, views, true));
    CHECK_FALSE(cache.Hit(5, 10, 2, views, true));
    // Another pipeline or handle set at the same serials: other views, order, count, none.
    const std::array other{View(3), View(8)};
    const std::array swapped{View(7), View(3)};
    const std::array fewer{View(3)};
    CHECK_FALSE(cache.Hit(5, 9, 2, other, true));
    CHECK_FALSE(cache.Hit(5, 9, 2, swapped, true));
    CHECK_FALSE(cache.Hit(5, 9, 2, fewer, true));
    CHECK_FALSE(cache.Hit(5, 9, 2, {}, true));
    // Serials only (EDEN_NX2_FEEDBACK_VIEWS=0) ignores the views, as before.
    CHECK(cache.Hit(5, 9, 2, other, false));

    cache.Store(5, 9, 2, {}, false);
    CHECK(cache.Hit(5, 9, 2, {}, true));
    CHECK_FALSE(cache.Answer());
    CHECK_FALSE(cache.Hit(5, 9, 2, views, true));
}

TEST_CASE("Feedback loop cache misses after the views were retired and the slot reused",
          "[texture_cache][feedback_loop]") {
    // A full draw samples view V of an unrelated image A: no feedback loop. A is deleted, which
    // retires V (the texture cache advances the view lifetime epoch), and a sampling view of the
    // still active depth image takes the same slot V. Render target serial, table serial and the
    // view list [V] are all unchanged, but the answer must be worked out again.
    FeedbackLoopCache cache;
    const std::array views{View(11)};
    u64 epoch = 4;
    cache.Store(1, 1, epoch, views, false);
    REQUIRE(cache.Hit(1, 1, epoch, views, true));
    ++epoch; // DeleteImage or InvalidateScale
    CHECK_FALSE(cache.Hit(1, 1, epoch, views, true));
    // An empty view list is retired the same way.
    cache.Store(1, 1, epoch, {}, false);
    ++epoch;
    CHECK_FALSE(cache.Hit(1, 1, epoch, {}, true));
}
