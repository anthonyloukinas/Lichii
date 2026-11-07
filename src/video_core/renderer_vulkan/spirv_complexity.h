// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <span>
#include <string>
#include <string_view>

#include "common/common_types.h"

namespace Vulkan {

/// Counts read from emitted SPIR-V words (EDEN_NX2_SHADER_STATS, a diagnostic).
struct SpirvComplexity {
    u64 words{};
    u64 instructions{};
    u64 body_instructions{}; ///< between OpFunction and OpFunctionEnd
    u32 id_bound{};
    u32 functions{};
    u32 function_calls{};
    u32 basic_blocks{};
    u32 loops{};             ///< OpLoopMerge
    u32 max_loop_depth{};    ///< nesting of loop constructs in block order
    u32 selections{};        ///< OpSelectionMerge
    u32 max_selection_depth{};
    u32 max_construct_depth{}; ///< loops and selections together
    u32 switches{};
    u32 phis{};
    u32 branches_conditional{};
    std::array<u32, 3> workgroup_size{};
    u64 shared_bytes{};      ///< Workgroup variables (max instead of sum when they alias)
    u64 local_bytes{};       ///< Function and Private variables of array or struct type
    u32 local_dynamic_index{}; ///< access chains into those with a non-constant index
    u32 barriers{};          ///< OpControlBarrier
    u32 memory_barriers{};   ///< OpMemoryBarrier
    u32 int64_ops{};         ///< body instructions whose result is a 64-bit integer (or vector)
    u32 float64_ops{};
    u32 int16_ops{};
    u32 float16_ops{};
    u32 int8_ops{};
    u32 atomics{};
    u32 image_ops{};         ///< sample, fetch, gather, read, write (sparse too)
    u32 subgroup_ops{};
    u32 ssbo_loads{};        ///< OpLoad from StorageBuffer / PhysicalStorageBuffer pointers
    u32 ssbo_stores{};
    u32 ubo_loads{};         ///< OpLoad from Uniform pointers
    u32 shared_loads{};
    u32 shared_stores{};
    u32 ext_insts{};         ///< OpExtInst (GLSL.std.450 and others)
    u32 int_div_mod{};       ///< OpUDiv, OpSDiv, OpUMod, OpSMod, OpSRem
    u32 conversions{};       ///< OpConvert*, OpUConvert, OpSConvert, OpFConvert, OpBitcast
    u32 constants{};
    u32 variables{};
};

SpirvComplexity AnalyzeSpirv(std::span<const u32> code);

std::string FormatSpirvComplexity(const SpirvComplexity& complexity);

/// EDEN_NX2_SHADER_STATS=1, read once.
bool ShaderStatsEnabled();

/// Logs "Shader stats <hash> <stage>: ..." when the switch is on.
void LogShaderStats(u64 hash, std::string_view stage, std::span<const u32> code,
                    bool from_cache);

} // namespace Vulkan
