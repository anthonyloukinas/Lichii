// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <fmt/format.h>

#include "common/logging.h"
#include "video_core/renderer_vulkan/spirv_complexity.h"

namespace Vulkan {

namespace {

// Opcode and enum values from the SPIR-V 1.6 unified grammar (spirv.hpp11).
enum Op : u32 {
    OpExtInst = 12,
    OpExecutionMode = 16,
    OpTypeBool = 20,
    OpTypeInt = 21,
    OpTypeFloat = 22,
    OpTypeVector = 23,
    OpTypeMatrix = 24,
    OpTypeArray = 28,
    OpTypeRuntimeArray = 29,
    OpTypeStruct = 30,
    OpTypePointer = 32,
    OpConstantTrue = 41,
    OpConstantFalse = 42,
    OpConstant = 43,
    OpConstantComposite = 44,
    OpConstantNull = 46,
    OpSpecConstantTrue = 48,
    OpSpecConstantFalse = 49,
    OpSpecConstant = 50,
    OpSpecConstantComposite = 51,
    OpSpecConstantOp = 52,
    OpFunction = 54,
    OpFunctionEnd = 56,
    OpFunctionCall = 57,
    OpVariable = 59,
    OpLoad = 61,
    OpStore = 62,
    OpAccessChain = 65,
    OpInBoundsAccessChain = 66,
    OpPtrAccessChain = 67,
    OpInBoundsPtrAccessChain = 70,
    OpDecorate = 71,
    OpMemberDecorate = 72,
    OpConvertFToU = 109,
    OpQuantizeToF16 = 116,
    OpBitcast = 124,
    OpUDiv = 134,
    OpSDiv = 135,
    OpUMod = 137,
    OpSRem = 138,
    OpSMod = 139,
    OpControlBarrier = 224,
    OpMemoryBarrier = 225,
    OpAtomicLoad = 227,
    OpAtomicXor = 242,
    OpPhi = 245,
    OpLoopMerge = 246,
    OpSelectionMerge = 247,
    OpLabel = 248,
    OpBranchConditional = 250,
    OpSwitch = 251,
    OpImageSparseSampleImplicitLod = 305,
    OpImageSparseTexelsResident = 316,
    OpAtomicFlagTestAndSet = 318,
    OpAtomicFlagClear = 319,
    OpImageSparseRead = 320,
    OpExecutionModeId = 331,
    OpGroupNonUniformElect = 333,
    OpGroupNonUniformQuadSwap = 366,
    OpSubgroupBallotKHR = 4421,
    OpSubgroupReadInvocationKHR = 4432,
    OpAtomicFMinEXT = 5614,
    OpAtomicFMaxEXT = 5615,
    OpAtomicFAddEXT = 6035,
};

constexpr u32 OpImageSampleImplicitLod = 87;
constexpr u32 OpImageWrite = 99;

constexpr u32 StorageClassUniform = 2;
constexpr u32 StorageClassWorkgroup = 4;
constexpr u32 StorageClassPrivate = 6;
constexpr u32 StorageClassFunction = 7;
constexpr u32 StorageClassStorageBuffer = 12;
constexpr u32 StorageClassPhysicalStorageBuffer = 5349;

constexpr u32 ExecutionModeLocalSize = 17;
constexpr u32 ExecutionModeLocalSizeId = 38;
constexpr u32 DecorationBlock = 2;
constexpr u32 DecorationBufferBlock = 3;
constexpr u32 DecorationArrayStride = 6;
constexpr u32 DecorationBuiltIn = 11;
constexpr u32 DecorationOffset = 35;
constexpr u32 BuiltInWorkgroupSize = 25;

struct TypeInfo {
    u32 opcode{};
    u32 width{};       ///< scalar width in bits (int, float); component's for vectors
    bool is_float{};
    u32 element{};     ///< vector, matrix, array, pointer element type
    u32 count{};       ///< vector/matrix components, array length
    u32 storage{};     ///< pointer storage class
    std::vector<u32> members;
};

struct Walker {
    std::unordered_map<u32, TypeInfo> types;
    std::unordered_map<u32, u32> value_type;     ///< result id -> result type id
    std::unordered_map<u32, u32> constant_value; ///< 32-bit scalar constants
    std::unordered_set<u32> constants;
    std::unordered_map<u32, u32> array_stride;
    std::unordered_map<u64, u32> member_offset; ///< (struct << 32 | member) -> offset
    std::unordered_set<u32> block_structs;
    std::unordered_set<u32> buffer_block_structs;
    /// Pointers into buffers, by root variable: 1 storage buffer, 2 uniform buffer, 3 workgroup.
    std::unordered_map<u32, u8> buffer_kind;
    std::unordered_set<u32> local_vars;         ///< Function/Private variables of aggregate type
    u32 workgroup_size_builtin{};

    u64 SizeOf(u32 type_id, u32 depth = 0) const {
        const auto it = types.find(type_id);
        if (it == types.end() || depth > 32) {
            return 0;
        }
        const TypeInfo& type = it->second;
        switch (type.opcode) {
        case OpTypeBool:
            return 4;
        case OpTypeInt:
        case OpTypeFloat:
            return type.width / 8;
        case OpTypeVector:
        case OpTypeMatrix:
            return SizeOf(type.element, depth + 1) * type.count;
        case OpTypeArray: {
            const auto stride = array_stride.find(type_id);
            const u64 element = stride != array_stride.end() ? stride->second
                                                             : SizeOf(type.element, depth + 1);
            return element * type.count;
        }
        case OpTypeStruct: {
            u64 size = 0;
            u64 packed = 0;
            for (u32 member = 0; member < type.members.size(); ++member) {
                const u64 member_size = SizeOf(type.members[member], depth + 1);
                packed += member_size;
                const auto offset =
                    member_offset.find((static_cast<u64>(type_id) << 32) | member);
                if (offset != member_offset.end()) {
                    size = (std::max)(size, offset->second + member_size);
                }
            }
            return (std::max)(size, packed);
        }
        default:
            return 0;
        }
    }

    /// Width and kind of a scalar or vector type, 0 for anything else.
    std::pair<u32, bool> ScalarWidth(u32 type_id) const {
        const auto it = types.find(type_id);
        if (it == types.end()) {
            return {0, false};
        }
        const TypeInfo& type = it->second;
        if (type.opcode == OpTypeInt || type.opcode == OpTypeFloat) {
            return {type.width, type.is_float};
        }
        if (type.opcode == OpTypeVector) {
            return ScalarWidth(type.element);
        }
        return {0, false};
    }

    u32 PointerStorage(u32 pointer_id) const {
        if (const auto kind = buffer_kind.find(pointer_id); kind != buffer_kind.end()) {
            return kind->second == 1   ? StorageClassStorageBuffer
                   : kind->second == 2 ? StorageClassUniform
                                       : StorageClassWorkgroup;
        }
        const auto value = value_type.find(pointer_id);
        if (value == value_type.end()) {
            return ~0u;
        }
        const auto type = types.find(value->second);
        return type != types.end() && type->second.opcode == OpTypePointer ? type->second.storage
                                                                           : ~0u;
    }
};

bool IsType(u32 opcode) {
    return (opcode >= 19 && opcode <= 39) || opcode == 322 || opcode == 327 || opcode == 4472 ||
           opcode == 5341 || opcode == 5358;
}

} // Anonymous namespace

SpirvComplexity AnalyzeSpirv(std::span<const u32> code) {
    SpirvComplexity result;
    result.words = code.size();
    if (code.size() < 5 || code[0] != 0x07230203) {
        return result;
    }
    result.id_bound = code[3];
    Walker walker;
    std::vector<u32> workgroup_vars;
    bool workgroup_aliased = false;
    bool in_function = false;
    // Open structured constructs in block order: merge block id, is loop.
    std::vector<std::pair<u32, bool>> constructs;
    u32 open_loops = 0;
    u32 open_selections = 0;
    std::vector<u32> local_size_ids;

    for (size_t pos = 5; pos < code.size();) {
        const u32 first = code[pos];
        const u32 count = first >> 16;
        const u32 opcode = first & 0xffff;
        if (count == 0 || pos + count > code.size()) {
            break;
        }
        const std::span<const u32> w = code.subspan(pos, count);
        pos += count;
        ++result.instructions;
        if (in_function) {
            ++result.body_instructions;
        }
        const auto operand = [&](u32 index) -> u32 { return index < count ? w[index] : 0; };

        if (IsType(opcode)) {
            TypeInfo type{};
            type.opcode = opcode;
            switch (opcode) {
            case OpTypeInt:
                type.width = operand(2);
                break;
            case OpTypeFloat:
                type.width = operand(2);
                type.is_float = true;
                break;
            case OpTypeVector:
            case OpTypeMatrix:
                type.element = operand(2);
                type.count = operand(3);
                break;
            case OpTypeArray: {
                type.element = operand(2);
                const auto length = walker.constant_value.find(operand(3));
                type.count = length != walker.constant_value.end() ? length->second : 0;
                break;
            }
            case OpTypeRuntimeArray:
                type.element = operand(2);
                break;
            case OpTypeStruct:
                type.members.assign(w.begin() + 2, w.end());
                break;
            case OpTypePointer:
                type.storage = operand(2);
                type.element = operand(3);
                break;
            default:
                break;
            }
            if (opcode != 39) { // OpTypeForwardPointer declares no result
                walker.types.emplace(operand(1), std::move(type));
            }
            continue;
        }
        switch (opcode) {
        case OpExecutionMode:
            if (operand(2) == ExecutionModeLocalSize && count >= 6) {
                result.workgroup_size = {w[3], w[4], w[5]};
            }
            break;
        case OpExecutionModeId:
            if (operand(2) == ExecutionModeLocalSizeId && count >= 6) {
                local_size_ids = {w[3], w[4], w[5]};
            }
            break;
        case OpDecorate:
            if (operand(2) == DecorationArrayStride) {
                walker.array_stride[operand(1)] = operand(3);
            } else if (operand(2) == DecorationBlock) {
                walker.block_structs.insert(operand(1));
            } else if (operand(2) == DecorationBufferBlock) {
                walker.buffer_block_structs.insert(operand(1));
            } else if (operand(2) == DecorationBuiltIn && operand(3) == BuiltInWorkgroupSize) {
                walker.workgroup_size_builtin = operand(1);
            }
            break;
        case OpMemberDecorate:
            if (operand(3) == DecorationOffset) {
                walker.member_offset[(static_cast<u64>(operand(1)) << 32) | operand(2)] =
                    operand(4);
            }
            break;
        case OpConstant:
        case OpSpecConstant:
            walker.constant_value[operand(2)] = operand(3);
            [[fallthrough]];
        case OpConstantTrue:
        case OpConstantFalse:
        case OpConstantComposite:
        case OpConstantNull:
        case OpSpecConstantTrue:
        case OpSpecConstantFalse:
        case OpSpecConstantComposite:
        case OpSpecConstantOp:
            walker.constants.insert(operand(2));
            walker.value_type[operand(2)] = operand(1);
            ++result.constants;
            if (operand(2) == walker.workgroup_size_builtin && opcode == OpConstantComposite &&
                count >= 6) {
                for (u32 axis = 0; axis < 3; ++axis) {
                    const auto value = walker.constant_value.find(w[3 + axis]);
                    result.workgroup_size[axis] =
                        value != walker.constant_value.end() ? value->second : 0;
                }
            }
            break;
        case OpVariable: {
            ++result.variables;
            walker.value_type[operand(2)] = operand(1);
            const u32 storage = operand(3);
            const auto pointer = walker.types.find(operand(1));
            const u32 pointee = pointer != walker.types.end() ? pointer->second.element : 0;
            if (storage == StorageClassStorageBuffer ||
                storage == StorageClassPhysicalStorageBuffer) {
                walker.buffer_kind[operand(2)] = 1;
            } else if (storage == StorageClassUniform) {
                // SPIR-V 1.0 storage buffers are Uniform blocks decorated BufferBlock.
                walker.buffer_kind[operand(2)] = walker.buffer_block_structs.contains(pointee) ? 1 : 2;
            }
            if (storage == StorageClassWorkgroup) {
                walker.buffer_kind[operand(2)] = 3;
                workgroup_vars.push_back(pointee);
                workgroup_aliased |= walker.block_structs.contains(pointee);
            } else if (storage == StorageClassFunction || storage == StorageClassPrivate) {
                const auto type = walker.types.find(pointee);
                if (type != walker.types.end() && (type->second.opcode == OpTypeArray ||
                                                   type->second.opcode == OpTypeStruct)) {
                    result.local_bytes += walker.SizeOf(pointee);
                    walker.local_vars.insert(operand(2));
                }
            }
            break;
        }
        case OpFunction:
            in_function = true;
            ++result.functions;
            constructs.clear();
            open_loops = open_selections = 0;
            break;
        case OpFunctionEnd:
            in_function = false;
            break;
        case OpFunctionCall:
            ++result.function_calls;
            break;
        case OpLabel: {
            ++result.basic_blocks;
            // The merge block ends its construct (and any construct still open inside it).
            const u32 label = operand(1);
            const auto it = std::find_if(constructs.begin(), constructs.end(),
                                         [label](const auto& c) { return c.first == label; });
            if (it != constructs.end()) {
                for (auto close = it; close != constructs.end(); ++close) {
                    (close->second ? open_loops : open_selections) -= 1;
                }
                constructs.erase(it, constructs.end());
            }
            break;
        }
        case OpLoopMerge:
        case OpSelectionMerge: {
            const bool loop = opcode == OpLoopMerge;
            constructs.emplace_back(operand(1), loop);
            if (loop) {
                ++result.loops;
                ++open_loops;
                result.max_loop_depth = (std::max)(result.max_loop_depth, open_loops);
            } else {
                ++result.selections;
                ++open_selections;
                result.max_selection_depth =
                    (std::max)(result.max_selection_depth, open_selections);
            }
            result.max_construct_depth =
                (std::max)(result.max_construct_depth, static_cast<u32>(constructs.size()));
            break;
        }
        case OpSwitch:
            ++result.switches;
            break;
        case OpBranchConditional:
            ++result.branches_conditional;
            break;
        case OpPhi:
            ++result.phis;
            break;
        case OpControlBarrier:
            ++result.barriers;
            break;
        case OpMemoryBarrier:
            ++result.memory_barriers;
            break;
        case OpExtInst:
            ++result.ext_insts;
            break;
        case OpUDiv:
        case OpSDiv:
        case OpUMod:
        case OpSRem:
        case OpSMod:
            ++result.int_div_mod;
            break;
        case OpLoad: {
            const u32 storage = walker.PointerStorage(operand(3));
            if (storage == StorageClassStorageBuffer ||
                storage == StorageClassPhysicalStorageBuffer) {
                ++result.ssbo_loads;
            } else if (storage == StorageClassUniform) {
                ++result.ubo_loads;
            } else if (storage == StorageClassWorkgroup) {
                ++result.shared_loads;
            }
            break;
        }
        case OpStore: {
            const u32 storage = walker.PointerStorage(operand(1));
            if (storage == StorageClassStorageBuffer ||
                storage == StorageClassPhysicalStorageBuffer) {
                ++result.ssbo_stores;
            } else if (storage == StorageClassWorkgroup) {
                ++result.shared_stores;
            }
            break;
        }
        case OpAccessChain:
        case OpInBoundsAccessChain:
        case OpPtrAccessChain:
        case OpInBoundsPtrAccessChain:
            if (const auto kind = walker.buffer_kind.find(operand(3));
                kind != walker.buffer_kind.end()) {
                walker.buffer_kind[operand(2)] = kind->second;
            }
            if (walker.local_vars.contains(operand(3))) {
                for (u32 index = 4; index < count; ++index) {
                    if (!walker.constants.contains(w[index])) {
                        ++result.local_dynamic_index;
                        break;
                    }
                }
            }
            break;
        default:
            break;
        }
        if ((opcode >= OpAtomicLoad && opcode <= OpAtomicXor) || opcode == OpAtomicFlagTestAndSet ||
            opcode == OpAtomicFlagClear || opcode == OpAtomicFMinEXT ||
            opcode == OpAtomicFMaxEXT || opcode == OpAtomicFAddEXT) {
            ++result.atomics;
        }
        if ((opcode >= OpImageSampleImplicitLod && opcode <= OpImageWrite) ||
            (opcode >= OpImageSparseSampleImplicitLod && opcode < OpImageSparseTexelsResident) ||
            opcode == OpImageSparseRead) {
            ++result.image_ops;
        }
        if ((opcode >= OpGroupNonUniformElect && opcode <= OpGroupNonUniformQuadSwap) ||
            (opcode >= OpSubgroupBallotKHR && opcode <= OpSubgroupReadInvocationKHR)) {
            ++result.subgroup_ops;
        }
        if ((opcode >= OpConvertFToU && opcode <= OpQuantizeToF16) || opcode == OpBitcast) {
            ++result.conversions;
        }
        // Result-typed body instructions: record the type (pointers for the load/store counts)
        // and count the 64/16/8-bit results. A body instruction's first operand names a declared
        // type only when it is the result type (ids are unique), so no opcode table is needed.
        if (in_function && count >= 3 && walker.types.contains(w[1])) {
            walker.value_type[w[2]] = w[1];
            const auto [width, is_float] = walker.ScalarWidth(w[1]);
            if (width == 64) {
                ++(is_float ? result.float64_ops : result.int64_ops);
            } else if (width == 16) {
                ++(is_float ? result.float16_ops : result.int16_ops);
            } else if (width == 8 && !is_float) {
                ++result.int8_ops;
            }
        }
    }
    if (!local_size_ids.empty()) {
        for (u32 axis = 0; axis < 3; ++axis) {
            const auto value = walker.constant_value.find(local_size_ids[axis]);
            result.workgroup_size[axis] = value != walker.constant_value.end() ? value->second : 0;
        }
    }
    for (const u32 type : workgroup_vars) {
        const u64 size = walker.SizeOf(type);
        result.shared_bytes = workgroup_aliased ? (std::max)(result.shared_bytes, size)
                                                : result.shared_bytes + size;
    }
    return result;
}

std::string FormatSpirvComplexity(const SpirvComplexity& c) {
    return fmt::format(
        "words={} insts={} body_insts={} id_bound={} functions={} calls={} blocks={} loops={} "
        "loop_depth={} selections={} selection_depth={} construct_depth={} switches={} "
        "cond_branches={} phis={} workgroup={}x{}x{} shared_bytes={} local_bytes={} "
        "local_dyn_index={} barriers={} memory_barriers={} i64_ops={} f64_ops={} i16_ops={} "
        "f16_ops={} i8_ops={} atomics={} image_ops={} subgroup_ops={} ssbo_loads={} "
        "ssbo_stores={} ubo_loads={} shared_loads={} shared_stores={} ext_insts={} int_div_mod={} "
        "conversions={} constants={} variables={}",
        c.words, c.instructions, c.body_instructions, c.id_bound, c.functions, c.function_calls,
        c.basic_blocks, c.loops, c.max_loop_depth, c.selections, c.max_selection_depth,
        c.max_construct_depth, c.switches, c.branches_conditional, c.phis, c.workgroup_size[0],
        c.workgroup_size[1], c.workgroup_size[2], c.shared_bytes, c.local_bytes,
        c.local_dynamic_index, c.barriers, c.memory_barriers, c.int64_ops, c.float64_ops,
        c.int16_ops, c.float16_ops, c.int8_ops, c.atomics, c.image_ops, c.subgroup_ops,
        c.ssbo_loads, c.ssbo_stores, c.ubo_loads, c.shared_loads, c.shared_stores, c.ext_insts,
        c.int_div_mod, c.conversions, c.constants, c.variables);
}

bool ShaderStatsEnabled() {
    static const bool enabled = [] {
        const char* const value = std::getenv("EDEN_NX2_SHADER_STATS");
        return value != nullptr && value[0] != '\0' && std::string_view{value} != "0";
    }();
    return enabled;
}

void LogShaderStats(u64 hash, std::string_view stage, std::span<const u32> code,
                    bool from_cache) {
    if (!ShaderStatsEnabled()) {
        return;
    }
    LOG_INFO(Render_Vulkan, "Shader stats {:016x} {}{}: {}", hash, stage,
             from_cache ? " (translation cache)" : "",
             FormatSpirvComplexity(AnalyzeSpirv(code)));
}

} // namespace Vulkan
