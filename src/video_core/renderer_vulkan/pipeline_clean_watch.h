// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <span>
#include <vector>

#include "common/common_types.h"
#include "video_core/renderer_vulkan/fixed_pipeline_state.h"

namespace Vulkan {

/// Register words [first, first + count) of Maxwell3D::Regs::reg_array.
struct CleanWatchRange {
    u32 first;
    u32 count;
};

struct CleanWatch {
    std::vector<CleanWatchRange> ranges;     ///< Read by every Refresh, sorted and merged
    std::vector<CleanWatchRange> xfb_ranges; ///< Read while transform feedback is enabled
};

/// The registers FixedPipelineState::Refresh (and what it calls) reads with these features.
[[nodiscard]] CleanWatch BuildCleanWatch(const Maxwell& regs, const DynamicFeatures& features);

/// Copies the words of `ranges` from `words` (a reg_array) to `saved`, one range after another.
void SaveCleanWords(const u32* words, std::span<const CleanWatchRange> ranges, u32* saved);

/// Whether the words of `ranges` in `words` equal those SaveCleanWords put in `saved`.
[[nodiscard]] bool CleanWordsEqual(const u32* words, std::span<const CleanWatchRange> ranges,
                                   const u32* saved);

} // namespace Vulkan
