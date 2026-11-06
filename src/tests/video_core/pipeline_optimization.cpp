// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/pipeline_optimization.h"

TEST_CASE("Only known AMD proprietary shaders disable pipeline optimization",
          "[video_core][vulkan][pipeline_optimization]") {
    constexpr std::array<u64, 2> hashes{0xeff3ccbe1438679dULL, 0xf9ea14a22394f5b1ULL};
    for (const u64 hash : hashes) {
        CHECK(Vulkan::BuildsUnoptimized(VK_DRIVER_ID_AMD_PROPRIETARY, hash));
        for (const auto driver : {VK_DRIVER_ID_AMD_OPEN_SOURCE, VK_DRIVER_ID_MESA_RADV,
                                  VK_DRIVER_ID_NVIDIA_PROPRIETARY,
                                  VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS,
                                  VK_DRIVER_ID_QUALCOMM_PROPRIETARY, VK_DRIVER_ID_MESA_TURNIP}) {
            CHECK_FALSE(Vulkan::BuildsUnoptimized(driver, hash));
        }
        CHECK_FALSE(Vulkan::BuildsUnoptimized(VK_DRIVER_ID_AMD_PROPRIETARY, hash ^ 1));
    }
    CHECK_FALSE(Vulkan::BuildsUnoptimized(VK_DRIVER_ID_AMD_PROPRIETARY, 0));
    CHECK_FALSE(Vulkan::BuildsUnoptimized(VK_DRIVER_ID_AMD_PROPRIETARY, 0x3c2193d0166610b3ULL));
    for (const u64 hash : {0x8c29514395041853ULL, 0xc332d87961d23b13ULL,
                           0xb2f024003969e2dcULL}) {
        CHECK_FALSE(Vulkan::BuildsUnoptimized(VK_DRIVER_ID_AMD_PROPRIETARY, hash));
    }
}

TEST_CASE("EDEN_NX2_NOOPT_SHADERS parses whole hexadecimal hashes",
          "[video_core][vulkan][pipeline_optimization]") {
    CHECK(Vulkan::ParseNooptShaders("all").all);
    CHECK(Vulkan::ParseNooptShaders("").hashes.empty());
    // Uppercase works; empty, non-hexadecimal and over-long tokens are ignored.
    const auto parsed =
        Vulkan::ParseNooptShaders("74ab167d83c54073,F12162B84E9CC190,,zz,12345678901234567");
    CHECK_FALSE(parsed.all);
    REQUIRE(parsed.hashes.size() == 2);
    CHECK(parsed.hashes[0] == 0x74ab167d83c54073ULL);
    CHECK(parsed.hashes[1] == 0xf12162b84e9cc190ULL);
    CHECK(Vulkan::ParseNooptShaders("c54073").hashes == std::vector<u64>{0xc54073});
}
