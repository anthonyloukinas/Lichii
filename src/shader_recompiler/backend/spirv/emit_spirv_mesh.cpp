// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Switch 2 (NVN) task and mesh programs. They talk to the hardware through the stage buffer
// (ISBE): a task program writes the number of mesh workgroups at byte 4 of its output region and
// its payload after it; each mesh workgroup reads that region (its own index at byte 0) and
// writes its vertices and primitives to an attribute region in groups of 32, one 32-word line per
// attribute, plus the primitive count and 8-bit vertex indices to a map region. Here the task's
// output region is the task payload, the mesh output regions live in workgroup memory, and the
// epilogue (reached by every invocation, see structured_control_flow.cpp) hands them to Vulkan.

#include <algorithm>
#include <span>
#include <vector>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"

namespace Shader::Backend::SPIRV {
namespace {
void Barrier(EmitContext& ctx) {
    ctx.OpControlBarrier(ctx.Const(static_cast<u32>(spv::Scope::Workgroup)),
                         ctx.Const(static_cast<u32>(spv::Scope::Workgroup)),
                         ctx.Const(static_cast<u32>(spv::MemorySemanticsMask::AcquireRelease |
                                                    spv::MemorySemanticsMask::WorkgroupMemory)));
}

Id LocalInvocation(EmitContext& ctx) {
    return ctx.OpCompositeExtract(ctx.U32[1], ctx.OpLoad(ctx.U32[3], ctx.local_invocation_id), 0U);
}

Id Clamped(EmitContext& ctx, Id index, u32 count) {
    return ctx.OpUMin(ctx.U32[1], index, ctx.Const(count - 1));
}

Id PayloadWord(EmitContext& ctx, Id index) {
    return ctx.OpAccessChain(ctx.mesh_payload_u32, ctx.mesh_payload,
                             Clamped(ctx, index, ctx.mesh_info.payload_words));
}

Id SharedWord(EmitContext& ctx, Id array, Id index) {
    return ctx.OpAccessChain(ctx.mesh_shared_u32, array, index);
}

// Runs body under `if (condition)`.
template <typename Func>
void If(EmitContext& ctx, Id condition, Func&& body) {
    const Id then_label{ctx.OpLabel()};
    const Id merge_label{ctx.OpLabel()};
    ctx.OpSelectionMerge(merge_label, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(condition, then_label, merge_label);
    ctx.AddLabel(then_label);
    body();
    ctx.OpBranch(merge_label);
    ctx.AddLabel(merge_label);
}

// Copies the lines of element `index` (a vertex or a primitive) from the attribute region.
void CopyLines(EmitContext& ctx, Id index, u32 base, u32 num_lines,
               const std::vector<EmitContext::MeshLineTarget>& targets) {
    // Word of line 0: base + (index / 32) * num_lines * 32 + index % 32.
    const Id group{ctx.OpShiftRightLogical(ctx.U32[1], index, ctx.Const(5U))};
    const Id slot{ctx.OpBitwiseAnd(ctx.U32[1], index, ctx.Const(31U))};
    const Id first{ctx.OpIAdd(ctx.U32[1],
                              ctx.OpIMul(ctx.U32[1], group, ctx.Const(num_lines * 32)),
                              ctx.OpIAdd(ctx.U32[1], slot, ctx.Const(base)))};
    for (u32 line = 0; line < num_lines; ++line) {
        const EmitContext::MeshLineTarget& target{targets[line]};
        if (!Sirit::ValidId(target.variable)) {
            continue;
        }
        const Id word{ctx.OpLoad(
            ctx.U32[1],
            SharedWord(ctx, ctx.mesh_attributes, ctx.OpIAdd(ctx.U32[1], first, ctx.Const(line * 32))))};
        const Id value{target.is_float ? ctx.OpBitcast(ctx.F32[1], word) : word};
        const Id pointer_type{target.is_float ? ctx.output_f32 : ctx.output_u32};
        const Id pointer{target.element == ~0U
                             ? ctx.OpAccessChain(pointer_type, target.variable, index)
                             : ctx.OpAccessChain(pointer_type, target.variable, index,
                                                 ctx.Const(target.element))};
        ctx.OpStore(pointer, value);
    }
}

// OpenGL-style clip space: z = (z + w) / 2, from the stage buffer's position lines.
void ConvertDepth(EmitContext& ctx, Id vertex) {
    const auto& lines{ctx.mesh_info.vertex_lines};
    const auto z_line{std::ranges::find(lines, static_cast<u32>(IR::Attribute::PositionZ))};
    const auto w_line{std::ranges::find(lines, static_cast<u32>(IR::Attribute::PositionW))};
    if (z_line == lines.end() || w_line == lines.end()) {
        return;
    }
    const u32 num_lines{static_cast<u32>(lines.size())};
    const Id group{ctx.OpShiftRightLogical(ctx.U32[1], vertex, ctx.Const(5U))};
    const Id slot{ctx.OpBitwiseAnd(ctx.U32[1], vertex, ctx.Const(31U))};
    const Id first{ctx.OpIAdd(ctx.U32[1], ctx.OpIMul(ctx.U32[1], group, ctx.Const(num_lines * 32)),
                              slot)};
    const auto read = [&](auto line) {
        const u32 offset{static_cast<u32>(line - lines.begin()) * 32};
        return ctx.OpBitcast(ctx.F32[1],
                             ctx.OpLoad(ctx.U32[1], SharedWord(ctx, ctx.mesh_attributes,
                                                               ctx.OpIAdd(ctx.U32[1], first,
                                                                          ctx.Const(offset)))));
    };
    const Id z{ctx.OpFMul(ctx.F32[1], ctx.OpFAdd(ctx.F32[1], read(z_line), read(w_line)),
                          ctx.Const(0.5f))};
    ctx.OpStore(ctx.OpAccessChain(ctx.output_f32, ctx.output_position, vertex, ctx.Const(2U)), z);
}

void TaskEpilogue(EmitContext& ctx) {
    Barrier(ctx);
    const Id count{ctx.OpLoad(ctx.U32[1], PayloadWord(ctx, ctx.Const(1U)))};
    const Id one{ctx.Const(1U)};
    // Marker for OpEmitMeshTasksEXT count, 1, 1, payload (PatchMeshInstructions).
    ctx.OpStore(count, one);
    ctx.OpStore(one, ctx.mesh_payload);
    ctx.mesh_tasks_marker = {count, one, one, ctx.mesh_payload};
}

constexpr u32 DEBUG_BALLOTS = 4;
constexpr u32 DEBUG_SHUFFLES = 53;
constexpr u32 DEBUG_RECORD_WORDS = 176;

void DefineDebugShuffles(EmitContext& ctx) {
    if (!ctx.mesh_debug_shuffles.empty()) {
        return;
    }
    const Id pointer_type{ctx.TypePointer(spv::StorageClass::Private, ctx.U32[1])};
    for (u32 i = 0; i < DEBUG_BALLOTS + DEBUG_SHUFFLES * 3; ++i) {
        const Id variable{ctx.AddGlobalVariable(pointer_type, spv::StorageClass::Private,
                                                ctx.Const(0xdeadbeefU))};
        if (ctx.profile.supported_spirv >= 0x00010400) {
            ctx.interfaces.push_back(variable);
        }
        ctx.mesh_debug_shuffles.push_back(variable);
    }
}

// Debug only (RuntimeInfo::mesh_positions_address): appends {tag, workgroup x, payload hash,
// vertex | primitive count << 16, position, subgroup lane, local invocation x, flags, ballot count,
// the first DEBUG_BALLOTS ballots, then value, source and result of the first DEBUG_SHUFFLES
// ShuffleIndex ops} while the host has armed the buffer.
void RecordPosition(EmitContext& ctx, Id vertex, Id count) {
    const u64 base{ctx.runtime_info.mesh_positions_address};
    ctx.AddCapability(spv::Capability::PhysicalStorageBufferAddresses);
    ctx.AddCapability(spv::Capability::Int64);
    ctx.AddExtension("SPV_KHR_physical_storage_buffer");
    ctx.SetMemoryModel(spv::AddressingModel::PhysicalStorageBuffer64, spv::MemoryModel::GLSL450);
    const Id u64_type{ctx.TypeInt(64, false)};
    const Id word_pointer{ctx.TypePointer(spv::StorageClass::PhysicalStorageBuffer, ctx.U32[1])};
    const auto pointer = [&](Id address) { return ctx.OpConvertUToPtr(word_pointer, address); };
    const auto header = [&](u64 offset) {
        return pointer(ctx.Constant(u64_type, base + offset));
    };
    const Id armed{ctx.OpLoad(ctx.U32[1], header(4), spv::MemoryAccessMask::Aligned, u32{4})};
    const Id cap{ctx.OpLoad(ctx.U32[1], header(8), spv::MemoryAccessMask::Aligned, u32{4})};
    If(ctx, ctx.OpINotEqual(ctx.U1, armed, ctx.u32_zero_value), [&] {
        const Id index{ctx.OpAtomicIAdd(ctx.U32[1], header(0),
                                        ctx.Const(static_cast<u32>(spv::Scope::Device)),
                                        ctx.u32_zero_value, ctx.Const(1U))};
        If(ctx, ctx.OpULessThan(ctx.U1, index, cap), [&] {
            Id workgroup{ctx.u32_zero_value};
            Id hash{ctx.Const(2166136261U)};
            if (Sirit::ValidId(ctx.workgroup_id)) {
                const Id id{ctx.OpLoad(ctx.U32[3], ctx.workgroup_id)};
                workgroup = ctx.OpCompositeExtract(ctx.U32[1], id, 0U);
                hash = ctx.OpIMul(ctx.U32[1],
                                  ctx.OpBitwiseXor(ctx.U32[1], hash,
                                                   ctx.OpCompositeExtract(ctx.U32[1], id, 1U)),
                                  ctx.Const(16777619U));
            }
            if (Sirit::ValidId(ctx.mesh_payload)) {
                for (u32 word = 0; word < std::min(ctx.mesh_info.payload_words, 16U); ++word) {
                    const Id value{ctx.OpLoad(ctx.U32[1], PayloadWord(ctx, ctx.Const(word)))};
                    hash = ctx.OpIMul(ctx.U32[1], ctx.OpBitwiseXor(ctx.U32[1], hash, value),
                                      ctx.Const(16777619U));
                }
            }
            const Id position_pointer{ctx.TypePointer(spv::StorageClass::Output, ctx.F32[4])};
            const Id position{ctx.OpLoad(
                ctx.F32[4], ctx.OpAccessChain(position_pointer, ctx.output_position, vertex))};
            const Id record{ctx.OpIAdd(
                u64_type, ctx.Constant(u64_type, base + 64),
                ctx.OpIMul(u64_type, ctx.OpUConvert(u64_type, index), ctx.Constant(u64_type, u64{DEBUG_RECORD_WORDS} * 4)))};
            const bool subgroups{ctx.profile.SupportsSubgroupStage(ctx.stage)};
            if (subgroups && !Sirit::ValidId(ctx.subgroup_local_invocation_id)) {
                ctx.AddCapability(spv::Capability::GroupNonUniform);
                ctx.subgroup_local_invocation_id = ctx.AddGlobalVariable(
                    ctx.TypePointer(spv::StorageClass::Input, ctx.U32[1]), spv::StorageClass::Input);
                ctx.Decorate(ctx.subgroup_local_invocation_id, spv::Decoration::BuiltIn,
                             spv::BuiltIn::SubgroupLocalInvocationId);
                ctx.interfaces.push_back(ctx.subgroup_local_invocation_id);
            }
            DefineDebugShuffles(ctx);
            const u32 flags{(subgroups ? 1U : 0U) |
                            (ctx.profile.warp_size_potentially_larger_than_guest ? 2U : 0U) |
                            (ctx.mesh_threads << 8) |
                            (std::min(ctx.mesh_debug_shuffle_count, 0xffU) << 24)};
            std::vector<Id> words{
                ctx.Const(ctx.runtime_info.mesh_positions_tag),
                workgroup,
                hash,
                ctx.OpBitwiseOr(ctx.U32[1], vertex,
                                ctx.OpShiftLeftLogical(ctx.U32[1], count, ctx.Const(16U))),
                ctx.OpBitcast(ctx.U32[1], ctx.OpCompositeExtract(ctx.F32[1], position, 0U)),
                ctx.OpBitcast(ctx.U32[1], ctx.OpCompositeExtract(ctx.F32[1], position, 1U)),
                ctx.OpBitcast(ctx.U32[1], ctx.OpCompositeExtract(ctx.F32[1], position, 2U)),
                ctx.OpBitcast(ctx.U32[1], ctx.OpCompositeExtract(ctx.F32[1], position, 3U)),
                subgroups ? ctx.OpLoad(ctx.U32[1], ctx.subgroup_local_invocation_id)
                          : ctx.Const(0xffffffffU),
                LocalInvocation(ctx),
                ctx.Const(flags),
                ctx.Const(ctx.mesh_debug_ballot_count),
            };
            for (const Id variable : ctx.mesh_debug_shuffles) {
                words.push_back(ctx.OpLoad(ctx.U32[1], variable));
            }
            while (words.size() < DEBUG_RECORD_WORDS) {
                words.push_back(ctx.u32_zero_value);
            }
            for (u32 element = 0; element < words.size(); ++element) {
                const Id address{ctx.OpIAdd(u64_type, record,
                                            ctx.Constant(u64_type, u64{element} * 4))};
                ctx.OpStore(pointer(address), words[element], spv::MemoryAccessMask::Aligned,
                            u32{4});
            }
        });
    });
}

void MeshEpilogue(EmitContext& ctx) {
    const MeshInfo& mesh{ctx.mesh_info};
    Barrier(ctx);
    const Id count{ctx.OpUMin(ctx.U32[1], ctx.OpLoad(ctx.U32[1], ctx.mesh_primitive_count),
                              ctx.Const(mesh.max_primitives))};
    const Id max_vertices{ctx.Const(mesh.max_vertices)};
    // Marker for OpSetMeshOutputsEXT max_vertices, count (PatchMeshInstructions).
    ctx.OpStore(max_vertices, count);
    ctx.mesh_outputs_marker = {max_vertices, count};

    const Id invocation{LocalInvocation(ctx)};
    const u32 threads{ctx.mesh_threads};
    const bool convert_depth{ctx.runtime_info.convert_depth_mode && !ctx.profile.support_native_ndc &&
                             Sirit::ValidId(ctx.output_position)};
    const u32 vertex_lines{static_cast<u32>(mesh.vertex_lines.size())};
    const bool record_positions{ctx.runtime_info.mesh_positions_address != 0 &&
                                Sirit::ValidId(ctx.output_position)};
    for (u32 first = 0; first < mesh.max_vertices; first += threads) {
        const Id vertex{ctx.OpIAdd(ctx.U32[1], invocation, ctx.Const(first))};
        const auto copy = [&] {
            CopyLines(ctx, vertex, 0, vertex_lines, ctx.mesh_vertex_targets);
            if (convert_depth) {
                ConvertDepth(ctx, vertex);
            }
            if (record_positions && first == 0) {
                RecordPosition(ctx, vertex, count);
            }
        };
        if (first + threads > mesh.max_vertices) {
            If(ctx, ctx.OpULessThan(ctx.U1, vertex, max_vertices), copy);
        } else {
            copy();
        }
    }
    const u32 primitive_lines{static_cast<u32>(mesh.primitive_lines.size())};
    for (u32 first = 0; first < mesh.max_primitives; first += threads) {
        const Id primitive{ctx.OpIAdd(ctx.U32[1], invocation, ctx.Const(first))};
        If(ctx, ctx.OpULessThan(ctx.U1, primitive, count), [&] {
            // Vertex indices: map bytes 4 + 3 * primitive.
            const Id byte{ctx.OpIAdd(ctx.U32[1], ctx.OpIMul(ctx.U32[1], primitive, ctx.Const(3U)),
                                     ctx.Const(4U))};
            std::array<Id, 3> indices;
            for (u32 element = 0; element < 3; ++element) {
                const Id address{ctx.OpIAdd(ctx.U32[1], byte, ctx.Const(element))};
                indices[element] = ctx.OpLoad(ctx.U32[1], SharedWord(ctx, ctx.mesh_map, address));
            }
            const Id output_indices{ctx.TypePointer(spv::StorageClass::Output, ctx.U32[3])};
            ctx.OpStore(ctx.OpAccessChain(output_indices, ctx.mesh_primitive_indices, primitive),
                        ctx.OpCompositeConstruct(ctx.U32[3], std::span<const Id>(indices)));
            CopyLines(ctx, primitive, ctx.mesh_primitive_base, primitive_lines,
                      ctx.mesh_primitive_targets);
        });
    }
}
} // Anonymous namespace

Id EmitIsbeRead(EmitContext& ctx, Id address) {
    Id workgroup{
        ctx.OpCompositeExtract(ctx.U32[1], ctx.OpLoad(ctx.U32[3], ctx.workgroup_id), 0U)};
    if (Sirit::ValidId(ctx.mesh_draw_base_push_constant)) {
        const Id pointer_type{ctx.TypePointer(spv::StorageClass::PushConstant, ctx.U32[1])};
        const Id pointer{ctx.OpAccessChain(pointer_type, ctx.mesh_draw_base_push_constant,
                                           ctx.Const(ctx.mesh_draw_base_member_index))};
        workgroup = ctx.OpIAdd(ctx.U32[1], workgroup, ctx.OpLoad(ctx.U32[1], pointer));
    }
    if (ctx.stage == Stage::Task) {
        return workgroup; // the input region holds the workgroup index (VERTEX_ID)
    }
    const Id index{ctx.OpShiftRightLogical(ctx.U32[1], address, ctx.Const(2U))};
    const Id word{ctx.OpLoad(ctx.U32[1], PayloadWord(ctx, index))};
    return ctx.OpSelect(ctx.U32[1], ctx.OpIEqual(ctx.U1, address, ctx.u32_zero_value), workgroup,
                        word);
}

void EmitIsbeWriteAttribute(EmitContext& ctx, Id address, Id value) {
    const Id index{ctx.OpShiftRightLogical(ctx.U32[1], address, ctx.Const(2U))};
    if (ctx.stage == Stage::Task) {
        ctx.OpStore(PayloadWord(ctx, index), value);
        return;
    }
    ctx.OpStore(SharedWord(ctx, ctx.mesh_attributes, Clamped(ctx, index, ctx.mesh_attribute_words)),
                value);
}

void EmitIsbeWriteMapU8(EmitContext& ctx, Id address, Id value) {
    if (ctx.stage != Stage::Mesh) {
        throw NotImplementedException("Stage buffer map write outside mesh programs");
    }
    ctx.OpStore(SharedWord(ctx, ctx.mesh_map, Clamped(ctx, address, ctx.mesh_map_bytes)),
                ctx.OpBitwiseAnd(ctx.U32[1], value, ctx.Const(0xffU)));
}

void EmitIsbeWriteMapU32(EmitContext& ctx, const IR::Value& address, Id value) {
    // The primitive count: the map is written from its end, so its first word is at byte 3.
    if (ctx.stage != Stage::Mesh || !address.IsImmediate() || address.U32() != 3) {
        throw NotImplementedException("32-bit stage buffer map write");
    }
    ctx.OpStore(ctx.mesh_primitive_count, value);
}

void EmitMeshPrologue(EmitContext& ctx) {
    if (ctx.stage != Stage::Mesh) {
        return;
    }
    // Every invocation stores the zero: no branch here, the entry block's label is a phi
    // predecessor for later blocks.
    ctx.OpStore(ctx.mesh_primitive_count, ctx.u32_zero_value);
    Barrier(ctx);
}

void RecordMeshShuffle(EmitContext& ctx, Id value, Id source, Id result) {
    if (ctx.mesh_debug_shuffle_count >= DEBUG_SHUFFLES) {
        ++ctx.mesh_debug_shuffle_count;
        return;
    }
    DefineDebugShuffles(ctx);
    const u32 base{DEBUG_BALLOTS + ctx.mesh_debug_shuffle_count++ * 3};
    ctx.OpStore(ctx.mesh_debug_shuffles[base], value);
    ctx.OpStore(ctx.mesh_debug_shuffles[base + 1], source);
    ctx.OpStore(ctx.mesh_debug_shuffles[base + 2], result);
}

void RecordMeshBallot(EmitContext& ctx, Id result) {
    if (ctx.mesh_debug_ballot_count++ >= DEBUG_BALLOTS) {
        return;
    }
    DefineDebugShuffles(ctx);
    ctx.OpStore(ctx.mesh_debug_shuffles[ctx.mesh_debug_ballot_count - 1], result);
}

void EmitMeshEpilogue(EmitContext& ctx) {
    if (ctx.stage == Stage::Task) {
        TaskEpilogue(ctx);
    } else if (ctx.stage == Stage::Mesh) {
        MeshEpilogue(ctx);
    }
}

} // namespace Shader::Backend::SPIRV
