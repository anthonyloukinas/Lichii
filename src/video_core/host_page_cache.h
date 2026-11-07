// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>

#include "common/common_types.h"

namespace VideoCommon {

/// EDEN_NX2_HANDLE_PTR_CACHE: host pointers of the 4 KiB GPU pages one thread read last through
/// one GPU memory manager (Tegra::MemoryManager::CachedPagePointer). A pointer stays valid while
/// the manager's map serial and the device memory's translation generation are what they were
/// when it was looked up: every GPU map, remap or unmap bumps the first, every device Map or
/// Unmap the second, and the host memory behind a mapped device page does not move.
class HostPageCache {
public:
    static constexpr u64 PageBits = 12;
    static constexpr u64 PageMask = (u64{1} << PageBits) - 1;
    static constexpr size_t Slots = 16;

    /// The host pointer of `addr`. `owner` identifies the manager (never reused), the serials
    /// are read before the call. `lookup(page)` returns the host pointer of the page at `page`,
    /// or nullptr (unmapped, not kept). `hit` tells whether no lookup was needed.
    template <typename Lookup>
    const u8* Get(size_t owner, u64 map_serial, u64 device_generation, GPUVAddr addr,
                  Lookup&& lookup, bool& hit) {
        if (owner_key != owner + 1 || serial != map_serial ||
            generation != device_generation) {
            *this = HostPageCache{};
            owner_key = owner + 1;
            serial = map_serial;
            generation = device_generation;
        }
        const u64 page = addr >> PageBits;
        const size_t slot = static_cast<size_t>(page) & (Slots - 1);
        if (pages[slot] == page + 1) {
            hit = true;
            return hosts[slot] + (addr & PageMask);
        }
        hit = false;
        const u8* const host = lookup(addr & ~PageMask);
        if (host == nullptr) {
            return nullptr;
        }
        pages[slot] = page + 1;
        hosts[slot] = host;
        return host + (addr & PageMask);
    }

private:
    size_t owner_key{}; ///< Owner + 1; 0: empty
    u64 serial{};
    u64 generation{};
    std::array<u64, Slots> pages{}; ///< GPU page + 1 per slot; 0: empty
    std::array<const u8*, Slots> hosts{};
};

} // namespace VideoCommon
