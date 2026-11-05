// SPDX-FileCopyrightText: 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/device_memory_manager.inc"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/gpu_split.h"
#include "video_core/nx2_hot_pages.h"
#include "video_core/rasterizer_interface.h"

namespace Tegra {

struct MaxwellDeviceMethods {
    static inline void MarkRegionCaching(Core::Memory::Memory* interface, VAddr address,
                                         size_t size, bool caching) {
        auto& counters = VideoCommon::Nx2HotPages::g_counters;
        const u64 pages = (size + Core::Memory::YUZU_PAGESIZE - 1) >> Core::Memory::YUZU_PAGEBITS;
        if (caching) {
            VideoCommon::Nx2HotPages::Count(counters.protect_calls, 1);
            VideoCommon::Nx2HotPages::Count(counters.protect_pages, pages);
            if (VideoCommon::GpuSplit::OnBackThread()) {
                VideoCommon::Nx2HotPages::Count(counters.back_protect_calls, 1);
            }
        } else {
            VideoCommon::Nx2HotPages::Count(counters.unprotect_calls, 1);
            VideoCommon::Nx2HotPages::Count(counters.unprotect_pages, pages);
        }
        interface->RasterizerMarkRegionCached(address, size, caching);
    }

    static inline bool MarkRegionReadTrap(Core::Memory::Memory* interface, VAddr address,
                                          size_t size) {
        return interface->RasterizerArmReadTraps(address, size);
    }

    static inline bool ReadTrapsActive() {
        return Core::Memory::Memory::RasterizerReadTrapsActive();
    }
};

} // namespace Tegra

template struct Core::DeviceMemoryManagerAllocator<Tegra::MaxwellDeviceTraits>;
template class Core::DeviceMemoryManager<Tegra::MaxwellDeviceTraits>;

template const u8* Tegra::MaxwellDeviceMemoryManager::GetPointer<u8>(DAddr addr) const;
template u8* Tegra::MaxwellDeviceMemoryManager::GetPointer<u8>(DAddr addr);

template u8 Tegra::MaxwellDeviceMemoryManager::Read<u8>(DAddr addr) const;
template u16 Tegra::MaxwellDeviceMemoryManager::Read<u16>(DAddr addr) const;
template u32 Tegra::MaxwellDeviceMemoryManager::Read<u32>(DAddr addr) const;
template u64 Tegra::MaxwellDeviceMemoryManager::Read<u64>(DAddr addr) const;
template void Tegra::MaxwellDeviceMemoryManager::Write<u8>(DAddr addr, u8 data);
template void Tegra::MaxwellDeviceMemoryManager::Write<u16>(DAddr addr, u16 data);
template void Tegra::MaxwellDeviceMemoryManager::Write<u32>(DAddr addr, u32 data);
template void Tegra::MaxwellDeviceMemoryManager::Write<u64>(DAddr addr, u64 data);