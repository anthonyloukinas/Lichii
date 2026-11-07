// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstring>

namespace Vulkan {

/// Live graphics pipelines below which none is evicted, and up to which the background prebuild
/// fills (prebuilds no draw took yet count as live). At ~0.45 MB of driver memory each, 8,000 is
/// ~3.6 GB. EDEN_NX2_PIPELINE_EVICT_ABOVE=<n> overrides it; 0 evicts every pipeline unused for the
/// eviction window, as before (the prebuild then still stops at 8,000).
inline constexpr size_t DefaultPipelineEvictAbove = 8000;

/// Parses a non-negative decimal count from an environment value; `fallback` when it is unset or
/// not a number.
[[nodiscard]] inline size_t ParseEnvCount(const char* value, size_t fallback) noexcept {
    if (value == nullptr || value[0] == '\0') {
        return fallback;
    }
    size_t result{};
    const char* const end = value + std::strlen(value);
    const auto [ptr, error] = std::from_chars(value, end, result);
    return error == std::errc{} && ptr == end ? result : fallback;
}

/// How many of `stale` eviction candidates to destroy with `resident` live pipelines: none at or
/// below `threshold`, otherwise the least recently used until `threshold` remain (all of them
/// with a threshold of 0).
[[nodiscard]] constexpr size_t PipelinesToEvict(size_t resident, size_t stale,
                                                size_t threshold) noexcept {
    if (resident <= threshold) {
        return 0;
    }
    return std::min(stale, resident - threshold);
}

} // namespace Vulkan
