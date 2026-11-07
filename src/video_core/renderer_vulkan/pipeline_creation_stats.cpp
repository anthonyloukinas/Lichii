// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdlib>
#include <mutex>
#include <set>
#include <string>

#include <fmt/format.h>

#include "common/fs/file.h"
#include "common/fs/fs.h"
#include "common/fs/fs_util.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "video_core/renderer_vulkan/pipeline_creation_stats.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan::PipelineCreationStats {

namespace {

std::string FeedbackText(const VkPipelineCreationFeedback& feedback) {
    const VkPipelineCreationFeedbackFlags flags = feedback.flags;
    return fmt::format("valid={} cache_hit={} base_accel={} duration={:.3f} ms",
                       (flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT) != 0 ? 1 : 0,
                       (flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT) !=
                               0
                           ? 1
                           : 0,
                       (flags & VK_PIPELINE_CREATION_FEEDBACK_BASE_PIPELINE_ACCELERATION_BIT) != 0
                           ? 1
                           : 0,
                       static_cast<double>(feedback.duration) / 1e6);
}

std::string StatisticValue(const VkPipelineExecutableStatisticKHR& statistic) {
    switch (statistic.format) {
    case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR:
        return statistic.value.b32 ? "true" : "false";
    case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR:
        return fmt::format("{}", statistic.value.i64);
    case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR:
        return fmt::format("{}", statistic.value.u64);
    case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_FLOAT64_KHR:
        return fmt::format("{}", statistic.value.f64);
    default:
        return "?";
    }
}

std::string SafeFileName(std::string_view name) {
    std::string out;
    for (const char c : name) {
        const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '-' || c == '_';
        out.push_back(keep ? c : '_');
    }
    return out.empty() ? std::string{"repr"} : out;
}

std::string StageNames(VkShaderStageFlags stages) {
    static constexpr std::array<std::pair<VkShaderStageFlagBits, const char*>, 8> names{{
        {VK_SHADER_STAGE_VERTEX_BIT, "vertex"},
        {VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, "tess_control"},
        {VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, "tess_eval"},
        {VK_SHADER_STAGE_GEOMETRY_BIT, "geometry"},
        {VK_SHADER_STAGE_FRAGMENT_BIT, "fragment"},
        {VK_SHADER_STAGE_COMPUTE_BIT, "compute"},
        {VK_SHADER_STAGE_TASK_BIT_EXT, "task"},
        {VK_SHADER_STAGE_MESH_BIT_EXT, "mesh"},
    }};
    std::string out;
    for (const auto& [bit, name] : names) {
        if ((stages & bit) != 0) {
            out += out.empty() ? "" : "+";
            out += name;
        }
    }
    return out.empty() ? fmt::format("{:#x}", stages) : out;
}

/// Logs each statistic's description the first time its name shows up.
void LogLegend(const VkPipelineExecutableStatisticKHR& statistic) {
    static std::mutex mutex;
    static std::set<std::string> seen;
    std::scoped_lock lock{mutex};
    if (seen.insert(statistic.name).second) {
        LOG_INFO(Render_Vulkan, "Pipeline stat legend: {} = {}", statistic.name,
                 statistic.description);
    }
}

} // Anonymous namespace

int Level() {
    static const int level = [] {
        const char* const value = std::getenv("EDEN_NX2_PIPELINE_STATS");
        if (value == nullptr || value[0] == '\0' || std::string_view{value} == "0") {
            return 0;
        }
        return std::string_view{value} == "2" ? 2 : 1;
    }();
    return level;
}

VkPipelineCreateFlags CaptureFlags(const Device& device) {
    const int level = Level();
    if (level == 0 || !device.IsKhrPipelineExecutablePropertiesEnabled()) {
        return 0;
    }
    VkPipelineCreateFlags flags = VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
    if (level >= 2) {
        flags |= VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR;
    }
    return flags;
}

const void* Feedback::Chain(const void* next, u32 num_stages) {
    pipeline = {};
    stages = {};
    create_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO,
        .pNext = next,
        .pPipelineCreationFeedback = &pipeline,
        .pipelineStageCreationFeedbackCount = num_stages < MaxStages ? num_stages : MaxStages,
        .pPipelineStageCreationFeedbacks = stages.data(),
    };
    return &create_info;
}

void Report(const Device& device, VkPipeline pipeline, std::string_view kind, u64 hash,
            const Feedback* feedback, std::span<const std::string> stage_labels, double wall_ms) {
    if (feedback != nullptr) {
        LOG_INFO(Render_Vulkan, "Pipeline stats {:016x} {}: feedback {} (wall {:.3f} ms)", hash,
                 kind, FeedbackText(feedback->pipeline), wall_ms);
        const u32 count = feedback->create_info.pipelineStageCreationFeedbackCount;
        for (u32 stage = 0; stage < count; ++stage) {
            LOG_INFO(Render_Vulkan, "Pipeline stats {:016x} {} stage {} {}: feedback {}", hash,
                     kind, stage, stage < stage_labels.size() ? stage_labels[stage] : "?",
                     FeedbackText(feedback->stages[stage]));
        }
    }
    if (!device.IsKhrPipelineExecutablePropertiesEnabled() || pipeline == VK_NULL_HANDLE) {
        return;
    }
    const vk::Device& logical = device.GetLogical();
    try {
        const auto executables = logical.GetPipelineExecutablePropertiesKHR(pipeline);
        for (u32 index = 0; index < static_cast<u32>(executables.size()); ++index) {
            const auto& executable = executables[index];
            const auto statistics = logical.GetPipelineExecutableStatisticsKHR(pipeline, index);
            std::string text;
            for (const auto& statistic : statistics) {
                LogLegend(statistic);
                text += fmt::format("{}{}={}", text.empty() ? "" : "; ", statistic.name,
                                    StatisticValue(statistic));
            }
            LOG_INFO(Render_Vulkan,
                     "Pipeline stats {:016x} {} exe {} \"{}\" stages={} subgroup={}: {}", hash,
                     kind, index, executable.name, StageNames(executable.stages),
                     executable.subgroupSize, text);
            if (Level() < 2) {
                continue;
            }
            const auto representations =
                logical.GetPipelineExecutableInternalRepresentationsKHR(pipeline, index);
            const auto dump_dir = Common::FS::GetEdenPath(Common::FS::EdenPath::DumpDir);
            [[maybe_unused]] const bool created = Common::FS::CreateDir(dump_dir);
            for (const auto& representation : representations) {
                const std::string exe_part =
                    executables.size() > 1 ? fmt::format(".e{}", index) : std::string{};
                const auto path =
                    dump_dir / fmt::format("{:016x}{}.{}.{}", hash, exe_part,
                                           SafeFileName(representation.name),
                                           representation.is_text ? "txt" : "bin");
                size_t size = representation.data.size();
                if (representation.is_text) {
                    // Text representations are null-terminated; the file holds the text only.
                    while (size > 0 && representation.data[size - 1] == '\0') {
                        --size;
                    }
                }
                bool written = false;
                {
                    Common::FS::IOFile file(path, Common::FS::FileAccessMode::Write,
                                            Common::FS::FileType::BinaryFile);
                    if (file.IsOpen()) {
                        written = file.WriteSpan(std::span<const char>(representation.data.data(),
                                                                       size)) == size;
                    }
                }
                LOG_INFO(Render_Vulkan,
                         "Pipeline stats {:016x} {} exe {} repr \"{}\" ({}): {} bytes{} -> {}",
                         hash, kind, index, representation.name, representation.description,
                         size, representation.is_text ? " text" : "",
                         written ? Common::FS::PathToUTF8String(path) : std::string{"not written"});
            }
        }
    } catch (const vk::Exception& exception) {
        LOG_WARNING(Render_Vulkan, "Pipeline stats {:016x} {}: query failed: {}", hash, kind,
                    exception.what());
    }
}

} // namespace Vulkan::PipelineCreationStats
