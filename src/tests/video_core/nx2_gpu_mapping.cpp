// SPDX-License-Identifier: GPL-3.0-or-later

#include <map>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/hle/service/nvdrv/devices/nx2_stub_device.h"

TEST_CASE("NX2 presentation resolves parent handles and image offsets", "[video_core][nx2_mapping]") {
    Service::Nvidia::Devices::Nx2GpuMemoryRegistry registry;
    registry.MapAt(7, 0x1000, 0x8000, 0x4000, 0x3000);
    registry.RecordDeviceRange(0x4000, 0x3000, 0x10000);
    REQUIRE(registry.GpuAddressOf(7, 0x1800) == 0x8800U);
    REQUIRE(registry.DeviceAddressOfGpu(0x8800) == 0x10800U);
    REQUIRE_FALSE(registry.GpuAddressOf(7, 0xfff));
    REQUIRE_FALSE(registry.GpuAddressOf(7, 0x4000));
    REQUIRE_FALSE(registry.GpuAddressOf(8, 0x1800));
    REQUIRE_FALSE(registry.DeviceAddressOfGpu(0xb000));
}

TEST_CASE("NX2 partial unmaps preserve remaining parent offsets", "[video_core][nx2_mapping]") {
    Service::Nvidia::Devices::Nx2GpuMemoryRegistry registry;
    registry.MapAt(7, 0x1000, 0x8000, 0x4000, 0x3000);
    registry.RecordDeviceRange(0x4000, 0x3000, 0x10000);
    registry.Unmap(0x9000, 0x1000);
    REQUIRE(registry.GpuAddressOf(7, 0x1000) == 0x8000U);
    REQUIRE_FALSE(registry.GpuAddressOf(7, 0x2000));
    REQUIRE_FALSE(registry.DeviceAddressOfGpu(0x9000));
    REQUIRE(registry.GpuAddressOf(7, 0x3000) == 0xa000U);
    REQUIRE(registry.DeviceAddressOfGpu(0xa000) == 0x12000U);
}

TEST_CASE("NX2 remaps and resets discard stale parent handles", "[video_core][nx2_mapping]") {
    Service::Nvidia::Devices::Nx2GpuMemoryRegistry registry;
    registry.MapAt(7, 0, 0x8000, 0x4000, 0x1000);
    registry.MapAt(8, 0x1000, 0x8000, 0x6000, 0x1000);
    registry.RecordDeviceRange(0x6000, 0x1000, 0x20000);
    REQUIRE_FALSE(registry.GpuAddressOf(7, 0));
    REQUIRE(registry.GpuAddressOf(8, 0x1000) == 0x8000U);
    REQUIRE(registry.DeviceAddressOfGpu(0x8000) == 0x20000U);
    registry.Reset();
    REQUIRE_FALSE(registry.GpuAddressOf(8, 0x1000));
    REQUIRE_FALSE(registry.DeviceAddressOfGpu(0x8000));
}

TEST_CASE("NX2 destroyed memory leaves no GPU range mapped into it", "[video_core][nx2_mapping]") {
    Service::Nvidia::Devices::Nx2GpuMemoryRegistry registry;
    std::vector<std::pair<u64, u64>> unmapped;
    registry.AddListener(
        &unmapped, [](u64, u64, u64) {},
        [&unmapped](u64 gpu_address, u64 size) { unmapped.emplace_back(gpu_address, size); });
    registry.Register(0x10000, 0x4000);
    registry.MapAt(5, 0x1000, 0x80000, 0x11000, 0x2000);
    registry.MapAt(6, 0, 0x90000, 0x20000, 0x1000);
    registry.MapAt(7, 0, 0xa0000, 0x13000, 0x3000);

    const auto leftovers = registry.UnmapMemory(0x10000, 0x4000);

    const std::vector<std::pair<u64, u64>> expected_leftovers{{0x80000, 0x2000}, {0xa0000, 0x1000}};
    REQUIRE(leftovers == expected_leftovers);
    const std::vector<std::pair<u64, u64>> expected_unmapped{
        {0x10000, 0x4000}, {0x80000, 0x2000}, {0xa0000, 0x1000}};
    REQUIRE(unmapped == expected_unmapped);
    REQUIRE_FALSE(registry.GpuAddressOf(5, 0x1000));
    REQUIRE(registry.GpuAddressOf(6, 0) == 0x90000U);
    // Only the part of a mapping that reached the destroyed memory goes.
    REQUIRE_FALSE(registry.GpuAddressOf(7, 0));
    REQUIRE(registry.GpuAddressOf(7, 0x1000) == 0xa1000U);
}

TEST_CASE("NX2 destroyed memory gets a new device range when registered again",
          "[video_core][nx2_mapping]") {
    Service::Nvidia::Devices::Nx2GpuMemoryRegistry registry;
    // Like nvhost_as_gpu: device ranges cached by CPU range, shared through the registry.
    std::map<std::pair<u64, u64>, u64> ranges;
    u64 next_device = 0x100000;
    const auto allocate = [&next_device] {
        const u64 device_address = next_device;
        next_device += 0x20000;
        return device_address;
    };
    registry.AddListener(
        &ranges,
        [&](u64, u64 cpu_address, u64 size) {
            const std::pair key{cpu_address, size};
            if (!ranges.contains(key)) {
                ranges[key] = registry.AcquireDeviceRange(1, cpu_address, size, allocate);
            }
            registry.RecordDeviceRange(cpu_address, size, ranges[key]);
        },
        [](u64, u64) {},
        [&](u64 cpu_address, u64 size) {
            if (ranges.erase(std::pair{cpu_address, size}) != 0) {
                registry.ReleaseDeviceRange(1, cpu_address, size);
            }
        });
    registry.Register(0x10000, 0x4000);
    const u64 first = ranges.at({0x10000, 0x4000});
    REQUIRE(registry.DeviceAddressOf(0x10000) == first);

    // Sub 102, then the game registers new memory at the same address.
    registry.UnmapMemory(0x10000, 0x4000);
    registry.ReleaseBacking(0x10000, 0x4000);
    REQUIRE(ranges.empty());
    REQUIRE_FALSE(registry.DeviceAddressOf(0x10000));
    registry.Register(0x10000, 0x4000);

    const u64 second = ranges.at({0x10000, 0x4000});
    REQUIRE(second != first);
    REQUIRE(registry.DeviceAddressOf(0x10000) == second);
}

TEST_CASE("NX2 index check describes mappings and their history", "[video_core][nx2_mapping]") {
    Service::Nvidia::Devices::Nx2GpuMemoryRegistry registry{true};
    registry.MapAt(7, 0x1000, 0x8000, 0x4000, 0x3000);
    registry.Register(0x20000, 0x1000);
    REQUIRE(registry.Describe(0x8800, 0x100).starts_with(
        "[0x8000, +0x3000] mapped by sub 211 handle 7 offset 0x1000 cpu 0x4000"));
    REQUIRE(registry.Describe(0x20000, 0x10).starts_with("[0x20000, +0x1000] mapped by sub 101"));

    registry.Unmap(0x9000, 0x1000, 213);
    registry.MapAt(0xa000, 0x30000, 0x1000, 216);
    registry.UnmapMemory(0x20000, 0x1000);
    const std::string unmapped = registry.Describe(0x9000, 0x1000);
    REQUIRE(unmapped.starts_with("not mapped; [0x9000, +0x1000] unmapped by sub 213"));
    REQUIRE(unmapped.find("(was sub 211 handle 7 cpu 0x5000)") != std::string::npos);
    const std::string remapped = registry.Describe(0xa000, 0x1000);
    REQUIRE(remapped.starts_with("[0xa000, +0x1000] mapped by sub 216 handle 0 offset 0x0 cpu 0x30000"));
    REQUIRE(remapped.find("remapped by sub 216") != std::string::npos);
    REQUIRE(remapped.find("(was sub 211 handle 7 cpu 0x6000)") != std::string::npos);
    REQUIRE(registry.Describe(0x20000, 0x1000).starts_with(
        "not mapped; [0x20000, +0x1000] destroyed by sub 102"));

    for (u64 i = 0; i < 5000; ++i) {
        registry.MapSparseTile(0x100000 + i * 0x1000, 0, 0x1000);
    }
    REQUIRE(registry.Describe(0x9000, 0x1000) == "not mapped");
    REQUIRE(registry.Describe(0x100000 + 4999 * 0x1000, 0x1000).starts_with(
        "not mapped; [0x1487000, +0x1000] tile unmapped by sub 216"));
}

TEST_CASE("NX2 registry keeps no history unless the index check is on", "[video_core][nx2_mapping]") {
    Service::Nvidia::Devices::Nx2GpuMemoryRegistry registry{false};
    registry.MapAt(7, 0, 0x8000, 0x4000, 0x1000);
    registry.Unmap(0x8000, 0x1000, 213);
    REQUIRE(registry.Describe(0x8000, 0x1000) == "not mapped");
}
