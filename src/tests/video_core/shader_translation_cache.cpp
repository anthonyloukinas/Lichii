// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <regex>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "video_core/shader_translation_cache.h"

namespace {
using namespace VideoCommon::TranslationCache;

class FakeEnvironment final : public Shader::Environment {
public:
    FakeEnvironment() {
        stage = Shader::Stage::Fragment;
        start_address = 0x80;
    }

    u64 ReadInstruction(u32) override {
        return 0;
    }
    u32 ReadCbufValue(u32 index, u32 offset) override {
        ++reads;
        u32& word = cbufs[(u64{index} << 32) | offset];
        // A guest that writes the buffer during the translation: every read sees another value.
        return drifting ? word++ : word;
    }
    Shader::TextureType ReadTextureType(u32 handle) override {
        return array_handles.contains(handle) ? Shader::TextureType::ColorArray2D
                                              : Shader::TextureType::Color2D;
    }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override {
        return Shader::TexturePixelFormat{};
    }
    bool IsTexturePixelFormatInteger(u32 handle) override {
        return integer_handles.contains(handle);
    }
    u32 ReadViewportTransformState() override {
        return viewport;
    }
    u32 TextureBoundBuffer() const override {
        return texture_bound;
    }
    u32 LocalMemorySize() const override {
        return 0;
    }
    u32 SharedMemorySize() const override {
        return 0;
    }
    std::array<u32, 3> WorkgroupSize() const override {
        return {1, 1, 1};
    }
    bool HasHLEMacroState() const override {
        return false;
    }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
    bool IsCbufIndirectStorageBufferPointer(u32 index, u32 offset, u32 indirect) override {
        ++table_queries;
        return table_pointers.contains({index, offset, indirect});
    }
    void Dump(u64, u64) override {}

    void SetHeaderWord(size_t word, u32 value) {
        std::memcpy(reinterpret_cast<u8*>(&sph) + word * sizeof(u32), &value, sizeof(value));
    }
    void SetMeshStage(Shader::Stage mesh_stage, const std::array<u32, 3>& registers) {
        stage = mesh_stage;
        mesh_registers = registers;
    }
    void SetPrimitiveHeaderWord(size_t word, u32 value) {
        std::memcpy(reinterpret_cast<u8*>(&mesh_primitive_header) + word * sizeof(u32), &value,
                    sizeof(value));
    }

    std::map<u64, u32> cbufs;
    u32 viewport = 1;
    u32 texture_bound = 2;
    u32 reads = 0;
    bool drifting = false;
    std::set<std::tuple<u32, u32, u32>> table_pointers;
    u32 table_queries = 0;
    std::set<u32> array_handles{7};
    std::set<u32> integer_handles;
};

// What a translation does with its environment, for the record/replay tests: the second read
// depends on the first answer, as a bindless handle lookup does.
void Translate(Shader::Environment& env) {
    const u32 first = env.ReadCbufValue(1, 0x10);
    (void)env.ReadCbufValue(1, first);
    (void)env.ReadTextureType(7);
    (void)env.ReadViewportTransformState();
    (void)env.GetReplaceConstBuffer(0, 4);
}

Shader::TextureDescriptor texture_for_test(u32 index) {
    Shader::TextureDescriptor texture{};
    texture.type = Shader::TextureType::Color2D;
    texture.cbuf_index = 0;
    texture.cbuf_offset = 0x100 + index * 8;
    texture.count = 1;
    return texture;
}

Shader::Info SampleInfo() {
    Shader::Info info{};
    info.uses_sample_id = true;
    info.uses_patches[3] = true;
    info.interpolation[5] = Shader::Interpolation::Flat;
    info.stores.Set(Shader::IR::Attribute::PositionX);
    info.legacy_stores_mapping.emplace(Shader::IR::Attribute::ColorFrontDiffuseR,
                                       Shader::IR::Attribute::Generic0X);
    info.stores_frag_color[2] = true;
    info.constant_buffer_mask = 0x5;
    info.constant_buffer_used_sizes[2] = 0x400;
    info.nvn_buffer_used.set(3);
    info.used_clip_distances = 2;
    info.constant_buffer_descriptors.push_back({.index = 2, .count = 1});
    info.storage_buffers_descriptors.push_back(
        {.cbuf_index = 0, .cbuf_offset = 0x110, .count = 1, .is_written = true});
    Shader::TextureDescriptor texture{};
    texture.type = Shader::TextureType::ColorArray2D;
    texture.cbuf_index = 1;
    texture.cbuf_offset = 0x30;
    texture.count = 1;
    texture.handle_program = 0;
    info.texture_descriptors.push_back(texture);
    info.handle_programs.push_back({1, 2, 3});
    return info;
}

std::filesystem::path TempPath(const char* name) {
    return std::filesystem::temp_directory_path() / name;
}

// Group 1 of each `declaration` match in the class that starts at `head` (comments removed).
// `head` missing gives an empty set.
std::set<std::string> DeclaredFunctions(const std::string& text, const std::string& head,
                                        const std::regex& declaration) {
    const size_t begin = text.find(head);
    if (begin == std::string::npos) {
        return {};
    }
    const size_t end = text.find("\n};", begin);
    const std::string body = std::regex_replace(
        text.substr(begin, end == std::string::npos ? std::string::npos : end - begin),
        std::regex{"//[^\n]*"}, "");
    std::set<std::string> names;
    for (auto it = std::sregex_iterator(body.begin(), body.end(), declaration);
         it != std::sregex_iterator{}; ++it) {
        names.insert((*it)[1].str());
    }
    return names;
}

// Virtuals of Shader::Environment (environment.h) that RecordingEnvironment (wrapper_header)
// does not override. A defaulted virtual it leaves out answers the translator without asking
// the live environment and is never recorded or replayed.
std::set<std::string> UnforwardedQueries(const std::string& environment_header,
                                         const std::string& wrapper_header) {
    std::set<std::string> missing = DeclaredFunctions(
        environment_header, "class Environment {", std::regex{R"(virtual\s+[^;{]*?(\w+)\s*\()"});
    missing.erase("Environment"); // the destructor
    for (const std::string& name :
         DeclaredFunctions(wrapper_header, "class RecordingEnvironment final",
                           std::regex{R"((\w+)\s*\([^;{)]*\)\s*(?:const\s*)?override)"})) {
        missing.erase(name);
    }
    return missing;
}

std::string ReadSourceFile(const std::filesystem::path& path) {
    std::ifstream file{path};
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}
} // Anonymous namespace

TEST_CASE("Translation cache covers every field of the shader structures",
          "[video_core][translation_cache]") {
    REQUIRE(LayoutIsCovered());
}

TEST_CASE("Translation cache round-trips the shader info", "[video_core][translation_cache]") {
    const Shader::Info info = SampleInfo();
    std::vector<u8> bytes;
    WriteInfo(bytes, info);
    std::span<const u8> in{bytes};
    Shader::Info read{};
    REQUIRE(ReadInfo(in, read));
    REQUIRE(in.empty());
    std::vector<u8> again;
    WriteInfo(again, read);
    REQUIRE(again == bytes);
    REQUIRE(InfoEqual(read, info));
    Shader::Info changed = info;
    changed.texture_descriptors[0].cbuf_offset = 0x34;
    REQUIRE_FALSE(InfoEqual(changed, info));
    changed = info;
    changed.stores.Set(Shader::IR::Attribute::PositionY);
    REQUIRE_FALSE(InfoEqual(changed, info));
    changed = info;
    changed.handle_programs[0].push_back(4);
    REQUIRE_FALSE(InfoEqual(changed, info));
    REQUIRE(read.uses_sample_id);
    REQUIRE(read.legacy_stores_mapping.size() == 1);
    REQUIRE(read.texture_descriptors.size() == 1);
    REQUIRE(read.texture_descriptors[0].cbuf_offset == 0x30);
    REQUIRE(read.handle_programs == std::vector<std::vector<u32>>{{1, 2, 3}});
    REQUIRE(read.storage_buffers_descriptors[0].is_written);

    // More descriptors than a small_vector keeps inline (a shader with 20 textures).
    Shader::Info many = SampleInfo();
    for (u32 i = 0; i < 20; ++i) {
        many.texture_descriptors.push_back(texture_for_test(i));
    }
    std::vector<u8> many_bytes;
    WriteInfo(many_bytes, many);
    std::span<const u8> many_in{many_bytes};
    Shader::Info many_read{};
    REQUIRE(ReadInfo(many_in, many_read));
    REQUIRE(many_read.texture_descriptors.size() == 21);
    REQUIRE(many_read.texture_descriptors[20].cbuf_offset == 0x100 + 19 * 8);

    // Truncated input is rejected rather than read past its end.
    std::span<const u8> cut{bytes.data(), bytes.size() - 3};
    REQUIRE_FALSE(ReadInfo(cut, read));
}

TEST_CASE("Translation cache replays the queries a translation made",
          "[video_core][translation_cache]") {
    FakeEnvironment env;
    env.cbufs[(u64{1} << 32) | 0x10] = 0x40;
    env.cbufs[(u64{1} << 32) | 0x40] = 0x1234;
    RecordingEnvironment recording{env};
    REQUIRE(recording.ShaderStage() == Shader::Stage::Fragment);
    REQUIRE(recording.StartAddress() == 0x80);
    Translate(recording);
    const std::vector<Query> queries = recording.TakeQueries();
    REQUIRE(queries.size() == 5);
    REQUIRE(ReplayQueries(queries, env));

    // A changed indirect word: the second query no longer matches.
    env.cbufs[(u64{1} << 32) | 0x40] = 0x9999;
    REQUIRE_FALSE(ReplayQueries(queries, env));
    env.cbufs[(u64{1} << 32) | 0x40] = 0x1234;

    // A changed first word stops the replay at once: the dependent read is never made.
    env.cbufs[(u64{1} << 32) | 0x10] = 0x44;
    env.reads = 0;
    REQUIRE_FALSE(ReplayQueries(queries, env));
    REQUIRE(env.reads == 1);
    env.cbufs[(u64{1} << 32) | 0x10] = 0x40;

    env.viewport = 0;
    REQUIRE_FALSE(ReplayQueries(queries, env));
}

TEST_CASE("Translation cache finds a texture again under another handle",
          "[video_core][translation_cache]") {
    FakeEnvironment env;
    const u64 word = (u64{2} << 32) | 0x30;
    Shader::ConstBufferAddr addr{};
    addr.index = 2;
    addr.offset = 0x30;
    env.cbufs[word] = 7;
    RecordingEnvironment recording{env};
    REQUIRE(recording.ReadTextureTypeAt(addr) == Shader::TextureType::ColorArray2D);
    (void)recording.ReadTexturePixelFormatAt(addr);
    REQUIRE_FALSE(recording.IsTexturePixelFormatIntegerAt(addr));
    const std::vector<Query> queries = recording.TakeQueries();
    // The handle's words are not recorded as values.
    REQUIRE(queries.size() == 3);
    REQUIRE(queries[0].kind == QueryKind::TextureTypeAt);
    REQUIRE(queries[1].kind == QueryKind::TexturePixelFormatAt);
    REQUIRE(queries[2].kind == QueryKind::TexturePixelFormatIntegerAt);
    REQUIRE(ReplayQueries(queries, env));

    // Another session gives a texture of the same kind another handle.
    env.array_handles.insert(0x1234);
    env.cbufs[word] = 0x1234;
    REQUIRE(ReplayQueries(queries, env));

    // A texture of another type, then one with an integer format, at the same address.
    env.cbufs[word] = 8;
    REQUIRE_FALSE(ReplayQueries(queries, env));
    env.cbufs[word] = 0x1234;
    env.integer_handles.insert(0x1234);
    REQUIRE_FALSE(ReplayQueries(queries, env));
    env.integer_handles.clear();

    // The bits of c no address uses are not ignored.
    std::vector<Query> damaged = queries;
    damaged[0].c |= 1U << 31;
    REQUIRE_FALSE(ReplayQueries(damaged, env));
}

TEST_CASE("Translation cache combines the two words of a texture handle",
          "[video_core][translation_cache]") {
    FakeEnvironment env;
    const u64 low = (u64{2} << 32) | 0x30;
    const u64 high = (u64{3} << 32) | 0x8;
    Shader::ConstBufferAddr addr{};
    addr.index = 2;
    addr.offset = 0x30;
    addr.has_secondary = true;
    addr.secondary_index = 3;
    addr.secondary_offset = 0x8;
    addr.secondary_shift_left = 20;
    env.cbufs[low] = 5;
    env.cbufs[high] = 3;
    env.array_handles = {5U | (3U << 20)};
    RecordingEnvironment recording{env};
    REQUIRE(recording.ReadTextureTypeAt(addr) == Shader::TextureType::ColorArray2D);
    const std::vector<Query> queries = recording.TakeQueries();
    REQUIRE(queries.size() == 1);
    REQUIRE(ReplayQueries(queries, env));

    // Both words change and the texture is of the same type: found.
    env.cbufs[low] = 6;
    env.cbufs[high] = 1;
    env.array_handles = {6U | (1U << 20)};
    REQUIRE(ReplayQueries(queries, env));
    // The secondary word alone decides the texture.
    env.cbufs[high] = 2;
    REQUIRE_FALSE(ReplayQueries(queries, env));
}

TEST_CASE("Translation cache keeps a handle word that is read as a value",
          "[video_core][translation_cache]") {
    FakeEnvironment env;
    const u64 word = (u64{1} << 32) | 0x30;
    Shader::ConstBufferAddr addr{};
    addr.index = 1;
    addr.offset = 0x30;
    env.cbufs[word] = 7;
    env.array_handles = {7, 9};

    // The word is also a constant of the translation (a mask): another handle is another
    // translation.
    {
        RecordingEnvironment recording{env};
        (void)recording.ReadCbufValue(1, 0x30);
        (void)recording.ReadTextureTypeAt(addr);
        const std::vector<Query> queries = recording.TakeQueries();
        REQUIRE(queries.size() == 2);
        REQUIRE(ReplayQueries(queries, env));
        env.cbufs[word] = 9;
        REQUIRE_FALSE(ReplayQueries(queries, env));
        env.cbufs[word] = 7;
    }
    // The translation asked for the handle itself.
    {
        RecordingEnvironment recording{env};
        REQUIRE(recording.ReadTextureHandle(addr) == 7);
        const std::vector<Query> queries = recording.TakeQueries();
        REQUIRE_FALSE(queries.empty());
        REQUIRE(std::ranges::all_of(
            queries, [](const Query& query) { return query.kind == QueryKind::CbufValue; }));
        REQUIRE(ReplayQueries(queries, env));
        env.cbufs[word] = 9;
        REQUIRE_FALSE(ReplayQueries(queries, env));
        env.cbufs[word] = 7;
    }
    // An address the query does not hold is recorded by value.
    {
        Shader::ConstBufferAddr wide = addr;
        wide.index = 300;
        env.cbufs[(u64{300} << 32) | 0x30] = 7;
        RecordingEnvironment recording{env};
        REQUIRE(recording.ReadTextureTypeAt(wide) == Shader::TextureType::ColorArray2D);
        const std::vector<Query> queries = recording.TakeQueries();
        REQUIRE(queries.back().kind == QueryKind::TextureType);
        REQUIRE(queries.front().kind == QueryKind::CbufValue);
        REQUIRE(ReplayQueries(queries, env));
        env.cbufs[(u64{300} << 32) | 0x30] = 9;
        REQUIRE_FALSE(ReplayQueries(queries, env));
    }
}

TEST_CASE("Translation cache reads a constant buffer word once", "[video_core][translation_cache]") {
    FakeEnvironment env;
    const u64 word = (u64{1} << 32) | 0x30;
    Shader::ConstBufferAddr addr{};
    addr.index = 1;
    addr.offset = 0x30;
    env.cbufs[word] = 7;
    {
        // Queries about the texture, the word as a value, the texture again: one read. The value
        // is recorded although the word had been read for the handle.
        RecordingEnvironment recording{env};
        (void)recording.ReadTextureTypeAt(addr);
        (void)recording.ReadTexturePixelFormatAt(addr);
        (void)recording.IsTexturePixelFormatIntegerAt(addr);
        REQUIRE(recording.ReadCbufValue(1, 0x30) == 7);
        (void)recording.ReadTextureTypeAt(addr);
        REQUIRE(env.reads == 1);
        const std::vector<Query> queries = recording.TakeQueries();
        REQUIRE(queries.size() == 5);
        REQUIRE(queries[3].kind == QueryKind::CbufValue);
        REQUIRE(queries[3].answer == 7);
        env.reads = 0;
        REQUIRE(ReplayQueries(queries, env));
        REQUIRE(env.reads == 1);
    }
    {
        // The value first, then the handle.
        env.reads = 0;
        RecordingEnvironment recording{env};
        REQUIRE(recording.ReadCbufValue(1, 0x30) == 7);
        REQUIRE(recording.ReadTextureTypeAt(addr) == Shader::TextureType::ColorArray2D);
        REQUIRE(env.reads == 1);
        REQUIRE(recording.TakeQueries().size() == 2);
    }
    {
        // A word the guest writes meanwhile: every answer comes from the first read.
        env.drifting = true;
        RecordingEnvironment recording{env};
        REQUIRE(recording.ReadTextureTypeAt(addr) == Shader::TextureType::ColorArray2D);
        REQUIRE(recording.ReadTextureTypeAt(addr) == Shader::TextureType::ColorArray2D);
        REQUIRE(recording.ReadCbufValue(1, 0x30) == 7);
        const std::vector<Query> queries = recording.TakeQueries();
        env.cbufs[word] = 7;
        REQUIRE(ReplayQueries(queries, env));
        env.drifting = false;
        env.cbufs[word] = 7;
    }
    {
        // Without the cache the handle's single word is read once as well, until the texture
        // pass clears its words.
        env.reads = 0;
        REQUIRE(env.ReadTextureHandle(addr) == 7);
        REQUIRE(env.ReadTextureTypeAt(addr) == Shader::TextureType::ColorArray2D);
        REQUIRE(env.reads == 1);
        env.cbuf_word_cache.clear();
        REQUIRE(env.ReadTextureHandle(addr) == 7);
        REQUIRE(env.reads == 2);
    }
}

TEST_CASE("Translation cache holds the widest handle address by address",
          "[video_core][translation_cache]") {
    FakeEnvironment env;
    const u64 first = (u64{255} << 32) | 0x10;
    const u64 second = (u64{255} << 32) | 0x14;
    Shader::ConstBufferAddr addr{};
    addr.index = 255;
    addr.offset = 0x10;
    addr.shift_left = 31;
    addr.has_secondary = true;
    addr.secondary_index = 255;
    addr.secondary_offset = 0x14;
    env.cbufs[first] = 1;
    env.cbufs[second] = 5;
    for (const bool swapped : {false, true}) {
        if (swapped) {
            std::swap(addr.shift_left, addr.secondary_shift_left);
        }
        const u32 handle = swapped ? (1U | (5U << 31)) : ((1U << 31) | 5U);
        env.array_handles = {handle};
        env.cbuf_word_cache.clear();
        REQUIRE(env.ReadTextureHandle(addr) == handle);
        RecordingEnvironment recording{env};
        REQUIRE(recording.ReadTextureTypeAt(addr) == Shader::TextureType::ColorArray2D);
        const std::vector<Query> queries = recording.TakeQueries();
        REQUIRE(queries.size() == 1);
        REQUIRE(queries[0].kind == QueryKind::TextureTypeAt);
        REQUIRE(ReplayQueries(queries, env));
        // The word shifted by 31 keeps its lowest bit only.
        env.cbufs[swapped ? second : first] = 2;
        REQUIRE_FALSE(ReplayQueries(queries, env));
        env.cbufs[swapped ? second : first] = swapped ? 5 : 1;
    }

    // One buffer index further is recorded by value: one value per word, then the query.
    Shader::ConstBufferAddr wide = addr;
    wide.secondary_index = 256;
    env.cbufs[(u64{256} << 32) | 0x14] = 5;
    RecordingEnvironment recording{env};
    (void)recording.ReadTextureTypeAt(wide);
    (void)recording.ReadTextureTypeAt(wide);
    const std::vector<Query> queries = recording.TakeQueries();
    REQUIRE(queries.size() == 4);
    REQUIRE(queries[0].kind == QueryKind::CbufValue);
    REQUIRE(queries[1].kind == QueryKind::CbufValue);
    REQUIRE(queries[2].kind == QueryKind::TextureType);
    REQUIRE(queries[3].kind == QueryKind::TextureType);
    REQUIRE(ReplayQueries(queries, env));
}

TEST_CASE("Translation cache stage keys cover the environment", "[video_core][translation_cache]") {
    const Digest config{1, 2};
    FakeEnvironment env;
    const Digest key = MakeStageKey(env, 0xabc, 5, config);
    REQUIRE(MakeStageKey(env, 0xabc, 5, config) == key);
    REQUIRE_FALSE(MakeStageKey(env, 0xabd, 5, config) == key);
    REQUIRE_FALSE(MakeStageKey(env, 0xabc, 4, config) == key);
    REQUIRE_FALSE(MakeStageKey(env, 0xabc, 5, Digest{1, 3}) == key);
    env.texture_bound = 3;
    REQUIRE_FALSE(MakeStageKey(env, 0xabc, 5, config) == key);
    env.texture_bound = 2;
    env.SetHeaderWord(4, 0x10);
    REQUIRE_FALSE(MakeStageKey(env, 0xabc, 5, config) == key);

    // Task and mesh programs: the mesh registers and the primitive output header decide the
    // program's mesh info.
    FakeEnvironment mesh;
    mesh.SetMeshStage(Shader::Stage::Mesh, {0x00004314, 0x00100000, 0x00401000});
    const Digest mesh_key = MakeStageKey(mesh, 0xabc, 3, config);
    mesh.SetMeshStage(Shader::Stage::Mesh, {0x00004311, 0x00100000, 0x00401000});
    REQUIRE_FALSE(MakeStageKey(mesh, 0xabc, 3, config) == mesh_key);
    mesh.SetMeshStage(Shader::Stage::Mesh, {0x00004314, 0x00100000, 0x00801000});
    REQUIRE_FALSE(MakeStageKey(mesh, 0xabc, 3, config) == mesh_key);
    mesh.SetMeshStage(Shader::Stage::Mesh, {0x00004314, 0x00100000, 0x00401000});
    REQUIRE(MakeStageKey(mesh, 0xabc, 3, config) == mesh_key);
    mesh.SetPrimitiveHeaderWord(13, 0x1);
    REQUIRE_FALSE(MakeStageKey(mesh, 0xabc, 3, config) == mesh_key);
    mesh.SetPrimitiveHeaderWord(13, 0);
    mesh.SetMeshStage(Shader::Stage::Task, {0x00004314, 0x00100000, 0x00401000});
    REQUIRE_FALSE(MakeStageKey(mesh, 0xabc, 3, config) == mesh_key);
}

TEST_CASE("Translation cache keeps the mesh info of task and mesh stages",
          "[video_core][translation_cache]") {
    Shader::IR::Program program;
    program.stage = Shader::Stage::Mesh;
    program.workgroup_size = {32, 1, 1};
    program.mesh.payload_words = 64;
    program.mesh.max_vertices = 64;
    program.mesh.max_primitives = 126;
    program.mesh.topology = 4;
    program.mesh.vertex_lines = {7, 8, 32};
    program.mesh.primitive_lines = {33};
    program.info = SampleInfo();
    program.info.is_mesh = true;
    const ProgramMeta meta = ProgramMeta::From(program);
    const Shader::IR::Program rebuilt = meta.ToProgram();
    REQUIRE(rebuilt.mesh.max_primitives == 126);
    REQUIRE(rebuilt.mesh.topology == 4);
    REQUIRE(std::ranges::equal(rebuilt.mesh.vertex_lines, program.mesh.vertex_lines));
    REQUIRE(std::ranges::equal(rebuilt.mesh.primitive_lines, program.mesh.primitive_lines));
    REQUIRE(MetaEqual(ProgramMeta::From(rebuilt), meta));

    // The verify mode compares the stored metadata with a fresh translation: each mesh field
    // that differs is noticed.
    const auto differs = [&meta](auto&& change) {
        ProgramMeta changed = meta;
        change(changed.mesh);
        return !MetaEqual(changed, meta);
    };
    REQUIRE(differs([](Shader::MeshInfo& mesh) { mesh.payload_words = 32; }));
    REQUIRE(differs([](Shader::MeshInfo& mesh) { mesh.max_vertices = 63; }));
    REQUIRE(differs([](Shader::MeshInfo& mesh) { mesh.max_primitives = 0; }));
    REQUIRE(differs([](Shader::MeshInfo& mesh) { mesh.topology = 0; }));
    REQUIRE(differs([](Shader::MeshInfo& mesh) { mesh.vertex_lines.pop_back(); }));
    REQUIRE(differs([](Shader::MeshInfo& mesh) { mesh.primitive_lines.clear(); }));

    // The mesh info survives the file.
    const auto path = TempPath("lichii_translation_cache_mesh_test.bin");
    std::filesystem::remove(path);
    const Digest config{0x41, 0x42};
    FakeEnvironment env;
    env.SetMeshStage(Shader::Stage::Mesh, {0x00fc4314, 0x00100000, 0x00401000});
    const Digest stage_key = MakeStageKey(env, 0x99, 3, config);
    {
        Store store;
        store.Open(path, config);
        REQUIRE(store.Insert(stage_key, {}, meta));
    }
    {
        Store store;
        store.Open(path, config);
        const auto entry = store.FindEntry(stage_key, env);
        REQUIRE(entry);
        REQUIRE(MetaEqual(entry->meta, meta));
        REQUIRE(entry->meta.mesh.payload_words == 64);
        REQUIRE(entry->meta.mesh.vertex_lines.size() == 3);
    }
    std::filesystem::remove(path);
}

TEST_CASE("Translation cache drops damaged mesh lists and files of an older format",
          "[video_core][translation_cache]") {
    const auto path = TempPath("lichii_translation_cache_mesh_damage_test.bin");
    const Digest config{0x51, 0x52};
    FakeEnvironment env;
    env.SetMeshStage(Shader::Stage::Mesh, {0x00fc4314, 0x00100000, 0x00401000});
    const Digest stage_key = MakeStageKey(env, 0x77, 3, config);
    ProgramMeta meta{};
    meta.stage = Shader::Stage::Mesh;
    meta.mesh.max_vertices = 64;
    meta.mesh.max_primitives = 126;
    meta.mesh.topology = 4;
    meta.mesh.vertex_lines = {0x51a7, 0x51a8, 0x51a9};
    // A full second list: the record holds more words after the first list's count than any
    // count the structure accepts, so only the capacity check stops an oversized count.
    meta.mesh.primitive_lines.resize(meta.mesh.primitive_lines.capacity(), 0x51b1);
    meta.info = SampleInfo();

    const auto write_file = [&] {
        std::filesystem::remove(path);
        Store store;
        store.Open(path, config);
        REQUIRE(store.Insert(stage_key, {}, meta));
    };
    const auto read_bytes = [&] {
        std::ifstream file{path, std::ios::binary};
        return std::vector<char>{std::istreambuf_iterator<char>{file}, {}};
    };
    const auto write_bytes = [&](const std::vector<char>& bytes) {
        std::ofstream file{path, std::ios::binary | std::ios::trunc};
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    };
    const auto found = [&] {
        Store store;
        store.Open(path, config);
        return store.FindEntry(stage_key, env) != nullptr;
    };
    const auto offset_of = [](const std::vector<char>& bytes, std::span<const u32> words) {
        const auto* begin = reinterpret_cast<const char*>(words.data());
        const auto it = std::search(bytes.begin(), bytes.end(), begin, begin + words.size_bytes());
        REQUIRE(it != bytes.end());
        return static_cast<size_t>(it - bytes.begin());
    };
    // The first list in the file: its count, then its values.
    const std::array<u32, 4> list{3, 0x51a7, 0x51a8, 0x51a9};
    const std::array<u32, 1> record_magic{0x52435453};

    write_file();
    REQUIRE(found());

    // A count over the structure's capacity.
    std::vector<char> bytes = read_bytes();
    const u32 oversized = static_cast<u32>(meta.mesh.vertex_lines.capacity()) + 1;
    REQUIRE(bytes.size() - offset_of(bytes, list) >= sizeof(u32) * (oversized + 1));
    std::memcpy(bytes.data() + offset_of(bytes, list), &oversized, sizeof(oversized));
    write_bytes(bytes);
    REQUIRE_FALSE(found());

    // A record that ends inside the list.
    write_file();
    bytes = read_bytes();
    const size_t record = offset_of(bytes, record_magic);
    const size_t cut = offset_of(bytes, list) + 2 * sizeof(u32);
    const u32 length = static_cast<u32>(cut - record - 2 * sizeof(u32));
    std::memcpy(bytes.data() + record + sizeof(u32), &length, sizeof(length));
    bytes.resize(cut);
    write_bytes(bytes);
    REQUIRE_FALSE(found());

    // Files of the two formats before this one (the version follows the 8 byte file magic):
    // their entries have no mesh info and are not read; the file starts over.
    constexpr size_t VersionOffset = 8;
    for (const u32 age : {1u, 2u}) {
        write_file();
        bytes = read_bytes();
        u32 version{};
        std::memcpy(&version, bytes.data() + VersionOffset, sizeof(version));
        const u32 older = version - age;
        std::memcpy(bytes.data() + VersionOffset, &older, sizeof(older));
        write_bytes(bytes);
        REQUIRE_FALSE(found());
        bytes = read_bytes();
        u32 rewritten{};
        std::memcpy(&rewritten, bytes.data() + VersionOffset, sizeof(rewritten));
        REQUIRE(rewritten == version);
    }
    std::filesystem::remove(path);
}

TEST_CASE("Translation cache runtime info digest covers emitted state",
          "[video_core][translation_cache]") {
    Shader::RuntimeInfo info{};
    const Digest base = DigestRuntimeInfo(info);
    REQUIRE(DigestRuntimeInfo(info) == base);
    info.alpha_test_func = Shader::CompareFunction::Greater;
    const Digest alpha = DigestRuntimeInfo(info);
    REQUIRE_FALSE(alpha == base);
    info.alpha_test_reference = 0.5f;
    REQUIRE_FALSE(DigestRuntimeInfo(info) == alpha);
    info = {};
    info.color_output_types[1] = Shader::AttributeType::UnsignedInt;
    REQUIRE_FALSE(DigestRuntimeInfo(info) == base);
    info = {};
    info.previous_stage_stores.Set(Shader::IR::Attribute::Generic3X);
    REQUIRE_FALSE(DigestRuntimeInfo(info) == base);
}

TEST_CASE("Translation cache records which pointer table slots hold pointers",
          "[video_core][translation_cache]") {
    // Each slot of a pointer table is asked about; the base class answers true for all of them,
    // so the recording wrapper must forward the question and record the answer and arguments.
    FakeEnvironment env;
    env.table_pointers.insert({2, 0x18, 0x60});
    RecordingEnvironment recording{env};
    REQUIRE(recording.IsCbufIndirectStorageBufferPointer(2, 0x18, 0x60));
    REQUIRE_FALSE(recording.IsCbufIndirectStorageBufferPointer(2, 0x20, 0x60));
    REQUIRE(env.table_queries == 2);
    const std::vector<Query> queries = recording.TakeQueries();
    REQUIRE(queries.size() == 2);
    REQUIRE(queries[0].kind == QueryKind::CbufIndirectStorageBufferPointer);
    REQUIRE(queries[0].c == 0x60);
    REQUIRE(ReplayQueries(queries, env));

    // The second slot now holds a pointer: the recorded translation no longer applies.
    env.table_pointers.insert({2, 0x20, 0x60});
    REQUIRE_FALSE(ReplayQueries(queries, env));
    env.table_pointers.erase({2, 0x20, 0x60});

    // The same slot at another holder offset is a different question.
    env.table_pointers.erase({2, 0x18, 0x60});
    env.table_pointers.insert({2, 0x18, 0x68});
    REQUIRE_FALSE(ReplayQueries(queries, env));
    env.table_pointers = {{2, 0x18, 0x60}};

    // The third argument survives the file.
    const auto path = TempPath("lichii_translation_cache_table_test.bin");
    std::filesystem::remove(path);
    const Digest config{0x31, 0x32};
    const Digest stage_key = MakeStageKey(env, 0x77, 5, config);
    ProgramMeta meta{};
    meta.stage = Shader::Stage::Fragment;
    {
        Store store;
        store.Open(path, config);
        REQUIRE(store.Insert(stage_key, queries, meta));
    }
    {
        Store store;
        store.Open(path, config);
        const auto entry = store.FindEntry(stage_key, env);
        REQUIRE(entry);
        REQUIRE(entry->queries == queries);
        env.table_pointers = {{2, 0x18, 0x68}};
        REQUIRE_FALSE(store.FindEntry(stage_key, env));
    }
    std::filesystem::remove(path);
}

TEST_CASE("Translation cache persists entries and variants", "[video_core][translation_cache]") {
    const auto path = TempPath("lichii_translation_cache_test.bin");
    std::filesystem::remove(path);
    const Digest config{0x11, 0x22};
    FakeEnvironment env;
    env.cbufs[(u64{1} << 32) | 0x10] = 0x40;
    const Digest stage_key = MakeStageKey(env, 0x55, 5, config);

    ProgramMeta meta{};
    meta.stage = Shader::Stage::Fragment;
    meta.invocations = 3;
    meta.info = SampleInfo();
    const Shader::Backend::Bindings binding_in{.texture = 2};
    const Digest runtime = DigestRuntimeInfo(Shader::RuntimeInfo{});
    Variant variant{.binding_out = {.unified = 4, .texture = 3}, .info = SampleInfo()};
    variant.info.stores_frag_depth = true;
    for (u32 i = 0; i < 300; ++i) {
        variant.spirv.push_back(0x07230203 + (i % 7));
    }
    Digest variant_key{};
    {
        Store store;
        store.Open(path, config);
        REQUIRE(store.Enabled());
        RecordingEnvironment recording{env};
        Translate(recording);
        const auto entry = store.Insert(stage_key, recording.TakeQueries(), meta);
        REQUIRE(entry);
        variant_key = MakeVariantKey(entry->id, runtime, binding_in);
        store.InsertVariant(*entry, variant_key, variant);
    }
    {
        Store store;
        store.Open(path, config);
        const auto entry = store.FindEntry(stage_key, env);
        REQUIRE(entry);
        REQUIRE(entry->meta.invocations == 3);
        REQUIRE(entry->meta.info.texture_descriptors.size() == 1);
        REQUIRE(MakeVariantKey(entry->id, runtime, binding_in) == variant_key);
        const auto found = store.FindVariant(variant_key);
        REQUIRE(found);
        REQUIRE(found->spirv == variant.spirv);
        REQUIRE(found->binding_out.texture == 3);
        REQUIRE(found->info.stores_frag_depth);
        REQUIRE_FALSE(store.FindVariant(MakeVariantKey(entry->id, runtime, {})));

        // The environment answers a query differently: no entry.
        env.viewport = 0;
        REQUIRE_FALSE(store.FindEntry(stage_key, env));
        env.viewport = 1;
    }
    // A record cut short at the end of the file is dropped; the ones before it are kept.
    {
        std::ofstream file{path, std::ios::binary | std::ios::app};
        const u32 partial[] = {0x52435453, 1000, 7};
        file.write(reinterpret_cast<const char*>(partial), sizeof(partial));
    }
    {
        Store store;
        store.Open(path, config);
        REQUIRE(store.FindEntry(stage_key, env));
        REQUIRE(store.FindVariant(variant_key));
    }
    // Another build or configuration starts over.
    {
        Store store;
        store.Open(path, Digest{0x11, 0x23});
        REQUIRE(store.Enabled());
        REQUIRE_FALSE(store.FindEntry(stage_key, env));
        REQUIRE_FALSE(store.FindVariant(variant_key));
    }
    std::filesystem::remove(path);
}

TEST_CASE("Translation cache wrapper forwards every environment query",
          "[video_core][translation_cache]") {
    // Reads the two headers this test was built from (LICHII_SOURCE_DIR, set by CMake).
    const std::filesystem::path source_dir{LICHII_SOURCE_DIR};
    const std::string environment_header =
        ReadSourceFile(source_dir / "shader_recompiler" / "environment.h");
    const std::string wrapper_header =
        ReadSourceFile(source_dir / "video_core" / "shader_translation_cache.h");
    if (environment_header.empty() || wrapper_header.empty()) {
        SKIP("Sources not found at " << source_dir.string());
    }

    // The scan sees the queries: without IsCbufIndirectStorageBufferPointer's override the
    // wrapper is reported (it was missing until FormatVersion 5).
    const std::string override_line = "IsCbufIndirectStorageBufferPointer(u32 index";
    const size_t line = wrapper_header.find(override_line);
    REQUIRE(line != std::string::npos);
    std::string without_override = wrapper_header;
    without_override.replace(line, override_line.size(), "Removed(u32 index");
    REQUIRE(UnforwardedQueries(environment_header, without_override) ==
            std::set<std::string>{"IsCbufIndirectStorageBufferPointer"});

    // A query virtual added to Shader::Environment must be forwarded and recorded by
    // RecordingEnvironment (and replayed by ReplayQueries, with a FormatVersion bump).
    const std::set<std::string> missing = UnforwardedQueries(environment_header, wrapper_header);
    for (const std::string& name : missing) {
        UNSCOPED_INFO("RecordingEnvironment does not override Shader::Environment::" << name);
    }
    REQUIRE(missing.empty());
}
