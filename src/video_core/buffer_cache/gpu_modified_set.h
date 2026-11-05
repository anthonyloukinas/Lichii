// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <memory>

#include "common/common_types.h"
#include "common/range_sets.h"

namespace VideoCommon {

/// The buffer cache's GPU-modified ranges, with a lock-free summary: one bit per 64 KiB granule
/// of device memory that may hold a GPU-modified range. Writers hold the buffer cache's mutex as
/// before; MayIntersect needs no lock. A bit is set before its range is added and cleared only
/// after no range is left in its granule, so a clear bit means the granule holds none (false
/// positives only: the caller then asks Intersects under the lock).
class GpuModifiedRangeSet : public Common::RangeSet<DAddr> {
public:
    static constexpr u32 GranuleBits = 16;
    static constexpr u32 AddressBits = 36;
    static constexpr u64 Granules = u64{1} << (AddressBits - GranuleBits);

    GpuModifiedRangeSet() : bits{std::make_unique<std::atomic<u64>[]>(Granules / 64)} {}

    void Add(DAddr address, size_t size) {
        if (size != 0) {
            ForEachGranuleWord(address, size, [](std::atomic<u64>& word, u64 mask) {
                if ((word.load(std::memory_order_relaxed) & mask) != mask) {
                    word.fetch_or(mask, std::memory_order_seq_cst);
                }
            });
        }
        Common::RangeSet<DAddr>::Add(address, size);
    }

    void Subtract(DAddr address, size_t size) {
        Common::RangeSet<DAddr>::Subtract(address, size);
        if (size == 0) {
            return;
        }
        ForEachGranuleWord(address, size, [this](std::atomic<u64>& word, u64 mask) {
            const u64 base = static_cast<u64>(&word - bits.get()) * 64;
            if (base + 64 == Granules) {
                return; // Also stands for addresses past the tracked space: kept set.
            }
            u64 set = word.load(std::memory_order_relaxed) & mask;
            while (set != 0) {
                const u32 bit = static_cast<u32>(std::countr_zero(set));
                set &= set - 1;
                const DAddr granule = (base + bit) << GranuleBits;
                if (!Intersects(granule, size_t{1} << GranuleBits)) {
                    word.fetch_and(~(u64{1} << bit), std::memory_order_seq_cst);
                }
            }
        });
    }

    void Clear() {
        Common::RangeSet<DAddr>::Clear();
        for (u64 i = 0; i < Granules / 64; ++i) {
            bits[i].store(0, std::memory_order_seq_cst);
        }
    }

    /// Any thread, no lock: false when no GPU-modified range overlaps [address, address + size).
    [[nodiscard]] bool MayIntersect(DAddr address, size_t size) const {
        if (size == 0) {
            return false;
        }
        bool found = false;
        ForEachGranuleWord(address, size, [&found](const std::atomic<u64>& word, u64 mask) {
            found |= (word.load(std::memory_order_seq_cst) & mask) != 0;
        });
        return found;
    }

private:
    /// Calls func(word, mask) for each bitmap word holding granules of the range; addresses past
    /// the tracked space map to every bit of the last word (conservative).
    template <typename Func>
    void ForEachGranuleWord(DAddr address, size_t size, Func&& func) const {
        u64 first = address >> GranuleBits;
        u64 last = (address + size - 1) >> GranuleBits;
        if (last < first || last >= Granules) {
            last = Granules - 1;
            first = (std::min)(first, last & ~u64{63});
        }
        for (u64 index = first / 64; index <= last / 64; ++index) {
            const u64 lo = index == first / 64 ? first % 64 : 0;
            const u64 hi = index == last / 64 ? last % 64 : 63;
            const u64 mask = (hi == 63 ? ~u64{0} : (u64{1} << (hi + 1)) - 1) & (~u64{0} << lo);
            func(bits[index], mask);
        }
    }

    std::unique_ptr<std::atomic<u64>[]> bits;
};

} // namespace VideoCommon
