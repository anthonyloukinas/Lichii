// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <filesystem>
#include <functional>
#include <ios>
#include <iosfwd>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <unordered_map>
#include "common/container/unordered_map.h"
#include <vector>

#include "common/common_types.h"
#include "common/polyfill_thread.h"
#include "common/unique_function.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/ampere/instruction.h"
#include "video_core/engines/maxwell_3d.h"

namespace Tegra {
class Memorymanager;
}

namespace VideoCommon {

/// A native (Switch 2) program after capture and lowering to Maxwell code. The shader cache keeps
/// it so the environments built for the program's pipelines don't capture and lower it again.
struct LoweredNativeCode {
    u64 native_program_key{};
    /// Shader header, lowered instructions with Maxwell scheduling words, then the native
    /// address map (what the environment's `code` used to be filled with).
    std::vector<u64> code;
    std::vector<Shader::Ampere::Instruction> trap_prefix;
    /// Bytes of the native program that were read, header included.
    size_t source_size{};
    /// Constant buffer holding the texture handles if the program requires one, otherwise ~0U
    /// (the engine's register value is used).
    u32 texture_bound = ~0U;
};

class GenericEnvironment : public Shader::Environment {
public:
    explicit GenericEnvironment() = default;
    explicit GenericEnvironment(Tegra::MemoryManager& gpu_memory_, GPUVAddr program_base_,
                                u32 start_address_);

    ~GenericEnvironment() override;

    [[nodiscard]] u32 TextureBoundBuffer() const final;

    [[nodiscard]] u32 LocalMemorySize() const final;

    [[nodiscard]] u32 SharedMemorySize() const final;

    [[nodiscard]] std::array<u32, 3> WorkgroupSize() const final;

    [[nodiscard]] u64 ReadInstruction(u32 address) final;

    [[nodiscard]] u32 TranslateProgramAddress(u32 address) const override;

    [[nodiscard]] bool IsNativeProgram() const override;

    [[nodiscard]] bool IsCbufStorageBufferPointer(u32 index, u32 offset) override;

    [[nodiscard]] bool IsCbufIndirectStorageBufferPointer(u32 index, u32 offset,
                                                          u32 indirect) override;

    [[nodiscard]] std::vector<std::pair<u32, u32>> NullStoragePointerSlots() const override;

    [[nodiscard]] std::vector<u64> MappedStoragePointerKeys() const override;

    /// Answers the classification of these keys (see MappedStoragePointerKeys) as storage buffer
    /// pointers, whatever the slots hold now; called before the program is translated
    void KeepStoragePointers(std::span<const u64> keys);

    [[nodiscard]] std::optional<u64> Analyze();

    void SetCachedSize(size_t size_bytes);

    [[nodiscard]] size_t CachedSizeWords() const noexcept;

    [[nodiscard]] size_t CachedSizeBytes() const noexcept;

    [[nodiscard]] size_t ReadSizeBytes() const noexcept;

    [[nodiscard]] size_t SourceSizeBytes() const noexcept {
        return native_source_size;
    }

    [[nodiscard]] const std::shared_ptr<const LoweredNativeCode>& LoweredNative() const noexcept {
        return lowered_native;
    }

    [[nodiscard]] bool CanBeSerialized() const noexcept;

    /// Why the environment cannot be written to the disk cache, or nullptr.
    [[nodiscard]] const char* NotSerializableReason() const noexcept;

    [[nodiscard]] bool CanCacheTranslation() const override {
        return CanBeSerialized();
    }

    [[nodiscard]] u64 CalculateHash() const;

    void Dump(u64 pipeline_hash, u64 shader_hash) override;

    void Serialize(std::ofstream& file) const;

    bool HasHLEMacroState() const override {
        return has_hle_engine_state;
    }
    bool HasNativeInstructions() const override;

protected:
    std::optional<u64> TryFindSize();
    virtual std::optional<u32> ReadTrapGuardConstant(u32, u32) { return std::nullopt; }

    /// Adopts an already lowered native program, or captures and lowers it when there is none.
    void InitializeNativeCode(std::shared_ptr<const LoweredNativeCode> lowered,
                              u32 source_header_size = 128, bool geometry_stage = false);
    std::shared_ptr<const LoweredNativeCode> LowerNativeCode(u32 source_header_size,
                                                          bool geometry_stage);
    size_t native_source_size{};
    std::shared_ptr<const LoweredNativeCode> lowered_native;

    Tegra::Texture::TICEntry ReadTextureInfo(GPUVAddr tic_addr, u32 tic_limit,
                                             bool via_header_index, u32 raw);

    Tegra::MemoryManager* gpu_memory{};
    GPUVAddr program_base{};

    std::vector<u64> code;
    ::Common::unordered_map<u32, Shader::TextureType> texture_types;
    ::Common::unordered_map<u32, Shader::TexturePixelFormat> texture_pixel_formats;
    ::Common::unordered_map<u64, u32> cbuf_values;
    ::Common::unordered_map<u64, u8> storage_pointer_types; ///< IsCbufStorageBufferPointer answers
    ::Common::unordered_map<u64, Shader::ReplaceConstant> cbuf_replacements;

    u32 local_memory_size{};
    u32 texture_bound{};
    u32 shared_memory_size{};
    std::array<u32, 3> workgroup_size{};

    u32 read_lowest = (std::numeric_limits<u32>::max)();
    u32 read_highest = 0;

    u32 cached_lowest = (std::numeric_limits<u32>::max)();
    u32 cached_highest = 0;
    u32 initial_offset = 0;

    u32 viewport_transform_state = 1;

    bool has_unbound_instructions = false;
    bool has_hle_engine_state = false;
};

class GraphicsEnvironment final : public GenericEnvironment {
public:
    explicit GraphicsEnvironment() = default;
    explicit GraphicsEnvironment(Tegra::Engines::Maxwell3D& maxwell3d_,
                                 Tegra::MemoryManager& gpu_memory_,
                                 Tegra::Engines::Maxwell3D::Regs::ShaderType program,
                                 GPUVAddr program_base_, u32 start_address_,
                                 std::shared_ptr<const LoweredNativeCode> lowered_native_ = {});

    ~GraphicsEnvironment() override = default;

    u32 ReadCbufValue(u32 cbuf_index, u32 cbuf_offset) override;

    Shader::TextureType ReadTextureType(u32 handle) override;

    Shader::TexturePixelFormat ReadTexturePixelFormat(u32 handle) override;

    bool IsTexturePixelFormatInteger(u32 handle) override;

    u32 ReadViewportTransformState() override;

    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32 bank, u32 offset) override;

private:
    Tegra::Engines::Maxwell3D* maxwell3d{};
    size_t stage_index{};
};

class ComputeEnvironment final : public GenericEnvironment {
public:
    explicit ComputeEnvironment() = default;
    explicit ComputeEnvironment(Tegra::Engines::KeplerCompute& kepler_compute_,
                                Tegra::MemoryManager& gpu_memory_, GPUVAddr program_base_,
                                u32 start_address_,
                                std::shared_ptr<const LoweredNativeCode> lowered_native_ = {});

    ~ComputeEnvironment() override = default;

    u32 ReadCbufValue(u32 cbuf_index, u32 cbuf_offset) override;

    Shader::TextureType ReadTextureType(u32 handle) override;

    Shader::TexturePixelFormat ReadTexturePixelFormat(u32 handle) override;

    bool IsTexturePixelFormatInteger(u32 handle) override;

    u32 ReadViewportTransformState() override;

    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(
        [[maybe_unused]] u32 bank, [[maybe_unused]] u32 offset) override {
        return std::nullopt;
    }

private:
    std::optional<u32> ReadTrapGuardConstant(u32 bank, u32 offset) override;
    Tegra::Engines::KeplerCompute* kepler_compute{};
};

std::optional<u32> ReadNativeGuardConstant(Tegra::Engines::KeplerCompute& compute,
                                         Tegra::MemoryManager& memory, u32 bank, u32 offset);

class FileEnvironment final : public Shader::Environment {
public:
    FileEnvironment() = default;
    ~FileEnvironment() override = default;

    FileEnvironment& operator=(FileEnvironment&&) noexcept = default;
    FileEnvironment(FileEnvironment&&) noexcept = default;

    FileEnvironment& operator=(const FileEnvironment&) = delete;
    FileEnvironment(const FileEnvironment&) = delete;

    void Deserialize(std::ifstream& file);

    /// Moves past one environment in the layout Deserialize reads and returns its stage, or
    /// nullopt when it would end past `end`.
    static std::optional<Shader::Stage> Skip(std::ifstream& file, std::streamoff end);

    [[nodiscard]] size_t MemorySize() const;

    [[nodiscard]] u64 ReadInstruction(u32 address) override;

    [[nodiscard]] u32 TranslateProgramAddress(u32 address) const override;

    [[nodiscard]] bool IsNativeProgram() const override;

    [[nodiscard]] bool IsCbufStorageBufferPointer(u32 index, u32 offset) override;

    [[nodiscard]] bool IsCbufIndirectStorageBufferPointer(u32 index, u32 offset,
                                                          u32 indirect) override;

    [[nodiscard]] std::vector<std::pair<u32, u32>> NullStoragePointerSlots() const override;

    [[nodiscard]] std::vector<u64> MappedStoragePointerKeys() const override;

    [[nodiscard]] u32 ReadCbufValue(u32 cbuf_index, u32 cbuf_offset) override;

    [[nodiscard]] Shader::TextureType ReadTextureType(u32 handle) override;

    [[nodiscard]] Shader::TexturePixelFormat ReadTexturePixelFormat(u32 handle) override;

    [[nodiscard]] bool IsTexturePixelFormatInteger(u32 handle) override;

    [[nodiscard]] u32 ReadViewportTransformState() override;

    [[nodiscard]] u32 LocalMemorySize() const override;

    [[nodiscard]] u32 SharedMemorySize() const override;

    [[nodiscard]] u32 TextureBoundBuffer() const override;

    [[nodiscard]] std::array<u32, 3> WorkgroupSize() const override;

    [[nodiscard]] std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32 bank,
                                                                               u32 offset) override;

    [[nodiscard]] bool HasHLEMacroState() const override {
        return cbuf_replacements.size() != 0;
    }
    bool HasNativeInstructions() const override;

    void Dump(u64 pipeline_hash, u64 shader_hash) override;

private:
    std::vector<u64> code;
    ::Common::unordered_map<u32, Shader::TextureType> texture_types;
    ::Common::unordered_map<u32, Shader::TexturePixelFormat> texture_pixel_formats;
    ::Common::unordered_map<u64, u32> cbuf_values;
    ::Common::unordered_map<u64, u8> storage_pointer_types;
    ::Common::unordered_map<u64, Shader::ReplaceConstant> cbuf_replacements;
    std::array<u32, 3> workgroup_size{};
    u32 local_memory_size{};
    u32 shared_memory_size{};
    u32 texture_bound{};
    u32 read_lowest{};
    u32 read_highest{};
    u32 initial_offset{};
    u32 viewport_transform_state = 1;
};

/// Appends a record. True only once it is complete on disk (flushed and closed without error).
/// False when nothing was written: an environment cannot be stored, or the write failed (the file
/// is then cut back to its earlier records).
bool SerializePipeline(std::span<const char> key, std::span<const GenericEnvironment* const> envs,
                       const std::filesystem::path& filename, u32 cache_version);

template <typename Key, typename Envs>
    requires std::is_trivially_copyable_v<Key>
        && std::has_unique_object_representations_v<Key>
bool SerializePipeline(const Key& key, const Envs& envs, const std::filesystem::path& filename, u32 cache_version) {
    return SerializePipeline(std::span(reinterpret_cast<const char*>(&key), sizeof(key)), std::span(envs.data(), envs.size()), filename, cache_version);
}

/// Result of AppendCheckedPipeline.
enum class CheckedWrite {
    Written,         ///< The record was appended.
    NotSerializable, ///< An environment cannot be stored; nothing was written.
    Rejected,        ///< check refused the capture as read back; nothing was appended.
    Failed,          ///< Writing or reading back failed; the cache keeps its earlier records.
};

/// Appends a record like SerializePipeline, but only after reading it back from a scratch file
/// beside the cache and passing the environments as a later boot would load them to check.
CheckedWrite AppendCheckedPipeline(std::span<const char> key,
                                   std::span<const GenericEnvironment* const> envs,
                                   const std::filesystem::path& filename, u32 cache_version,
                                   Common::UniqueFunction<bool, std::span<FileEnvironment>> check);

template <typename Key, typename Envs>
    requires std::is_trivially_copyable_v<Key> && std::has_unique_object_representations_v<Key>
CheckedWrite AppendCheckedPipeline(const Key& key, const Envs& envs,
                                   const std::filesystem::path& filename, u32 cache_version,
                                   Common::UniqueFunction<bool, std::span<FileEnvironment>> check) {
    return AppendCheckedPipeline(std::span(reinterpret_cast<const char*>(&key), sizeof(key)),
                                 std::span(envs.data(), envs.size()), filename, cache_version,
                                 std::move(check));
}

/// Newest-record-wins loading. The pipeline cache is append-only: a capture written again after
/// the stored one failed to build follows it in the file, so of the records with the same key
/// only the last one is loaded.
struct PipelineRecordIndex {
    /// The part of a stored key that decides equality (graphics keys compare only their first
    /// Size() bytes, compute keys all of them), and the hash it is looked up by. Records are told
    /// apart by those bytes; equal hashes alone never merge two keys.
    struct Identity {
        u64 hash;
        size_t size;
    };
    struct RecordKey {
        u64 hash;
        std::string bytes;
        bool operator==(const RecordKey&) const = default;
    };
    struct RecordKeyHash {
        size_t operator()(const RecordKey& key) const noexcept {
            return static_cast<size_t>(key.hash);
        }
    };

    size_t compute_key_size{};
    size_t graphics_key_size{};
    /// compute is true for the key of a compute record.
    std::function<Identity(std::span<const char> key, bool compute)> identify;
    /// Optional: rewrites a graphics key to the form the loader uses before it is identified, so
    /// the newest record is chosen among the records the loader treats as one key. mesh is true
    /// when the record holds a task or mesh program (from its stored stages).
    std::function<void(std::span<char> key, bool mesh)> canonical_graphics_key;

    /// Filled by LoadPipelines: the records stored per key, and how many older records it
    /// skipped for a newer one.
    std::unordered_map<RecordKey, u32, RecordKeyHash> compute_records;
    std::unordered_map<RecordKey, u32, RecordKeyHash> graphics_records;
    size_t superseded{};

    [[nodiscard]] RecordKey Key(std::span<const char> key, bool compute) const {
        const Identity identity = identify(key, compute);
        return {identity.hash, std::string(key.data(), std::min(identity.size, key.size()))};
    }

    /// The key as identified (canonical_graphics_key applied to a graphics key).
    [[nodiscard]] RecordKey CanonicalKey(std::span<const char> key, bool compute,
                                         bool mesh) const {
        if (compute || !canonical_graphics_key) {
            return Key(key, compute);
        }
        std::string bytes(key.data(), key.size());
        canonical_graphics_key(std::span<char>{bytes.data(), bytes.size()}, mesh);
        return Key(std::span<const char>{bytes.data(), bytes.size()}, compute);
    }

    [[nodiscard]] u32 Records(std::span<const char> key, bool compute, bool mesh = false) const {
        const auto& records = compute ? compute_records : graphics_records;
        const auto it = records.find(CanonicalKey(key, compute, mesh));
        return it == records.end() ? 1 : it->second;
    }
};

/// With an index, a record whose key is stored again later in the file is skipped.
void LoadPipelines(std::stop_token stop_loading, const std::filesystem::path& filename, u32 expected_cache_version,
    Common::UniqueFunction<void, std::ifstream&, FileEnvironment> load_compute,
    Common::UniqueFunction<void, std::ifstream&, std::vector<FileEnvironment>> load_graphics,
    PipelineRecordIndex* index = nullptr);

} // namespace VideoCommon
