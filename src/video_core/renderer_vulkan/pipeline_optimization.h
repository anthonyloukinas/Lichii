// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <string_view>
#include <system_error>
#include <vector>

#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan.h"

namespace Vulkan {

/// EDEN_NX2_NOOPT_SHADERS (#210): "all", or comma-separated hexadecimal shader hashes in either
/// case. A diagnostic: those pipelines are built unoptimized on any driver.
struct NooptShaders {
    bool all = false;
    std::vector<u64> hashes;
};

inline NooptShaders ParseNooptShaders(std::string_view text) {
    NooptShaders result;
    if (text == "all") {
        result.all = true;
        return result;
    }
    for (size_t begin = 0; begin < text.size();) {
        const size_t end = (std::min)(text.find(',', begin), text.size());
        const std::string_view token = text.substr(begin, end - begin);
        u64 hash{};
        const auto [last, error] =
            std::from_chars(token.data(), token.data() + token.size(), hash, 16);
        if (!token.empty() && error == std::errc{} && last == token.data() + token.size()) {
            result.hashes.push_back(hash);
        }
        begin = end + 1;
    }
    return result;
}

inline bool BuildsUnoptimized(VkDriverId driver_id, u64 shader_hash) {
    static const NooptShaders forced = [] {
        const char* const value = std::getenv("EDEN_NX2_NOOPT_SHADERS");
        return ParseNooptShaders(value != nullptr ? value : "");
    }();
    if (forced.all || std::ranges::find(forced.hashes, shader_hash) != forced.hashes.end()) {
        return true;
    }
    // The Legend of Zelda: Ocarina of Time (0400486024DEC000), driver 2.0.353 on an RX 6600 XT
    static constexpr std::array<u64, 2> amd_proprietary{
        0xeff3ccbe1438679dULL, // compute shader built as the AV1 intro starts: 6 min (#200)
        0xf9ea14a22394f5b1ULL, // compute shader built as the AV1 intro starts: 18 min (#200)
    };
    return driver_id == VK_DRIVER_ID_AMD_PROPRIETARY &&
           std::ranges::find(amd_proprietary, shader_hash) != amd_proprietary.end();
}

} // namespace Vulkan
