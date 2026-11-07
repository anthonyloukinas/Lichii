// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <random>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/core.h"
#include "core/device_memory.h"
#include "tests/core/system_test_context.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/memory_manager.h"
#include "video_core/renderer_vulkan/fixed_pipeline_state.h"
#include "video_core/renderer_vulkan/pipeline_clean_watch.h"
#include "video_core/renderer_vulkan/vk_state_tracker.h"

namespace {

using Tegra::Engines::Maxwell3D;
using Vulkan::CleanWatch;
using Vulkan::CleanWatchRange;
using Vulkan::DynamicFeatures;
using Vulkan::FixedPipelineState;

struct Fixture {
    ScopedSystemTestContext context;
    Core::System system;
    Core::DeviceMemory physical;
    Tegra::MaxwellDeviceMemoryManager device_memory{physical};
    Tegra::MemoryManager memory{system, device_memory, 32, 1ULL << 31};
    Maxwell3D maxwell3d{memory};
};

DynamicFeatures Features(u32 bits) {
    DynamicFeatures features{};
    bool* const flags[] = {
        &features.has_extended_dynamic_state,
        &features.has_extended_dynamic_state_2,
        &features.has_extended_dynamic_state_2_logic_op,
        &features.has_extended_dynamic_state_3_blend,
        &features.has_extended_dynamic_state_3_enables,
        &features.has_dynamic_state3_depth_clamp_enable,
        &features.has_dynamic_state3_logic_op_enable,
        &features.has_dynamic_state3_line_stipple_enable,
        &features.has_dynamic_vertex_input,
        &features.has_static_vertex_repack,
        &features.has_color_write_enable,
        &features.has_provoking_vertex,
        &features.has_provoking_vertex_first_mode,
        &features.has_provoking_vertex_last_mode,
        &features.has_provoking_vertex_tf_preserve,
    };
    for (size_t i = 0; i < std::size(flags); ++i) {
        *flags[i] = ((bits >> i) & 1) != 0;
    }
    return features;
}

/// Refresh with every dirty group it recomputes set, so the key depends on the registers alone.
FixedPipelineState FullRefresh(Maxwell3D& maxwell3d, DynamicFeatures& features) {
    FixedPipelineState state;
    std::memset(&state, 0, sizeof(state));
    auto& flags = maxwell3d.dirty.flags;
    flags[Vulkan::Dirty::VertexInput] = true;
    flags[Vulkan::Dirty::ViewportSwizzles] = true;
    flags[Vulkan::Dirty::Blending] = true;
    state.Refresh(maxwell3d, features);
    return state;
}

std::vector<bool> Watched(const CleanWatch& watch, bool xfb) {
    std::vector<bool> watched(Maxwell3D::Regs::NUM_REGS);
    const auto add = [&watched](const std::vector<CleanWatchRange>& ranges) {
        for (const CleanWatchRange& range : ranges) {
            REQUIRE(range.first + range.count <= watched.size());
            for (u32 word = range.first; word < range.first + range.count; ++word) {
                watched[word] = true;
            }
        }
    };
    add(watch.ranges);
    if (xfb) {
        add(watch.xfb_ranges);
    }
    return watched;
}

size_t Words(const std::vector<CleanWatchRange>& ranges) {
    size_t words = 0;
    for (const CleanWatchRange& range : ranges) {
        words += range.count;
    }
    return words;
}

std::vector<u32> FeatureSets() {
    // No features, all of them, each one alone and a fixed random sample of the rest.
    std::vector<u32> sets{0, (1u << 15) - 1};
    for (u32 i = 0; i < 15; ++i) {
        sets.push_back(1u << i);
    }
    std::mt19937 rng{0xc1ea};
    for (int i = 0; i < 48; ++i) {
        sets.push_back(rng() & ((1u << 15) - 1));
    }
    return sets;
}

} // Anonymous namespace

TEST_CASE("Clean pipeline lookup: registers it doesn't watch don't change the key",
          "[video_core][pipeline_clean]") {
    Fixture f;
    auto& words = f.maxwell3d.regs.reg_array;
    std::mt19937 rng{0x5eed};
    for (const u32 bits : FeatureSets()) {
        DynamicFeatures features = Features(bits);
        const CleanWatch watch = Vulkan::BuildCleanWatch(f.maxwell3d.regs, features);
        for (const bool xfb : {false, true}) {
            CAPTURE(bits, xfb);
            f.maxwell3d.regs.transform_feedback_enabled = xfb ? 1 : 0;
            const std::vector<bool> watched = Watched(watch, xfb);
            const FixedPipelineState reference = FullRefresh(f.maxwell3d, features);
            for (u32 word = 0; word < Maxwell3D::Regs::NUM_REGS; ++word) {
                if (watched[word]) {
                    continue;
                }
                const u32 saved = words[word];
                for (const u32 value : {~saved, static_cast<u32>(rng())}) {
                    CAPTURE(word, value);
                    words[word] = value;
                    REQUIRE(FullRefresh(f.maxwell3d, features) == reference);
                }
                words[word] = saved;
            }
        }
    }
    f.maxwell3d.regs.transform_feedback_enabled = 0;
}

TEST_CASE("Clean pipeline lookup: each watched register makes it refresh",
          "[video_core][pipeline_clean]") {
    Fixture f;
    auto& words = f.maxwell3d.regs.reg_array;
    for (const u32 bits : FeatureSets()) {
        const DynamicFeatures features = Features(bits);
        const CleanWatch watch = Vulkan::BuildCleanWatch(f.maxwell3d.regs, features);
        CAPTURE(bits);
        REQUIRE(Words(watch.ranges) != 0);
        REQUIRE(Words(watch.xfb_ranges) != 0);
        for (const auto* ranges : {&watch.ranges, &watch.xfb_ranges}) {
            std::vector<u32> snapshot(Words(*ranges));
            Vulkan::SaveCleanWords(words.data(), *ranges, snapshot.data());
            REQUIRE(Vulkan::CleanWordsEqual(words.data(), *ranges, snapshot.data()));
            for (const CleanWatchRange& range : *ranges) {
                for (u32 word = range.first; word < range.first + range.count; ++word) {
                    CAPTURE(word);
                    words[word] ^= 1u << (word % 32);
                    REQUIRE_FALSE(Vulkan::CleanWordsEqual(words.data(), *ranges, snapshot.data()));
                    words[word] ^= 1u << (word % 32);
                }
            }
            REQUIRE(Vulkan::CleanWordsEqual(words.data(), *ranges, snapshot.data()));
        }
    }
}
