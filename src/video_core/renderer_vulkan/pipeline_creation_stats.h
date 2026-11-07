// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <span>
#include <string>
#include <string_view>

#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;

/// EDEN_NX2_PIPELINE_STATS (diagnostic, off by default). =1: every pipeline create chains
/// VkPipelineCreationFeedbackCreateInfo and logs the driver's duration and flags per pipeline and
/// per stage; with VK_KHR_pipeline_executable_properties it is created with
/// CAPTURE_STATISTICS and every statistic the driver returns is logged. =2 also captures the
/// internal representations and writes them to <dump dir>/<hash>.<name>.txt (or .bin).
namespace PipelineCreationStats {

/// 0 (off), 1 or 2, read once from the environment.
int Level();

/// The capture flags to add to a pipeline create for the current level (0 when off or when the
/// device does not expose the executable properties).
VkPipelineCreateFlags CaptureFlags(const Device& device);

/// Creation feedback storage for one pipeline create; it must outlive the create call.
struct Feedback {
    static constexpr u32 MaxStages = 5;
    VkPipelineCreationFeedback pipeline{};
    std::array<VkPipelineCreationFeedback, MaxStages> stages{};
    VkPipelineCreationFeedbackCreateInfo create_info{};

    /// Fills create_info for num_stages stages, chained in front of next; returns &create_info.
    const void* Chain(const void* next, u32 num_stages);
};

/// Logs the feedback (when given) and the executables' statistics (and, at level 2, their
/// internal representations) of a pipeline that was just created. stage_labels names the stages in
/// the order of the create's pStages.
void Report(const Device& device, VkPipeline pipeline, std::string_view kind, u64 hash,
            const Feedback* feedback, std::span<const std::string> stage_labels, double wall_ms);

} // namespace PipelineCreationStats

} // namespace Vulkan
