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
#include <utility>

#include <fmt/format.h>

#include "common/common_types.h"
#include "common/unique_function.h"

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
/// loaded later (LoadPipelines with a PipelineRecordIndex). It is appended only if it builds as a
/// later boot reads it back (AppendCheckedPipeline), so a capture that keeps failing is never
/// written again, however many older records the key has. Thread-safe: stored captures are built
/// by the boot and background workers while the GPU thread builds live. Keys are told apart by
/// their own equality; KeyHash only looks them up.
template <typename Key, typename KeyHash = std::hash<Key>>
class DiskPipelineRecords {
public:
    /// Deferred: the stored capture is still being built; the live capture is kept and written
    /// as a replacement if that build fails.
    enum class Write { No, New, Replacement, Deferred };

    /// Queues the write of a live capture (called once, with New or Replacement).
    using Writer = Common::UniqueFunction<void, Write>;

    /// A build of the stored capture of a key, from StartBuild. Finish reports its outcome; one
    /// destroyed unfinished (cancelled, or never run because the GPU thread took the key first)
    /// reports nothing about the capture.
    class Build {
    public:
        Build() = default;
        Build(DiskPipelineRecords* records_, const Key& key_) : records{records_}, key{key_} {}
        Build(Build&& other) noexcept
            : records{std::exchange(other.records, nullptr)}, key{other.key} {}
        Build& operator=(Build&&) = delete;
        Build(const Build&) = delete;
        Build& operator=(const Build&) = delete;
        ~Build() {
            if (records) {
                records->EndBuild(key, Outcome::Unknown);
            }
        }

        /// built: the stored capture gave a pipeline. A failure runs a deferred live write.
        void Finish(bool built) {
            if (auto* const owner = std::exchange(records, nullptr)) {
                owner->EndBuild(key, built ? Outcome::Built : Outcome::Failed);
            }
        }

    private:
        DiskPipelineRecords* records{};
        Key key{};
    };

    /// The cache holds a record of this key (the newest was loaded). False when the key was noted
    /// already.
    bool NoteStored(const Key& key) {
        std::scoped_lock lock{mutex};
        return keys.try_emplace(key).second;
    }

    /// The stored capture of this key failed to build.
    void NoteFailed(const Key& key) {
        EndBuild(key, Outcome::Failed);
    }

    /// The stored capture of this key is about to be built on another thread.
    [[nodiscard]] Build StartBuild(const Key& key) {
        std::scoped_lock lock{mutex};
        if (const auto it = keys.find(key); it != keys.end()) {
            ++it->second.building;
        }
        return Build{this, key};
    }

    /// A live build of this key succeeded: whether its capture is to be appended to the cache.
    /// New and Replacement: the caller runs writer. Deferred: writer was taken and runs if the
    /// stored capture's build in flight fails (never on the calling thread's account, so the GPU
    /// thread does not wait for it). One replacement per noted failure.
    Write ClaimWrite(const Key& key, Writer& writer) {
        std::scoped_lock lock{mutex};
        const auto [it, inserted] = keys.try_emplace(key);
        State& state = it->second;
        if (inserted) {
            return Write::New;
        }
        if (state.failed) {
            state.failed = false;
            return Write::Replacement;
        }
        if (state.building != 0 && !state.deferred && writer) {
            state.deferred = std::move(writer);
            return Write::Deferred;
        }
        return Write::No;
    }

    Write ClaimWrite(const Key& key) {
        Writer none;
        return ClaimWrite(key, none);
    }

    /// The write ClaimWrite granted stored nothing (the capture cannot be stored, or the file
    /// failed): a later live build of the key may claim it again.
    void ReturnWrite(const Key& key, Write write) {
        std::scoped_lock lock{mutex};
        if (write == Write::New) {
            keys.erase(key);
        } else if (write == Write::Replacement) {
            const auto it = keys.find(key);
            if (it != keys.end()) {
                it->second.failed = true;
            }
        }
    }

private:
    enum class Outcome { Unknown, Built, Failed };

    void EndBuild(const Key& key, Outcome outcome) {
        Writer writer;
        {
            std::scoped_lock lock{mutex};
            const auto it = keys.find(key);
            if (it == keys.end()) {
                return;
            }
            State& state = it->second;
            if (state.building != 0) {
                --state.building;
            }
            if (outcome == Outcome::Failed) {
                if (state.deferred) {
                    writer = std::move(state.deferred);
                } else {
                    state.failed = true;
                }
            }
            if (state.building == 0) {
                // Built, or not built at all: the live capture is not needed.
                state.deferred = {};
            }
        }
        if (writer) {
            writer(Write::Replacement);
        }
    }

    struct State {
        u32 building{};
        bool failed{};
        Writer deferred;
    };
    std::mutex mutex;
    std::unordered_map<Key, State, KeyHash> keys;
};

} // namespace VideoCommon
