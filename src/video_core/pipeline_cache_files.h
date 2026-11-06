// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string_view>
#include <system_error>
#include <unordered_map>

#include <fmt/format.h>

#include "common/common_types.h"

namespace VideoCommon {

inline constexpr u32 OpenGLPipelineCacheVersion = 48;
inline constexpr u32 VulkanPipelineCacheVersion = 53;

// Official Eglantine (cache version 18 at c0cb3325) and this fork (a different version) both
// used <shader dir>/<title>/vulkan.bin and vulkan_pipelines.bin. Each build deletes a file
// with another version at boot, so alternating builds made every run start with no pipelines
// (MKW log: "Deleting old pipeline cache", "Total Pipeline Count: 0", then hundreds of
// pipelines built on the GPU thread). Version-suffixed names let the builds coexist.
[[nodiscard]] inline std::filesystem::path VersionedPipelineCachePath(
    const std::filesystem::path& directory, std::string_view stem, u32 version) {
    return directory / fmt::format("{}.fork-v{}.bin", stem, version);
}

/// True when the file starts with this magic number followed by this cache version.
[[nodiscard]] inline bool PipelineCacheHeaderMatches(const std::filesystem::path& file,
                                                     const std::array<char, 8>& magic,
                                                     u32 version) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream.is_open()) {
        return false;
    }
    std::array<char, 8> file_magic{};
    u32 file_version{};
    stream.read(file_magic.data(), file_magic.size());
    stream.read(reinterpret_cast<char*>(&file_version), sizeof(file_version));
    return stream.good() && file_magic == magic && file_version == version;
}

/// Moves a legacy-named cache that this exact cache version wrote to its versioned name, so an
/// existing warm cache survives the rename. A file of any other version (another build's) is
/// never touched; neither is an existing versioned file.
inline bool AdoptLegacyPipelineCache(const std::filesystem::path& legacy,
                                     const std::filesystem::path& versioned,
                                     const std::array<char, 8>& magic, u32 version) {
    std::error_code error;
    if (std::filesystem::exists(versioned, error) || error) {
        return false;
    }
    if (!PipelineCacheHeaderMatches(legacy, magic, version)) {
        return false;
    }
    std::filesystem::rename(legacy, versioned, error);
    return !error;
}

inline bool CopyCompatiblePipelineCache(const std::filesystem::path& source,
                                        const std::filesystem::path& destination,
                                        const std::array<char, 8>& magic, u32 source_version,
                                        u32 destination_version) {
    std::error_code error;
    if (std::filesystem::exists(destination, error) || error ||
        !PipelineCacheHeaderMatches(source, magic, source_version)) {
        return false;
    }
    auto temporary = destination;
    temporary += ".migrating";
    if (!std::filesystem::copy_file(source, temporary,
                                    std::filesystem::copy_options::overwrite_existing, error)) {
        return false;
    }
    {
        std::fstream stream(temporary, std::ios::binary | std::ios::in | std::ios::out);
        stream.seekp(static_cast<std::streamoff>(magic.size()));
        stream.write(reinterpret_cast<const char*>(&destination_version),
                     sizeof(destination_version));
        stream.close();
        if (stream.fail()) {
            std::filesystem::remove(temporary, error);
            return false;
        }
    }
    std::filesystem::rename(temporary, destination, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}

/// Which pipelines the transferable cache holds, so that a live build is appended only when the
/// cache has no record for it, or when the stored capture failed to build (it lacks an answer
/// the translation now asks for). Such a replacement follows the old record and is the one
/// loaded later (LoadPipelines with a PipelineRecordIndex). Thread-safe: failures are noted by
/// the boot and background build workers. Keys are told apart by their own equality; KeyHash
/// only looks them up.
template <typename Key, typename KeyHash = std::hash<Key>>
class DiskPipelineRecords {
public:
    /// No replacement is written once a key has this many records (one that keeps failing does
    /// not grow the file on every launch).
    static constexpr u32 MaxRecordsPerKey = 4;

    enum class Write { No, New, Replacement };

    /// The cache holds `records` records for this key, of which the newest was loaded. False when
    /// the key was noted already.
    bool NoteStored(const Key& key, u32 records) {
        std::scoped_lock lock{mutex};
        return keys.try_emplace(key, State{.records = records}).second;
    }

    /// The stored capture of this key failed to build.
    void NoteFailed(const Key& key) {
        std::scoped_lock lock{mutex};
        const auto it = keys.find(key);
        if (it != keys.end() && it->second.records < MaxRecordsPerKey) {
            it->second.failed = true;
        }
    }

    /// A live build of this key succeeded: whether its capture is to be appended to the cache.
    Write ClaimWrite(const Key& key) {
        std::scoped_lock lock{mutex};
        const auto [it, inserted] = keys.try_emplace(key, State{.records = 1});
        if (inserted) {
            return Write::New;
        }
        if (!it->second.failed) {
            return Write::No;
        }
        it->second.failed = false;
        ++it->second.records;
        return Write::Replacement;
    }

private:
    struct State {
        u32 records{};
        bool failed{};
    };
    std::mutex mutex;
    std::unordered_map<Key, State, KeyHash> keys;
};

} // namespace VideoCommon
