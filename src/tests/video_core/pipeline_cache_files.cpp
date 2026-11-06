// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/exception.h"
#include "video_core/pipeline_cache_files.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/shader_environment.h"

namespace {

constexpr std::array<char, 8> Magic{'y', 'u', 'z', 'u', 'c', 'a', 'c', 'h'};
constexpr std::array<char, 8> OtherMagic{'y', 'u', 'z', 'u', 'v', 'k', 'c', 'h'};

struct TempDir {
    std::filesystem::path path;
    TempDir() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() /
               ("eden-pipeline-cache-files-" + std::to_string(stamp));
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

void WriteCache(const std::filesystem::path& file, const std::array<char, 8>& magic, u32 version,
                std::string_view body) {
    std::ofstream stream(file, std::ios::binary);
    stream.write(magic.data(), magic.size());
    stream.write(reinterpret_cast<const char*>(&version), sizeof(version));
    stream.write(body.data(), static_cast<std::streamsize>(body.size()));
}

std::string ReadAll(const std::filesystem::path& file) {
    std::ifstream stream(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

class SerializedEnvironment final : public VideoCommon::GenericEnvironment {
public:
    explicit SerializedEnvironment(Shader::Stage shader_stage, u32 cbuf_value = 0x1357,
                                   bool classifies_pointer = false) {
        stage = shader_stage;
        code = {0x123456789abcdef0, 0xfedcba9876543210};
        cached_lowest = 0;
        cached_highest = sizeof(u64);
        mesh_registers = {0x12345678, 0x87654321, 0xabcdef01};
        std::memset(&mesh_primitive_header, 0x5a, sizeof(mesh_primitive_header));
        gp_passthrough_mask.fill(0xa5a5a5a5);
        texture_bound = 2;
        cbuf_values.emplace((u64{1} << 32) | 16, cbuf_value);
        texture_types.emplace(3, Shader::TextureType::Color2D);
        texture_pixel_formats.emplace(4, Shader::TexturePixelFormat{});
        cbuf_replacements.emplace(5, Shader::ReplaceConstant{});
        if (classifies_pointer) {
            // The answer to IsCbufStorageBufferPointer(1, 16) a complete capture holds.
            storage_pointer_types.emplace((u64{1} << 32) | 16, 1);
        }
    }

    void MakeUnserializable() {
        has_unbound_instructions = true;
    }

    u32 ReadCbufValue(u32, u32) override {
        return 0;
    }
    Shader::TextureType ReadTextureType(u32) override {
        return Shader::TextureType::Color2D;
    }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override {
        return {};
    }
    bool IsTexturePixelFormatInteger(u32) override {
        return false;
    }
    u32 ReadViewportTransformState() override {
        return 1;
    }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
};

} // Anonymous namespace

TEST_CASE("Task and mesh disk environments roundtrip all translation state and record boundaries",
          "[pipeline_cache_files][mesh_serialization]") {
    TempDir temp;
    const auto path = temp.path / "mesh.bin";
    constexpr u64 key = 0xabcdef;
    for (const auto stage : {Shader::Stage::Task, Shader::Stage::Mesh, Shader::Stage::Fragment}) {
        SerializedEnvironment source{stage};
        const std::array<const VideoCommon::GenericEnvironment*, 1> envs{&source};
        VideoCommon::SerializePipeline(key, envs, path, VideoCommon::VulkanPipelineCacheVersion);
    }
    size_t read = 0;
    VideoCommon::LoadPipelines(
        {}, path, VideoCommon::VulkanPipelineCacheVersion,
        [](std::ifstream&, VideoCommon::FileEnvironment) { FAIL("Unexpected compute record"); },
        [&](std::ifstream& file, std::vector<VideoCommon::FileEnvironment> envs) {
            const std::array stages{Shader::Stage::Task, Shader::Stage::Mesh,
                                    Shader::Stage::Fragment};
            REQUIRE(read < stages.size());
            REQUIRE(envs.size() == 1);
            auto& restored = envs.front();
            SerializedEnvironment source{stages[read++]};
            REQUIRE(restored.ShaderStage() == source.ShaderStage());
            REQUIRE(restored.ReadInstruction(0) == 0x123456789abcdef0);
            REQUIRE(restored.ReadInstruction(8) == 0xfedcba9876543210);
            REQUIRE(restored.ReadCbufValue(1, 16) == 0x1357);
            REQUIRE(restored.ReadTextureType(3) == Shader::TextureType::Color2D);
            REQUIRE(restored.TextureBoundBuffer() == 2);
            REQUIRE(restored.MemorySize() > sizeof(restored) + 2 * sizeof(u64));
            if (source.ShaderStage() != Shader::Stage::Fragment) {
                REQUIRE(restored.MeshRegisters() == source.MeshRegisters());
                REQUIRE(std::memcmp(&restored.MeshPrimitiveHeader(), &source.MeshPrimitiveHeader(),
                                    sizeof(Shader::ProgramHeader)) == 0);
            }
            u64 restored_key{};
            file.read(reinterpret_cast<char*>(&restored_key), sizeof(restored_key));
            REQUIRE(restored_key == key);
        });
    REQUIRE(read == 3);
}

TEST_CASE("Compatible v52 pipelines migrate without changing or discarding the old warm cache",
          "[pipeline_cache_files][mesh_serialization]") {
    TempDir temp;
    const auto old = VideoCommon::VersionedPipelineCachePath(temp.path, "vulkan", 52);
    const auto current = VideoCommon::VersionedPipelineCachePath(
        temp.path, "vulkan", VideoCommon::VulkanPipelineCacheVersion);
    SerializedEnvironment source{Shader::Stage::Fragment};
    const std::array<const VideoCommon::GenericEnvironment*, 1> envs{&source};
    constexpr u64 key = 42;
    VideoCommon::SerializePipeline(key, envs, old, 52);
    const auto before = ReadAll(old);
    REQUIRE(VideoCommon::CopyCompatiblePipelineCache(old, current, Magic, 52,
                                                     VideoCommon::VulkanPipelineCacheVersion));
    REQUIRE(ReadAll(old) == before);
    REQUIRE(ReadAll(current).substr(12) == before.substr(12));
    REQUIRE_FALSE(VideoCommon::CopyCompatiblePipelineCache(
        old, current, Magic, 52, VideoCommon::VulkanPipelineCacheVersion));
    bool loaded = false;
    VideoCommon::LoadPipelines(
        {}, current, VideoCommon::VulkanPipelineCacheVersion,
        [](std::ifstream&, VideoCommon::FileEnvironment) { FAIL("Unexpected compute record"); },
        [&](std::ifstream& file, std::vector<VideoCommon::FileEnvironment> restored) {
            REQUIRE(restored.size() == 1);
            REQUIRE(restored.front().ShaderStage() == Shader::Stage::Fragment);
            u64 restored_key{};
            file.read(reinterpret_cast<char*>(&restored_key), sizeof(restored_key));
            REQUIRE(restored_key == key);
            loaded = true;
        });
    REQUIRE(loaded);
    const auto other = temp.path / "wrong.bin";
    REQUIRE_FALSE(VideoCommon::CopyCompatiblePipelineCache(old, other, Magic, 51, 53));
    REQUIRE_FALSE(std::filesystem::exists(other));
}

TEST_CASE("Compatible pipeline cache migration retries after an interrupted temporary copy",
          "[pipeline_cache_files][mesh_serialization][migration_retry]") {
    TempDir temp;
    const auto old = VideoCommon::VersionedPipelineCachePath(temp.path, "vulkan", 52);
    const auto current = VideoCommon::VersionedPipelineCachePath(
        temp.path, "vulkan", VideoCommon::VulkanPipelineCacheVersion);
    auto temporary = current;
    temporary += ".migrating";
    WriteCache(old, Magic, 52, "complete pipeline records");
    const auto before = ReadAll(old);
    {
        std::ofstream stream(temporary, std::ios::binary);
        stream.write(before.data(), 4);
    }
    REQUIRE(ReadAll(temporary) == before.substr(0, 4));
    REQUIRE(VideoCommon::CopyCompatiblePipelineCache(old, current, Magic, 52,
                                                     VideoCommon::VulkanPipelineCacheVersion));
    REQUIRE(ReadAll(old) == before);
    REQUIRE(VideoCommon::PipelineCacheHeaderMatches(
        current, Magic, VideoCommon::VulkanPipelineCacheVersion));
    REQUIRE(ReadAll(current).substr(12) == before.substr(12));
    REQUIRE_FALSE(std::filesystem::exists(temporary));
    const auto migrated = ReadAll(current);
    REQUIRE_FALSE(VideoCommon::CopyCompatiblePipelineCache(
        old, current, Magic, 52, VideoCommon::VulkanPipelineCacheVersion));
    REQUIRE(ReadAll(current) == migrated);
    REQUIRE(ReadAll(old) == before);
}

TEST_CASE("Truncated mesh state is rejected before a pipeline can be queued",
          "[pipeline_cache_files][mesh_serialization]") {
    TempDir temp;
    const auto path = temp.path / "truncated-mesh.bin";
    SerializedEnvironment source{Shader::Stage::Mesh};
    const std::array<const VideoCommon::GenericEnvironment*, 1> envs{&source};
    constexpr u64 key = 42;
    VideoCommon::SerializePipeline(key, envs, path, VideoCommon::VulkanPipelineCacheVersion);
    std::filesystem::resize_file(path, std::filesystem::file_size(path) - sizeof(key) - 1);
    VideoCommon::LoadPipelines(
        {}, path, VideoCommon::VulkanPipelineCacheVersion,
        [](std::ifstream&, VideoCommon::FileEnvironment) { FAIL("Unexpected compute record"); },
        [](std::ifstream&, std::vector<VideoCommon::FileEnvironment>) {
            FAIL("Incomplete mesh state reached pipeline creation");
        });
    REQUIRE_FALSE(std::filesystem::exists(path));
}

TEST_CASE("Cancelled mesh loading preserves its disk cache and queues no work",
          "[pipeline_cache_files][mesh_serialization]") {
    TempDir temp;
    const auto path = temp.path / "cancelled-mesh.bin";
    SerializedEnvironment source{Shader::Stage::Mesh};
    const std::array<const VideoCommon::GenericEnvironment*, 1> envs{&source};
    constexpr u64 key = 42;
    VideoCommon::SerializePipeline(key, envs, path, VideoCommon::VulkanPipelineCacheVersion);
    const auto before = ReadAll(path);
    std::stop_source stop;
    stop.request_stop();
    VideoCommon::LoadPipelines(
        stop.get_token(), path, VideoCommon::VulkanPipelineCacheVersion,
        [](std::ifstream&, VideoCommon::FileEnvironment) { FAIL("Unexpected compute record"); },
        [](std::ifstream&, std::vector<VideoCommon::FileEnvironment>) {
            FAIL("Cancelled load reached pipeline creation");
        });
    REQUIRE(ReadAll(path) == before);
}

namespace {

constexpr u32 Version = VideoCommon::VulkanPipelineCacheVersion;

/// A graphics key of another size than the compute key (u64), as in the Vulkan cache.
struct GraphicsKey {
    u64 id;
    u64 state;
};

constexpr std::initializer_list<Shader::Stage> GraphicsStages{
    Shader::Stage::VertexB, Shader::Stage::Geometry, Shader::Stage::Fragment};

/// The environments of one pipeline as a live build captures them.
struct Capture {
    std::vector<std::unique_ptr<SerializedEnvironment>> owned;
    std::vector<const VideoCommon::GenericEnvironment*> envs;

    Capture(bool compute, u32 value, bool complete, std::initializer_list<Shader::Stage> stages) {
        if (compute) {
            owned.push_back(
                std::make_unique<SerializedEnvironment>(Shader::Stage::Compute, value, complete));
        }
        for (const auto stage : stages) {
            owned.push_back(std::make_unique<SerializedEnvironment>(stage, value, complete));
        }
        for (const auto& env : owned) {
            envs.push_back(env.get());
        }
    }
};

void WriteRecord(const std::filesystem::path& path, u64 key, bool compute, u32 value,
                 bool complete, std::initializer_list<Shader::Stage> stages = {}) {
    const Capture capture{compute, value, complete, stages};
    if (compute) {
        VideoCommon::SerializePipeline(key, capture.envs, path, Version);
    } else {
        VideoCommon::SerializePipeline(GraphicsKey{key, ~key}, capture.envs, path, Version);
    }
}

bool TranslatesAll(std::span<VideoCommon::FileEnvironment> envs);

/// What the Vulkan cache does with a replacement: append it only if it builds as read back.
VideoCommon::CheckedWrite AppendChecked(const std::filesystem::path& path, u64 key, bool compute,
                                        const Capture& capture) {
    if (compute) {
        return VideoCommon::AppendCheckedPipeline(key, capture.envs, path, Version,
                                                  &TranslatesAll);
    }
    return VideoCommon::AppendCheckedPipeline(GraphicsKey{key, ~key}, capture.envs, path, Version,
                                              &TranslatesAll);
}

void WriteCompute(const std::filesystem::path& path, u64 key, u32 value, bool complete = true) {
    WriteRecord(path, key, true, value, complete);
}

void WriteGraphics(const std::filesystem::path& path, u64 key, u32 value, bool complete = true) {
    WriteRecord(path, key, false, value, complete,
                {Shader::Stage::VertexB, Shader::Stage::Geometry, Shader::Stage::Fragment});
}

/// colliding: every key hashes the same, so only the key bytes tell records apart.
VideoCommon::PipelineRecordIndex MakeIndex(bool colliding = false) {
    return {
        .compute_key_size = sizeof(u64),
        .graphics_key_size = sizeof(GraphicsKey),
        .identify = [colliding](std::span<const char> bytes,
                                bool) -> VideoCommon::PipelineRecordIndex::Identity {
            u64 id;
            std::memcpy(&id, bytes.data(), sizeof(id));
            return {colliding ? 0x5eed : id, bytes.size()};
        },
    };
}

u32 Records(const VideoCommon::PipelineRecordIndex& index, u64 id, bool compute) {
    const GraphicsKey graphics{id, ~id};
    return compute ? index.Records({reinterpret_cast<const char*>(&id), sizeof(id)}, true)
                   : index.Records({reinterpret_cast<const char*>(&graphics), sizeof(graphics)},
                                   false);
}

/// A lookup hash under which all keys collide.
struct SameHash {
    size_t operator()(u64) const noexcept {
        return 0x5eed;
    }
};

/// What a translation of the capture needs: the cbuf value it reads and the pointer
/// classification, which an incomplete capture lacks (FileEnvironment throws, as at boot).
bool Translates(VideoCommon::FileEnvironment& env) {
    try {
        static_cast<void>(env.ReadCbufValue(1, 16));
        static_cast<void>(env.IsCbufStorageBufferPointer(1, 16));
        return true;
    } catch (const Shader::Exception&) {
        return false;
    }
}

bool TranslatesAll(std::span<VideoCommon::FileEnvironment> envs) {
    return std::ranges::all_of(envs, Translates);
}

struct Loaded {
    bool compute;
    u64 key;
    u32 value;
    bool translates;
};

std::vector<Loaded> Load(const std::filesystem::path& path,
                         VideoCommon::PipelineRecordIndex* index) {
    std::vector<Loaded> loaded;
    VideoCommon::LoadPipelines(
        {}, path, Version,
        [&](std::ifstream& file, VideoCommon::FileEnvironment env) {
            u64 key{};
            file.read(reinterpret_cast<char*>(&key), sizeof(key));
            REQUIRE(env.ShaderStage() == Shader::Stage::Compute);
            loaded.push_back({true, key, env.ReadCbufValue(1, 16), Translates(env)});
        },
        [&](std::ifstream& file, std::vector<VideoCommon::FileEnvironment> envs) {
            GraphicsKey key{};
            file.read(reinterpret_cast<char*>(&key), sizeof(key));
            REQUIRE(key.state == ~key.id);
            for (const auto& env : envs) {
                if (env.ShaderStage() == Shader::Stage::Geometry) {
                    REQUIRE(env.GpPassthroughMask()[7] == 0xa5a5a5a5);
                }
                if (env.ShaderStage() == Shader::Stage::Mesh) {
                    REQUIRE(env.MeshRegisters()[2] == 0xabcdef01);
                }
            }
            loaded.push_back({false, key.id, envs.front().ReadCbufValue(1, 16),
                              std::ranges::all_of(envs, Translates)});
        },
        index);
    return loaded;
}

/// One launch as the Vulkan cache runs it: load (newest record per key), build every loaded
/// capture, then build every pipeline the game uses live and append what ClaimWrite allows.
struct Launch {
    size_t loaded{};
    size_t failed{};
    size_t written{};
    size_t rejected{};
};

Launch RunLaunch(const std::filesystem::path& path, std::span<const std::pair<u64, bool>> used,
                 bool live_capture_complete) {
    VideoCommon::DiskPipelineRecords<u64> compute_records;
    VideoCommon::DiskPipelineRecords<u64> graphics_records;
    auto index = MakeIndex();
    Launch launch;
    for (const Loaded& record : Load(path, &index)) {
        auto& records = record.compute ? compute_records : graphics_records;
        REQUIRE(records.NoteStored(record.key));
        ++launch.loaded;
        if (!record.translates) {
            records.NoteFailed(record.key);
            ++launch.failed;
        }
    }
    for (const auto& [key, compute] : used) {
        auto& records = compute ? compute_records : graphics_records;
        using Write = VideoCommon::DiskPipelineRecords<u64>::Write;
        const Write write = records.ClaimWrite(key);
        if (write == Write::No) {
            continue;
        }
        const Capture capture{compute, 2, live_capture_complete,
                              compute ? std::initializer_list<Shader::Stage>{} : GraphicsStages};
        if (write == Write::New) {
            WriteRecord(path, key, compute, 2, live_capture_complete,
                        compute ? std::initializer_list<Shader::Stage>{} : GraphicsStages);
            ++launch.written;
        } else if (AppendChecked(path, key, compute, capture) ==
                   VideoCommon::CheckedWrite::Written) {
            ++launch.written;
        } else {
            ++launch.rejected;
        }
    }
    return launch;
}

} // Anonymous namespace

TEST_CASE("The newest record of a pipeline key is the one loaded",
          "[pipeline_cache_files][repair]") {
    TempDir temp;
    const auto path = temp.path / "records.bin";
    WriteCompute(path, 1, 10);
    WriteGraphics(path, 1, 20);
    WriteRecord(path, 7, false, 70, true, {Shader::Stage::Task, Shader::Stage::Mesh});
    WriteCompute(path, 1, 11);
    WriteCompute(path, 2, 30);
    WriteGraphics(path, 1, 21);
    WriteCompute(path, 1, 12);

    const auto values = [](const std::vector<Loaded>& loaded) {
        std::vector<u32> result;
        for (const auto& record : loaded) {
            result.push_back(record.value);
        }
        return result;
    };
    // Without an index every record is read, as before (and as the OpenGL cache reads).
    REQUIRE(values(Load(path, nullptr)) == std::vector<u32>{10, 20, 70, 11, 30, 21, 12});
    auto index = MakeIndex();
    REQUIRE(values(Load(path, &index)) == std::vector<u32>{70, 30, 21, 12});
    REQUIRE(index.superseded == 3);
    REQUIRE(Records(index, 1, true) == 3);
    REQUIRE(Records(index, 2, true) == 1);
    REQUIRE(Records(index, 1, false) == 2);
    REQUIRE(Records(index, 7, false) == 1);
    REQUIRE(Records(index, 99, true) == 1);
}

TEST_CASE("A damaged record still ends the indexed load as before",
          "[pipeline_cache_files][repair]") {
    TempDir temp;
    const auto path = temp.path / "damaged.bin";
    WriteCompute(path, 1, 10);
    WriteCompute(path, 1, 11);
    WriteGraphics(path, 3, 30);
    std::filesystem::resize_file(path, std::filesystem::file_size(path) - 1);
    auto index = MakeIndex();
    std::vector<u32> values;
    VideoCommon::LoadPipelines(
        {}, path, Version,
        [&](std::ifstream& file, VideoCommon::FileEnvironment env) {
            file.seekg(sizeof(u64), std::ios::cur);
            values.push_back(env.ReadCbufValue(1, 16));
        },
        [&](std::ifstream& file, std::vector<VideoCommon::FileEnvironment>) {
            GraphicsKey key;
            file.read(reinterpret_cast<char*>(&key), sizeof(key));
            FAIL("A cut record reached pipeline creation");
        },
        &index);
    REQUIRE(values == std::vector<u32>{11});
    // The loader deletes a damaged cache, as it did without the index.
    REQUIRE_FALSE(std::filesystem::exists(path));
}

TEST_CASE("A capture that fails to build is replaced by the live build and then loads",
          "[pipeline_cache_files][repair]") {
    TempDir temp;
    const auto path = temp.path / "repair.bin";
    // Incomplete captures of a compute and a graphics pipeline, as an older build wrote them,
    // and a complete one that must stay as it is.
    WriteCompute(path, 1, 1, false);
    WriteGraphics(path, 1, 1, false);
    WriteGraphics(path, 5, 1, true);
    const std::array<std::pair<u64, bool>, 4> used{{{1, true}, {1, false}, {5, false}, {9, true}}};

    const Launch first = RunLaunch(path, used, true);
    CHECK(first.loaded == 3);
    CHECK(first.failed == 2);
    // The two replacements and the new compute pipeline 9; graphics 5 is not written again.
    CHECK(first.written == 3);
    const auto size = std::filesystem::file_size(path);

    const Launch second = RunLaunch(path, used, true);
    CHECK(second.loaded == 4);
    CHECK(second.failed == 0);
    CHECK(second.written == 0);
    CHECK(std::filesystem::file_size(path) == size);
    auto index = MakeIndex();
    for (const Loaded& record : Load(path, &index)) {
        CHECK(record.translates);
        CHECK(record.value == (record.key == 5 ? 1u : 2u));
    }
}

TEST_CASE("A capture that keeps failing does not grow the cache on every launch",
          "[pipeline_cache_files][repair]") {
    TempDir temp;
    const auto path = temp.path / "bounded.bin";
    WriteCompute(path, 1, 1, false);
    WriteGraphics(path, 1, 1, false);
    const auto size = std::filesystem::file_size(path);
    const std::array<std::pair<u64, bool>, 2> used{{{1, true}, {1, false}}};
    for (int launch = 0; launch < 10; ++launch) {
        // The live capture fails as read back too, so it is checked once per launch and never
        // appended.
        const Launch result = RunLaunch(path, used, false);
        CHECK(result.failed == 2);
        CHECK(result.rejected == 2);
        CHECK(result.written == 0);
        CHECK(std::filesystem::file_size(path) == size);
    }
}

TEST_CASE("Older duplicate records do not use up the repair of a key",
          "[pipeline_cache_files][repair]") {
    TempDir temp;
    const auto path = temp.path / "legacy.bin";
    // Four records per key, as the old compute path left them (one per live build), the newest
    // missing the answer the translation now asks for.
    for (u32 value = 0; value < 4; ++value) {
        WriteCompute(path, 1, value, false);
        WriteGraphics(path, 1, value, false);
    }
    const std::array<std::pair<u64, bool>, 2> used{{{1, true}, {1, false}}};
    const Launch first = RunLaunch(path, used, true);
    CHECK(first.loaded == 2);
    CHECK(first.failed == 2);
    CHECK(first.written == 2); // exactly one replacement per key
    CHECK(first.rejected == 0);
    const auto size = std::filesystem::file_size(path);

    const Launch second = RunLaunch(path, used, true);
    CHECK(second.failed == 0);
    CHECK(second.written == 0);
    CHECK(std::filesystem::file_size(path) == size);
    auto index = MakeIndex();
    for (const Loaded& record : Load(path, &index)) {
        CHECK(record.translates);
        CHECK(record.value == 2);
    }
    CHECK(Records(index, 1, true) == 5);
    CHECK(Records(index, 1, false) == 5);
}

TEST_CASE("A checked append writes nothing for a capture it cannot store or that fails",
          "[pipeline_cache_files][repair]") {
    TempDir temp;
    const auto path = temp.path / "checked.bin";
    WriteCompute(path, 1, 1, false);
    const auto size = std::filesystem::file_size(path);
    {
        Capture capture{true, 2, true, {}};
        capture.owned.front()->MakeUnserializable();
        CHECK(AppendChecked(path, 1, true, capture) == VideoCommon::CheckedWrite::NotSerializable);
    }
    CHECK(AppendChecked(path, 1, true, Capture{true, 2, false, {}}) ==
          VideoCommon::CheckedWrite::Rejected);
    CHECK(std::filesystem::file_size(path) == size);
    CHECK(AppendChecked(path, 1, true, Capture{true, 2, true, {}}) ==
          VideoCommon::CheckedWrite::Written);
    CHECK_FALSE(std::filesystem::exists(path.string() + ".candidate"));
    auto index = MakeIndex();
    const auto loaded = Load(path, &index);
    REQUIRE(loaded.size() == 1);
    CHECK(loaded.front().translates);
    CHECK(loaded.front().value == 2);
}

TEST_CASE("Disk pipeline records allow one replacement per failure",
          "[pipeline_cache_files][repair]") {
    using Write = VideoCommon::DiskPipelineRecords<u64>::Write;
    VideoCommon::DiskPipelineRecords<u64> records;
    REQUIRE(records.NoteStored(1));
    REQUIRE_FALSE(records.NoteStored(1));
    CHECK(records.ClaimWrite(1) == Write::No);
    records.NoteFailed(1);
    CHECK(records.ClaimWrite(1) == Write::Replacement);
    CHECK(records.ClaimWrite(1) == Write::No);
    CHECK(records.ClaimWrite(2) == Write::New);
    CHECK(records.ClaimWrite(2) == Write::No);
    // A failure of a pipeline the cache does not hold changes nothing.
    records.NoteFailed(4);
    CHECK(records.ClaimWrite(4) == Write::New);
}

TEST_CASE("Pipeline cache names are version-suffixed per build", "[pipeline_cache_files]") {
    const std::filesystem::path dir{"shader"};
    CHECK(VideoCommon::VersionedPipelineCachePath(dir, "vulkan", 39).filename() ==
          "vulkan.fork-v39.bin");
    CHECK(VideoCommon::VersionedPipelineCachePath(dir, "vulkan_pipelines", 39).filename() ==
          "vulkan_pipelines.fork-v39.bin");
    CHECK(VideoCommon::VersionedPipelineCachePath(dir, "vulkan", 18) !=
          VideoCommon::VersionedPipelineCachePath(dir, "vulkan", 39));
}

TEST_CASE("A legacy cache of this version is adopted with its contents",
          "[pipeline_cache_files]") {
    TempDir temp;
    const auto legacy = temp.path / "vulkan.bin";
    const auto versioned = VideoCommon::VersionedPipelineCachePath(temp.path, "vulkan", 39);
    WriteCache(legacy, Magic, 39, "pipelines");
    const std::string before = ReadAll(legacy);
    REQUIRE(VideoCommon::AdoptLegacyPipelineCache(legacy, versioned, Magic, 39));
    CHECK_FALSE(std::filesystem::exists(legacy));
    CHECK(ReadAll(versioned) == before);
    // Idempotent: nothing left to adopt.
    CHECK_FALSE(VideoCommon::AdoptLegacyPipelineCache(legacy, versioned, Magic, 39));
}

TEST_CASE("Another build's legacy cache is never touched", "[pipeline_cache_files]") {
    TempDir temp;
    const auto legacy = temp.path / "vulkan.bin";
    const auto versioned = VideoCommon::VersionedPipelineCachePath(temp.path, "vulkan", 39);
    WriteCache(legacy, Magic, 18, "official");
    const std::string before = ReadAll(legacy);
    CHECK_FALSE(VideoCommon::AdoptLegacyPipelineCache(legacy, versioned, Magic, 39));
    CHECK(ReadAll(legacy) == before);
    CHECK_FALSE(std::filesystem::exists(versioned));

    WriteCache(legacy, OtherMagic, 39, "wrong magic");
    CHECK_FALSE(VideoCommon::AdoptLegacyPipelineCache(legacy, versioned, Magic, 39));
    CHECK(std::filesystem::exists(legacy));

    // Truncated header.
    {
        std::ofstream stream(legacy, std::ios::binary | std::ios::trunc);
        stream.write(Magic.data(), 4);
    }
    CHECK_FALSE(VideoCommon::AdoptLegacyPipelineCache(legacy, versioned, Magic, 39));
    CHECK(std::filesystem::exists(legacy));
}

TEST_CASE("An existing versioned cache is never overwritten by a legacy one",
          "[pipeline_cache_files]") {
    TempDir temp;
    const auto legacy = temp.path / "vulkan.bin";
    const auto versioned = VideoCommon::VersionedPipelineCachePath(temp.path, "vulkan", 39);
    WriteCache(legacy, Magic, 39, "older");
    WriteCache(versioned, Magic, 39, "current");
    const std::string current = ReadAll(versioned);
    CHECK_FALSE(VideoCommon::AdoptLegacyPipelineCache(legacy, versioned, Magic, 39));
    CHECK(ReadAll(versioned) == current);
    CHECK(std::filesystem::exists(legacy));
    CHECK_FALSE(VideoCommon::AdoptLegacyPipelineCache(temp.path / "missing.bin", versioned,
                                                      Magic, 39));
}

TEST_CASE("Cached compute failures retain their cause and the current dispatch address",
          "[pipeline_cache][diagnostics]") {
    const Vulkan::ComputePipelineCacheKey key{
        .unique_hash = 0x1234,
        .shared_memory_size = 1024,
        .workgroup_size = {32, 1, 1},
    };
    std::unordered_map<Vulkan::ComputePipelineCacheKey, Vulkan::ComputePipelineCacheEntry> cache;
    const std::string cause =
        "SPIR-V emission failed at IR opcode UndefU8: SPIR-V Instruction is not implemented";
    cache[key].failure = cause;
    for (const GPUVAddr address : {0x211ddd9000ULL, 0x21ebdfa000ULL}) {
        const auto [entry, inserted] = cache.try_emplace(key);
        REQUIRE_FALSE(inserted);
        CHECK(entry->second.Get(false, address, key) == nullptr);
        try {
            static_cast<void>(entry->second.Get(true, address, key));
            FAIL("Native compute failure was not propagated");
        } catch (const Shader::RuntimeError& exception) {
            const std::string message{exception.what()};
            CHECK(message.find(cause) != std::string::npos);
            CHECK(message.find(fmt::format("address={:#x}", address)) != std::string::npos);
            CHECK(message.find(fmt::format("shader={:#016x}", key.unique_hash)) != std::string::npos);
            CHECK(message.find(fmt::format("pipeline={:#016x}", key.Hash())) != std::string::npos);
        }
        CHECK(entry->second.failure == cause);
        CHECK_FALSE(entry->second.pipeline);
    }
    auto other = key;
    other.workgroup_size[0] = 64;
    const auto [entry, inserted] = cache.try_emplace(other);
    REQUIRE(inserted);
    CHECK(entry->second.failure.empty());
    CHECK(entry->second.Get(false, 0x2120633c00, other) == nullptr);
    REQUIRE_THROWS_AS(entry->second.Get(true, 0x2120633c00, other), Shader::RuntimeError);
}

TEST_CASE("Pipeline keys with the same hash stay apart in the index and the disk records",
          "[pipeline_cache_files][repair]") {
    TempDir temp;
    const auto path = temp.path / "collide.bin";
    WriteCompute(path, 1, 10);
    WriteCompute(path, 2, 20);
    WriteGraphics(path, 3, 30);
    WriteGraphics(path, 4, 40);
    WriteCompute(path, 1, 11);
    auto index = MakeIndex(true);
    std::vector<std::pair<u64, u32>> loaded;
    for (const Loaded& record : Load(path, &index)) {
        loaded.emplace_back(record.key, record.value);
    }
    // Only the older record of compute key 1 gives way; keys 2, 3 and 4 share its hash but not
    // its bytes, so all of them load.
    REQUIRE(loaded == std::vector<std::pair<u64, u32>>{{2, 20}, {3, 30}, {4, 40}, {1, 11}});
    REQUIRE(index.superseded == 1);
    REQUIRE(Records(index, 1, true) == 2);
    REQUIRE(Records(index, 2, true) == 1);
    REQUIRE(Records(index, 3, false) == 1);

    using Write = VideoCommon::DiskPipelineRecords<u64, SameHash>::Write;
    VideoCommon::DiskPipelineRecords<u64, SameHash> records;
    REQUIRE(records.NoteStored(1));
    REQUIRE(records.NoteStored(2));
    records.NoteFailed(2);
    CHECK(records.ClaimWrite(1) == Write::No);
    CHECK(records.ClaimWrite(2) == Write::Replacement);
    CHECK(records.ClaimWrite(5) == Write::New);
}
