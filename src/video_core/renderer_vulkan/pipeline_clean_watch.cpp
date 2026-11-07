// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstring>
#include <utility>

#include "video_core/renderer_vulkan/pipeline_clean_watch.h"

namespace Vulkan {

CleanWatch BuildCleanWatch(const Maxwell& regs, const DynamicFeatures& features) {
    using Range = CleanWatchRange;
    const auto watch_into = [&regs](std::vector<Range>& ranges, const auto& field) {
        const auto offset = static_cast<size_t>(reinterpret_cast<const char*>(&field) -
                                                reinterpret_cast<const char*>(&regs));
        const size_t first = offset / sizeof(u32);
        const size_t last = (offset + sizeof(field) + sizeof(u32) - 1) / sizeof(u32);
        ranges.push_back({static_cast<u32>(first), static_cast<u32>(last - first)});
    };
    std::vector<Range> ranges;
    const auto watch = [&](const auto& field) { watch_into(ranges, field); };

    // Refresh itself, always:
    watch(regs.transform_feedback_enabled);
    watch(regs.depth_mode);
    watch(regs.polygon_mode_front);
    watch(regs.tessellation);
    watch(regs.patch_vertices);
    watch(regs.anti_alias_samples_mode);
    watch(regs.blend);                    // ComputeAttachment0DualSourceBlend
    watch(regs.blend_per_target_enabled); // ComputeAttachment0DualSourceBlend
    watch(regs.blend_per_target[0]);      // ComputeAttachment0DualSourceBlend
    watch(regs.alpha_test_enabled);
    watch(regs.alpha_test_func);
    watch(regs.alpha_test_ref);
    watch(regs.mandated_early_z);
    watch(regs.zeta_enable);
    watch(regs.zeta.format);
    watch(regs.window_origin);
    watch(regs.provoking_vertex);
    watch(regs.conservative_raster_enable);
    watch(regs.line_anti_alias_enable);
    watch(regs.anti_alias_alpha_control);
    watch(regs.depth_bounds);
    watch(regs.line_stipple_params);
    for (const auto& rt : regs.rt) {
        watch(rt.format);
    }
    watch(regs.point_size);
    // Dirty::VertexInput (both branches) and the static vertex repack:
    watch(regs.vertex_attrib_format);
    const bool repack = !features.has_dynamic_vertex_input && features.has_static_vertex_repack;
    if (!features.has_dynamic_vertex_input) {
        watch(regs.vertex_stream_instances);
        for (const auto& stream : regs.vertex_streams) {
            watch(stream.frequency);
        }
    }
    if (!features.has_extended_dynamic_state || repack) {
        // Strides (DynamicState::Refresh's vertex_strides, the repack): the first word of each
        // stream, VertexStream's leading union of stride and enable.
        for (const auto& stream : regs.vertex_streams) {
            const auto offset = static_cast<size_t>(reinterpret_cast<const char*>(&stream) -
                                                    reinterpret_cast<const char*>(&regs));
            ranges.push_back({static_cast<u32>(offset / sizeof(u32)), 1});
        }
    }
    // Dirty::ViewportSwizzles:
    for (const auto& viewport : regs.viewport_transform) {
        watch(viewport.swizzle);
    }
    // DynamicState::Refresh (no extended dynamic state):
    if (!features.has_extended_dynamic_state) {
        watch(regs.gl_front_face);
        watch(regs.stencil_front_op);
        watch(regs.stencil_two_side_enable);
        watch(regs.stencil_back_op);
        watch(regs.stencil_enable);
        watch(regs.depth_write_enabled);
        watch(regs.depth_bounds_enable);
        watch(regs.depth_test_enable);
        watch(regs.depth_test_func);
        watch(regs.gl_cull_face);
        watch(regs.gl_cull_test_enabled);
    }
    // DynamicState::Refresh2:
    if (!features.has_extended_dynamic_state_2_logic_op) {
        watch(regs.logic_op);
        if (!features.has_extended_dynamic_state_2) {
            watch(regs.polygon_offset_point_enable);
            watch(regs.polygon_offset_line_enable);
            watch(regs.polygon_offset_fill_enable);
            watch(regs.rasterize_enable);
            watch(regs.primitive_restart);
        }
    }
    // Dirty::Blending, BlendingAttachment::Refresh (no extended dynamic state 3 blending):
    if (!features.has_extended_dynamic_state_3_blend) {
        watch(regs.color_mask_common);
        watch(regs.color_mask);
        watch(regs.blend_per_target);
        watch(regs.iterated_blend);
    }
    // DynamicState::Refresh3:
    if (!features.has_dynamic_state3_logic_op_enable) {
        watch(regs.logic_op);
    }
    if (!features.has_dynamic_state3_depth_clamp_enable) {
        watch(regs.viewport_clip_control);
    }
    if (!features.has_dynamic_state3_line_stipple_enable) {
        watch(regs.line_stipple_enable);
    }

    // Sorted and merged: contiguous words compare as one block.
    const auto merge = [](std::vector<Range> input) {
        std::ranges::sort(input, {}, &Range::first);
        std::vector<Range> merged;
        for (const Range& range : input) {
            if (!merged.empty() && range.first <= merged.back().first + merged.back().count) {
                const u32 end = (std::max)(merged.back().first + merged.back().count,
                                           range.first + range.count);
                merged.back().count = end - merged.back().first;
            } else {
                merged.push_back(range);
            }
        }
        return merged;
    };
    CleanWatch result;
    result.ranges = merge(std::move(ranges));
    // RefreshXfbState, while transform_feedback_enabled (watched above) is set:
    std::vector<Range> xfb_ranges;
    watch_into(xfb_ranges, regs.transform_feedback.controls);
    watch_into(xfb_ranges, regs.stream_out_layout);
    result.xfb_ranges = merge(std::move(xfb_ranges));
    return result;
}

void SaveCleanWords(const u32* words, std::span<const CleanWatchRange> ranges, u32* saved) {
    for (const CleanWatchRange& range : ranges) {
        std::memcpy(saved, words + range.first, range.count * sizeof(u32));
        saved += range.count;
    }
}

bool CleanWordsEqual(const u32* words, std::span<const CleanWatchRange> ranges,
                     const u32* saved) {
    for (const CleanWatchRange& range : ranges) {
        if (std::memcmp(words + range.first, saved, range.count * sizeof(u32)) != 0) {
            return false;
        }
        saved += range.count;
    }
    return true;
}

} // namespace Vulkan
