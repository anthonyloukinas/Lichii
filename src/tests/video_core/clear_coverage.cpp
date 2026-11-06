// SPDX-License-Identifier: GPL-3.0-or-later

#include <memory>

#include <catch2/catch_test_macros.hpp>

#include "video_core/texture_cache/clear_coverage.h"

namespace {
using Regs = Tegra::Engines::Maxwell3D::Regs;

std::unique_ptr<Regs> MakeRegs(u32 clip_width, u32 clip_height) {
    auto regs = std::make_unique<Regs>();
    regs->surface_clip.width.Assign(clip_width);
    regs->surface_clip.height.Assign(clip_height);
    return regs;
}

void SetScissor(Regs& regs, u32 min_x, u32 max_x, u32 min_y, u32 max_y) {
    regs.clear_control.use_scissor.Assign(1);
    auto& scissor = regs.scissor_test[0];
    scissor.enable = 1;
    scissor.min_x.Assign(min_x);
    scissor.max_x.Assign(max_x);
    scissor.min_y.Assign(min_y);
    scissor.max_y.Assign(max_y);
}

// The check before #311: raw scissor only, every clear full with scissoring off.
bool OldClearCovers(const Regs& regs, u32 width, u32 height) {
    if (regs.clear_control.use_scissor == 0) {
        return true;
    }
    const auto& scissor = regs.scissor_test[0];
    return scissor.min_x == 0 && scissor.min_y == 0 && scissor.max_x >= width &&
           scissor.max_y >= height;
}
} // namespace

TEST_CASE("Clear coverage stops at the surface clip", "[video_core][clear_coverage]") {
    const auto regs = MakeRegs(64, 64);
    REQUIRE(OldClearCovers(*regs, 128, 128));
    REQUIRE_FALSE(VideoCommon::ClearCoversExtent(*regs, 128, 128));
    REQUIRE(VideoCommon::ClearCoversExtent(*regs, 64, 64));
}

TEST_CASE("Clear coverage flips the scissor for a lower-left origin",
          "[video_core][clear_coverage]") {
    const auto regs = MakeRegs(128, 256);
    regs->window_origin.mode.Assign(Regs::WindowOrigin::Mode::LowerLeft);
    SetScissor(*regs, 0, 128, 0, 128);
    // The clear writes rows [128, 256), outside a 128-high target.
    REQUIRE(OldClearCovers(*regs, 128, 128));
    REQUIRE_FALSE(VideoCommon::ClearCoversExtent(*regs, 128, 128));

    SetScissor(*regs, 0, 128, 128, 256);
    REQUIRE(VideoCommon::ClearCoversExtent(*regs, 128, 128));
}

TEST_CASE("Clear coverage keeps full clears full", "[video_core][clear_coverage]") {
    const auto regs = MakeRegs(128, 128);
    REQUIRE(VideoCommon::ClearCoversExtent(*regs, 128, 128));
    SetScissor(*regs, 0, 128, 0, 128);
    REQUIRE(VideoCommon::ClearCoversExtent(*regs, 128, 128));
    SetScissor(*regs, 1, 128, 0, 128);
    REQUIRE_FALSE(VideoCommon::ClearCoversExtent(*regs, 128, 128));
}
