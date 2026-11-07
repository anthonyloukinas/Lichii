// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <vector>

#include "common/common_types.h"
#include "core/device_memory_manager.h"

namespace VideoCommon {

/// What BindHostGraphicsUniformBuffer last uploaded of each device page of a native program's
/// 64 KiB uniform bank, so that the once-per-frame re-upload sends only the parts whose guest
/// bytes changed since. The re-upload exists because writes to such a bank can escape the memory
/// tracker (6053738ae2); the bytes themselves show whether anything was written.
///
/// An entry holds the bytes [lo, hi) of its page as they are in one host buffer (owner). The
/// buffer cache invalidates the entries of every range any other path writes to a host buffer
/// (tracked uploads, inline writes, GPU writes, DMA copies and clears, buffer deletion), so an
/// entry's bytes are always the host buffer's. When the guest bytes equal them, the upload
/// would copy the same bytes again.
class NativeBankSnapshots {
public:
    static constexpr u32 PAGE_SIZE = static_cast<u32>(Core::DEVICE_PAGESIZE);
    /// 32 MiB of snapshots at most; past that all are dropped (the next checks upload again).
    static constexpr size_t MAX_PAGES = 8192;

    /// True when [lo, hi) of `page` is held for `owner` and equals `guest` (the guest bytes at
    /// page + lo).
    [[nodiscard]] bool Unchanged(DAddr page, u32 owner, u32 lo, u32 hi, const u8* guest) const {
        const auto it = index.find(page);
        if (it == index.end()) {
            return false;
        }
        const Entry& entry = slots[it->second];
        return entry.owner == owner && entry.lo <= lo && hi <= entry.hi &&
               std::memcmp(entry.bytes.data() + lo, guest, hi - lo) == 0;
    }

    /// Takes [lo, hi) of `page` from `guest` as the bytes about to be uploaded to `owner`'s
    /// buffer, and returns the slot to copy them from (stable until the next Record call that
    /// clears, Invalidate or Clear). Upload from the slot, not from guest memory again: the guest
    /// bytes can change in between, and the entry must hold exactly what the host buffer got.
    u32 Record(DAddr page, u32 owner, u32 lo, u32 hi, const u8* guest) {
        auto [it, inserted] = index.try_emplace(page, 0);
        if (inserted) {
            if (slots.capacity() == 0) {
                slots.reserve(MAX_PAGES);
            }
            if (free_slots.empty()) {
                it->second = static_cast<u32>(slots.size());
                slots.emplace_back();
            } else {
                it->second = free_slots.back();
                free_slots.pop_back();
            }
            begin = (std::min)(begin, page);
            end = (std::max)(end, page + PAGE_SIZE);
        }
        Entry& entry = slots[it->second];
        // The bytes held so far stay valid next to the new ones when the two ranges touch
        // (nothing wrote the buffer there, or the entry would be gone).
        if (inserted || entry.owner != owner || hi < entry.lo || entry.hi < lo) {
            entry.owner = owner;
            entry.lo = lo;
            entry.hi = hi;
        } else {
            entry.lo = (std::min)(entry.lo, lo);
            entry.hi = (std::max)(entry.hi, hi);
        }
        std::memcpy(entry.bytes.data() + lo, guest, hi - lo);
        return it->second;
    }

    [[nodiscard]] const u8* Bytes(u32 slot) const {
        return slots[slot].bytes.data();
    }

    /// The host buffer bytes of [addr, addr + size) were written by another path.
    void Invalidate(DAddr addr, u64 size) {
        if (index.empty() || size == 0 || addr >= end || addr + size <= begin) [[likely]] {
            return;
        }
        const DAddr first = (std::max)(addr & ~static_cast<DAddr>(PAGE_SIZE - 1), begin);
        const DAddr last = (std::min)(addr + size, end);
        if ((last - first) / PAGE_SIZE > index.size()) {
            for (auto it = index.begin(); it != index.end();) {
                if (it->first + PAGE_SIZE > addr && it->first < addr + size) {
                    free_slots.push_back(it->second);
                    it = index.erase(it);
                } else {
                    ++it;
                }
            }
            return;
        }
        for (DAddr page = first; page < last; page += PAGE_SIZE) {
            if (const auto it = index.find(page); it != index.end()) {
                free_slots.push_back(it->second);
                index.erase(it);
            }
        }
    }

    /// Makes room for `pages` more entries; drops all of them when that would pass MAX_PAGES.
    /// Returns true when it dropped them.
    bool Reserve(size_t pages) {
        if (index.size() + pages <= MAX_PAGES) {
            return false;
        }
        Clear();
        return true;
    }

    void Clear() {
        index.clear();
        free_slots.clear();
        slots.clear();
        begin = std::numeric_limits<DAddr>::max();
        end = 0;
    }

    [[nodiscard]] size_t Size() const {
        return index.size();
    }

private:
    struct Entry {
        u32 owner{};
        u32 lo{};
        u32 hi{};
        std::array<u8, PAGE_SIZE> bytes;
    };

    std::unordered_map<DAddr, u32> index;
    std::vector<Entry> slots;
    std::vector<u32> free_slots;
    DAddr begin = std::numeric_limits<DAddr>::max();
    DAddr end = 0;
};

} // namespace VideoCommon
