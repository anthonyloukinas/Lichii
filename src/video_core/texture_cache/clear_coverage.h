// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"
#include "video_core/engines/maxwell_3d.h"

namespace VideoCommon {

/// Returns true if the current clear's render area and scissor cover a width x height image
/// (upstream #311).
[[nodiscard]] inline bool ClearCoversExtent(const Tegra::Engines::Maxwell3D::Regs& regs,
                                            u32 width, u32 height) {
    // The render area is the surface clip's size.
    if (regs.surface_clip.width < width || regs.surface_clip.height < height) {
        return false;
    }
    if (regs.clear_control.use_scissor == 0) {
        return true;
    }
    // With a lower-left window origin, the clear's scissor is flipped around the surface clip's
    // height (GetScissorState).
    const auto& scissor = regs.scissor_test[0];
    const bool lower_left =
        regs.window_origin.mode != Tegra::Engines::Maxwell3D::Regs::WindowOrigin::Mode::UpperLeft;
    const s32 clip_height = static_cast<s32>(regs.surface_clip.height.Value());
    const s32 min_y = static_cast<s32>(scissor.min_y.Value());
    const s32 max_y = static_cast<s32>(scissor.max_y.Value());
    const s32 top = lower_left ? clip_height - max_y : min_y;
    const s32 bottom = lower_left ? clip_height - min_y : max_y;
    return scissor.min_x == 0 && top <= 0 && scissor.max_x >= width &&
           bottom >= static_cast<s32>(height);
}

} // namespace VideoCommon
