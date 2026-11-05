// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>

#include "common/common_types.h"

namespace VideoCommon {

/// The GPU memory ranges the split GPU thread's front sent writes to (constant buffer and inline
/// uploads, reports, semaphore releases), each with the ring position right after the method
/// that writes it. A front read of guest memory then waits only for the queued writes that
/// overlap it (GpuSplit::MemoryTarget). Bounded: once a range falls out of the log, the newest
/// position among the ones that fell out bounds every read.
class SplitWriteLog {
public:
    static constexpr size_t Capacity = 512;
    /// A write that continues the last range joins it up to this size (contiguous uploads).
    static constexpr u64 MaxMerged = 0x10000;

    void Note(GPUVAddr begin, u64 size, u64 position) {
        if (size == 0) {
            return;
        }
        const GPUVAddr end = begin + size;
        newest_position = position;
        if (count != 0) {
            Entry& last = entries[(count - 1) % Capacity];
            if (begin <= last.end && last.begin <= end &&
                std::max(end, last.end) - std::min(begin, last.begin) <= MaxMerged) {
                last.begin = std::min(begin, last.begin);
                last.end = std::max(end, last.end);
                last.position = position;
                return;
            }
        }
        Entry& slot = entries[count % Capacity];
        if (count >= Capacity) {
            evicted_position = std::max(evicted_position, slot.position);
        }
        slot = {begin, end, position};
        ++count;
    }

    /// The position the back must reach before [begin, begin + size) holds every write the log
    /// knows of: at least `floor`, and nothing at or below `consumed` is looked at. `overlap`
    /// tells whether a logged range (or the overflow bound) raised the floor.
    [[nodiscard]] u64 Target(GPUVAddr begin, u64 size, u64 floor, u64 consumed,
                             bool* overlap = nullptr) const {
        u64 target = floor;
        bool raised = false;
        if (evicted_position > target && evicted_position > consumed) {
            target = evicted_position;
            raised = true;
        }
        const GPUVAddr end = begin + size;
        const size_t oldest = count > Capacity ? count - Capacity : 0;
        // Positions grow with the index: the newest overlapping range is the latest one, and
        // nothing older can raise the target once positions fall to it (or to `consumed`).
        for (size_t i = count; i > oldest; --i) {
            const Entry& entry = entries[(i - 1) % Capacity];
            if (entry.position <= target || entry.position <= consumed) {
                break;
            }
            if (entry.begin < end && begin < entry.end) {
                target = entry.position;
                raised = true;
                break;
            }
        }
        if (overlap != nullptr) {
            *overlap = raised;
        }
        return target;
    }

    /// The position after the newest logged write (0 if none): what waiting for all of them takes.
    [[nodiscard]] u64 NewestPosition() const {
        return newest_position;
    }

    void Reset() {
        *this = {};
    }

private:
    struct Entry {
        GPUVAddr begin;
        GPUVAddr end;
        u64 position;
    };
    std::array<Entry, Capacity> entries{};
    size_t count{};
    u64 evicted_position{};
    u64 newest_position{};
};

} // namespace VideoCommon
