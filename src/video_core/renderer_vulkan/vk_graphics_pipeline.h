// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <optional>
#include <span>
#include <type_traits>
#include <vector>

#include "common/thread_worker.h"
#include "shader_recompiler/shader_info.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/nx2_fast_draw.h"
#include "video_core/renderer_vulkan/fixed_pipeline_state.h"
#include "video_core/renderer_vulkan/pipeline_build.h"
#include "video_core/renderer_vulkan/pipeline_helper.h"
#include "video_core/renderer_vulkan/vk_buffer_cache.h"
#include "video_core/renderer_vulkan/vk_descriptor_buffer.h"
#include "video_core/renderer_vulkan/vk_descriptor_pool.h"
#include "video_core/renderer_vulkan/vk_texture_cache.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace VideoCore {
class ShaderNotify;
}

namespace Vulkan {

struct GraphicsPipelineCacheKey {
    std::array<u64, 6> unique_hashes;
    FixedPipelineState state;

    size_t Hash() const noexcept;

    bool operator==(const GraphicsPipelineCacheKey& rhs) const noexcept;

    bool operator!=(const GraphicsPipelineCacheKey& rhs) const noexcept {
        return !operator==(rhs);
    }

    size_t Size() const noexcept {
        return sizeof(unique_hashes) + state.Size();
    }
};
static_assert(std::has_unique_object_representations_v<GraphicsPipelineCacheKey>);
static_assert(std::is_trivially_copyable_v<GraphicsPipelineCacheKey>);
static_assert(std::is_trivially_constructible_v<GraphicsPipelineCacheKey>);

} // namespace Vulkan

namespace std {
template <>
struct hash<Vulkan::GraphicsPipelineCacheKey> {
    size_t operator()(const Vulkan::GraphicsPipelineCacheKey& k) const noexcept {
        return k.Hash();
    }
};
} // namespace std

namespace Vulkan {

class Device;
class PipelineStatistics;
class RenderPassCache;
class RescalingPushConstant;
class RenderAreaPushConstant;
class Scheduler;

class GraphicsPipeline {
    static constexpr size_t NUM_STAGES = Tegra::Engines::Maxwell3D::Regs::MaxShaderStage;

public:
    explicit GraphicsPipeline(
        Scheduler& scheduler, BufferCache& buffer_cache, TextureCache& texture_cache,
        vk::PipelineCache& pipeline_cache, VideoCore::ShaderNotify* shader_notify,
        const Device& device, DescriptorPool& descriptor_pool,
        GuestDescriptorQueue& guest_descriptor_queue,
        DescriptorBufferRing& descriptor_buffer_ring, Common::ThreadWorker* worker_thread,
        PipelineStatistics* pipeline_statistics, RenderPassCache& render_pass_cache,
        const GraphicsPipelineCacheKey& key, std::array<vk::ShaderModule, NUM_STAGES> stages,
        const std::array<const Shader::Info*, NUM_STAGES>& infos,
        std::function<std::array<vk::ShaderModule, NUM_STAGES>()> emit_stages = {});

    static std::chrono::nanoseconds TakeHoldTime() noexcept;

    bool HasDynamicVertexInput() const noexcept { return key.state.dynamic_vertex_input; }
    const std::array<u64, 6>& ShaderHashes() const noexcept {
        return key.unique_hashes;
    }

    struct SampledImage {
        u32 stage;
        u32 tic;
        VideoCommon::ImageViewId id;
        VideoCommon::SamplerId sampler;
    };

    /// The sampled textures of the last configured draw, kept while a sampled frame dump runs.
    std::span<const SampledImage> SampledImages() const noexcept {
        return sampled_images;
    }

    bool SupportsAlphaToCoverage() const noexcept {
        return fragment_has_color0_output;
    }

    bool SupportsAlphaToOne() const noexcept {
        return fragment_has_color0_output;
    }

    /// Texture handles of the last configured draw whose handle program gave 0.
    u32 ZeroTextureHandles() const noexcept {
        return zero_texture_handles;
    }

    bool FragmentCanDiscard() const noexcept {
        return stage_infos[NUM_STAGES - 1].uses_demote_to_helper_invocation;
    }

    bool UsesExtendedDynamicState() const noexcept {
        return key.state.extended_dynamic_state != 0;
    }

    /// Switch 2 task/mesh pipeline: drawn with vkCmdDrawMeshTasksEXT.
    [[nodiscard]] bool IsMeshPipeline() const noexcept {
        return stage_infos[0].is_task || stage_infos[0].is_mesh;
    }
    GraphicsPipeline& operator=(GraphicsPipeline&&) noexcept = delete;
    GraphicsPipeline(GraphicsPipeline&&) noexcept = delete;

    GraphicsPipeline& operator=(const GraphicsPipeline&) = delete;
    GraphicsPipeline(const GraphicsPipeline&) = delete;

    void AddTransition(GraphicsPipeline* transition);

    bool Configure(bool is_indexed) {
        return configure_func(this, is_indexed);
    }

    [[nodiscard]] GraphicsPipeline* Next(const GraphicsPipelineCacheKey& current_key) noexcept {
        if (key == current_key) {
            return this;
        }
        const auto it{std::find(transition_keys.begin(), transition_keys.end(), current_key)};
        return it != transition_keys.end() ? transitions[std::distance(transition_keys.begin(), it)]
                                           : nullptr;
    }

    /// The frame the pipeline cache last handed this pipeline out (evicts long unused ones).
    void MarkUsed(u64 frame) noexcept {
        last_use_frame = frame;
    }

    [[nodiscard]] u64 LastUse() const noexcept {
        return last_use_frame;
    }

    /// Forgets the transitions, which may point to pipelines the cache evicted.
    void ClearTransitions() noexcept {
        transition_keys.clear();
        transitions.clear();
    }

    [[nodiscard]] bool WritesStorage() const noexcept {
        return writes_storage;
    }

    /// Writes storage images or image buffers (their images are marked modified when resolved).
    [[nodiscard]] bool WritesImages() const noexcept {
        return writes_images;
    }

    [[nodiscard]] bool IsBuilt() const noexcept {
        return build->IsBuilt();
    }

    [[nodiscard]] bool Failed() const noexcept {
        return build->Failed();
    }

    /// EDEN_NX2_FAST_DRAW=2 (nx2_fast_draw.h): whether draws can keep this pipeline's resolved
    /// image views and samplers after light changes. Handles computed at draw time are resolved
    /// on every draw.
    [[nodiscard]] bool TakesDeltaDraws() const noexcept {
        return DeltaCapable() && delta_views_valid;
    }

    /// EDEN_NX2_FAST_DRAW_COMPUTED: delta draws of this pipeline must recheck its handles.
    [[nodiscard]] bool HasComputedHandles() const noexcept {
        return has_computed_handles;
    }

    [[nodiscard]] bool HasHandles() const noexcept {
        return num_image_elements != 0;
    }

    /// With fast draws: whether every texture and image handle reads as it did at the last full
    /// Configure (compares only).
    [[nodiscard]] bool HandlesUnchanged();

    /// With fast draws: the raw handles the last full Configure read, in its order.
    [[nodiscard]] std::span<const u32> RawHandles() const noexcept {
        return raw_handles;
    }

    /// Why TakesDeltaDraws is false (diagnostics).
    enum class DeltaBlocker : u8 {
        None,
        WritesImages,
        Mesh,
        ComputedHandles,
        NotConfigured,
        Count,
    };
    [[nodiscard]] DeltaBlocker DeltaDrawBlocker() const noexcept {
        return !DeltaCapable()    ? delta_blocker
               : delta_views_valid ? DeltaBlocker::None
                                   : DeltaBlocker::NotConfigured;
    }

    /// Constant buffer slots per stage the pipeline reads texture and image handles from.
    /// With EDEN_NX2_FAST_DRAW_COMPUTED, the slots of handles computed from storage buffers are
    /// left out (delta draws recheck those).
    [[nodiscard]] const std::array<u32, NUM_STAGES>& HandleConstantBuffers() const noexcept {
        return VideoCommon::Nx2FastDraw::g_computed_delta ? static_handle_constant_buffers
                                                          : handle_constant_buffers;
    }

    enum class DeltaResult { Configured, Failed, NeedsFull, HandlesChanged };
    /// Binds the constant and storage buffers again (storage buffer addresses are read from
    /// constant buffers) and records the draw's descriptors with the image views, samplers and
    /// texture buffers the last full Configure resolved. recheck_handles: RecheckHandles first.
    DeltaResult ConfigureDelta(bool recheck_handles);

    enum class RecheckResult : u8 { Kept, Resolved, BufferHandleChanged, Count };
    /// Reads the handles again and resolves the views and samplers of the changed ones into the
    /// lists delta draws use. BufferHandleChanged: a texture or image buffer handle changed (the
    /// draw must be resolved in full; the lists are left invalid).
    RecheckResult RecheckHandles();
    /// The result of the last delta draw's RecheckHandles.
    [[nodiscard]] RecheckResult LastRecheck() const noexcept {
        return last_recheck;
    }

    /// EDEN_NX2_FAST_DRAW_VERIFY: the next Configure compares what it resolves with the views
    /// and samplers a delta draw would have kept; DeltaMismatch reports the result.
    void VerifyNextDelta() noexcept {
        verify_delta = true;
    }
    [[nodiscard]] bool DeltaMismatch() const noexcept {
        return delta_mismatch;
    }

    template <typename Spec>
    static auto MakeConfigureSpecFunc() {
        return [](GraphicsPipeline* pl, bool is_indexed) { return pl->ConfigureImpl<Spec>(is_indexed); };
    }

    void SetEngine(Tegra::Engines::Maxwell3D* maxwell3d_, Tegra::MemoryManager* gpu_memory_) {
        maxwell3d = maxwell3d_;
        gpu_memory = gpu_memory_;
    }

private:
    template <typename Spec>
    bool ConfigureImpl(bool is_indexed);

    [[nodiscard]] bool DeltaCapable() const noexcept {
        return delta_blocker == DeltaBlocker::None ||
               (delta_blocker == DeltaBlocker::ComputedHandles &&
                VideoCommon::Nx2FastDraw::g_computed_delta);
    }

    /// The raw handle of element index of desc (stage's constant buffer words, or its handle
    /// program).
    template <typename Descriptor>
    u32 ReadRawHandle(size_t stage, const Descriptor& desc, u32 index);

    /// Binds stage's native_indirect storage buffers at first_index on (task and mesh programs)
    void BindNativeIndirect(size_t stage, size_t first_index);

    bool ConfigureDraw(const RescalingPushConstant& rescaling,
                       const RenderAreaPushConstant& render_are);

    void MakePipeline(VkRenderPass render_pass);

    void Validate();

    const GraphicsPipelineCacheKey key;
    Tegra::Engines::Maxwell3D* maxwell3d;
    Tegra::MemoryManager* gpu_memory;
    const Device& device;
    TextureCache& texture_cache;
    BufferCache& buffer_cache;
    vk::PipelineCache& pipeline_cache;
    Scheduler& scheduler;
    GuestDescriptorQueue& guest_descriptor_queue;
    DescriptorBufferRing& descriptor_buffer_ring;

    bool (*configure_func)(GraphicsPipeline*, bool){};

    std::vector<GraphicsPipelineCacheKey> transition_keys;
    std::vector<GraphicsPipeline*> transitions;
    u64 last_use_frame{};

    std::array<vk::ShaderModule, NUM_STAGES> spv_modules;

    std::array<Shader::Info, NUM_STAGES> stage_infos;
    std::array<u32, 5> enabled_uniform_buffer_masks{};
    VideoCommon::UniformBufferSizes uniform_buffer_sizes{};
    u32 num_descriptor_entries{};
    size_t num_image_elements{};
    std::vector<SampledImage> sampled_images;
    u32 num_textures{};
    bool fragment_has_color0_output{};
    u32 zero_texture_handles{};
    bool writes_storage{};
    bool writes_images{};
    /// Task and mesh programs' read-only storage buffers for pointers loaded from records: how
    /// many each stage has (after its other storage buffers), and the base addresses the draw
    /// pushes at NATIVE_INDIRECT_BASES_OFFSET (task's, then mesh's: NativeIndirectBasesFirst).
    std::array<u32, NUM_STAGES> num_native_indirect{};
    bool uses_native_indirect{};
    std::array<u64, Shader::Backend::SPIRV::NATIVE_INDIRECT_BASES_SIZE / sizeof(u64)>
        native_indirect_bases{};

    DeltaBlocker delta_blocker{};
    bool delta_views_valid{};
    bool verify_delta{};
    bool delta_mismatch{};
    std::array<bool, NUM_STAGES> configured_stages{};
    std::array<u32, NUM_STAGES> handle_constant_buffers{};
    std::array<u32, NUM_STAGES> static_handle_constant_buffers{};
    bool has_computed_handles{};
    std::vector<VideoCommon::ImageViewInOut> delta_views;
    std::vector<VideoCommon::SamplerId> delta_samplers;
    /// The raw handle of each view element at the last full Configure (with fast draws); kept in
    /// step with delta_views by RecheckHandles.
    std::vector<u32> raw_handles;
    RecheckResult last_recheck{};

    vk::DescriptorSetLayout descriptor_set_layout;
    DescriptorAllocator descriptor_allocator;
    vk::PipelineLayout pipeline_layout;
    vk::DescriptorUpdateTemplate descriptor_update_template;
    vk::Pipeline pipeline;

    DescriptorBufferLayout descriptor_buffer_layout;
    std::vector<DescriptorUpdateEntry> last_descriptor_payload;
    VkDeviceSize last_descriptor_buffer_offset{};
    u32 last_descriptor_buffer_chunk{};
    u64 last_descriptor_buffer_generation{};

    const bool worker_only_build;
    bool uses_push_descriptor{false};
    bool uses_descriptor_buffer{false};
    std::optional<PipelineBuild> build;
};

} // namespace Vulkan
