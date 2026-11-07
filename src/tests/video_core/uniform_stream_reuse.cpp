// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <utility>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "video_core/buffer_cache/uniform_stream_reuse.h"

namespace {
void Check(bool condition, const char* message) {
    INFO(message);
    REQUIRE(condition);
}

struct Memory {
    std::array<u8, 8192> bytes{};
    void ReadBlockUnsafe(u64 address, void* destination, u32 size) {
        Check(address + size <= bytes.size(), "read outside guest memory");
        std::memcpy(destination, bytes.data() + address, size);
    }
};

/// Guest memory with GetSpan, as DeviceMemoryManager has: one host run, or null for a range that
/// starts on an unmapped page or crosses a host discontinuity (both read with ReadBlockUnsafe).
struct SpanMemory {
    static constexpr u64 PageSize = 0x1000;
    std::array<u8, 8192> bytes{};
    bool first_page_unmapped{};
    bool discontiguous{}; ///< The two pages are not one host run
    int spans{};
    int fallbacks{};

    const u8* GetSpan(u64 address, u32 size) {
        Check(address + size <= bytes.size(), "span outside guest memory");
        const bool crosses = address < PageSize && address + size > PageSize;
        if ((first_page_unmapped && address < PageSize) || (discontiguous && crosses)) {
            return nullptr;
        }
        ++spans;
        return bytes.data() + address;
    }
    void ReadBlockUnsafe(u64 address, void* destination, u32 size) {
        Check(address + size <= bytes.size(), "read outside guest memory");
        ++fallbacks;
        u8* const out = static_cast<u8*>(destination);
        for (u32 i = 0; i < size; ++i) {
            out[i] = first_page_unmapped && address + i < PageSize ? 0 : bytes[address + i];
        }
    }
    /// What a read of the range gives (zeros where unmapped).
    std::vector<u8> Expected(u64 address, u32 size) {
        std::vector<u8> result(size);
        for (u32 i = 0; i < size; ++i) {
            result[i] = first_page_unmapped && address + i < PageSize ? 0 : bytes[address + i];
        }
        return result;
    }
};

struct Runtime {
    struct StreamUniformRef {
        size_t allocation;
    };
    std::vector<std::vector<u8>> allocations;
    StreamUniformRef last{}, bound{};
    u64 tick{1};
    u64 CurrentTick() const {
        return tick;
    }
    StreamUniformRef LastStreamUniformRef() const {
        return last;
    }
    bool advance_on_allocation{}; ///< The allocation submits: it lands in the next tick
    std::span<u8> BindMappedUniformBuffer(size_t, u32, u32 size) {
        if (std::exchange(advance_on_allocation, false)) {
            NextTick();
        }
        allocations.emplace_back(size);
        last = bound = StreamUniformRef{allocations.size() - 1};
        return allocations.back();
    }
    void RebindStreamUniform(StreamUniformRef ref, u32 size) {
        Check(ref.allocation < allocations.size(), "reused a recycled allocation");
        Check(allocations[ref.allocation].size() == size, "reused wrong allocation size");
        bound = ref;
    }
    void NextTick() {
        ++tick;
        allocations.clear();
    }
};

void CheckBound(const Runtime& runtime, const Memory& memory, u64 address, u32 size) {
    const auto& bytes = runtime.allocations.at(runtime.bound.allocation);
    Check(bytes.size() == size &&
              std::memcmp(bytes.data(), memory.bytes.data() + address, size) == 0,
          "bound uniform data differs from guest memory");
}
} // namespace

TEST_CASE("Uniform stream reuse preserves data through mutation and session resets",
          "[video_core]") {
    using VideoCommon::BindSmallUniformReusing;
    Memory memory;
    Runtime runtime;
    VideoCommon::ResetUniformStreamReuse<Runtime>();
    auto bind = [&](size_t stage, u32 slot, u64 address, u32 size) {
        Check(BindSmallUniformReusing(runtime, memory, stage, slot, slot, address, size),
              "valid small uniform rejected");
        CheckBound(runtime, memory, address, size);
    };
    bind(0, 0, 0, 64);
    bind(0, 0, 0, 64);
    Check(runtime.allocations.size() == 1, "identical bytes were copied twice");
    memory.bytes[7] = 23;
    bind(0, 0, 0, 64);
    Check(runtime.allocations.size() == 2, "changed bytes reused stale data");
    bind(0, 1, 0, 64);
    bind(1, 0, 0, 64);
    Check(runtime.allocations.size() == 4, "stage or slot shared the wrong allocation");
    runtime.NextTick();
    bind(0, 0, 0, 64);
    Check(runtime.allocations.size() == 1, "a new command buffer reused an old allocation");
    VideoCommon::ResetUniformStreamReuse<Runtime>();
    bind(0, 0, 0, 64);
    Check(runtime.allocations.size() == 2, "session reset retained an old allocation");
    bind(4, 17, 0, 4096);
    const auto before = runtime.allocations.size();
    for (auto [stage, slot, size] :
         {std::array<u32, 3>{5, 0, 64}, {0, 18, 64}, {0, 0, 0}, {0, 0, 4097}}) {
        Check(!BindSmallUniformReusing(runtime, memory, stage, slot, slot, 0, size),
              "invalid uniform range did not use the fallback");
    }
    Check(runtime.allocations.size() == before, "fallback allocated stream memory");

    u32 random = 17;
    for (u32 index = 0; index < 10000; ++index) {
        random = random * 1664525U + 1013904223U;
        if (index % 37 == 0)
            runtime.NextTick();
        if (index % 503 == 0)
            VideoCommon::ResetUniformStreamReuse<Runtime>();
        const u32 size = 1 + random % 4096;
        const u64 address = (random >> 12) % (memory.bytes.size() - 4096);
        if (index % 3 == 0)
            memory.bytes[address + random % size] ^= u8{0x5a};
        const size_t stage = (random >> 20) % 5;
        const u32 slot = (random >> 24) % 18;
        bind(stage, slot, address, size);
        bind(stage, slot, address, size);
    }
}

TEST_CASE("Uniform stream reuse compares in guest memory when it has one host run",
          "[video_core][uniform_stream_reuse]") {
    using VideoCommon::BindSmallUniformReusing;
    using Reuse = VideoCommon::UniformStreamReuse<Runtime::StreamUniformRef>;
    SpanMemory memory;
    Runtime runtime;
    VideoCommon::ResetUniformStreamReuse<Runtime>();
    Reuse& reuse = VideoCommon::UniformStreamReuseState<Runtime::StreamUniformRef>();
    for (size_t i = 0; i < memory.bytes.size(); ++i) {
        memory.bytes[i] = static_cast<u8>(i * 7 + 1);
    }
    // Binds, then checks the bound allocation and the slot's saved bytes against guest memory.
    const auto bind = [&](size_t stage, u32 slot, u64 address, u32 size) {
        Check(BindSmallUniformReusing(runtime, memory, stage, slot, slot, address, size),
              "valid small uniform rejected");
        const std::vector<u8> expected = memory.Expected(address, size);
        const auto& bound = runtime.allocations.at(runtime.bound.allocation);
        Check(bound.size() == size && std::memcmp(bound.data(), expected.data(), size) == 0,
              "bound uniform data differs from guest memory");
        const auto& saved = reuse.slots[stage][slot];
        Check(saved.size == size && std::memcmp(saved.bytes.data(), bound.data(), size) == 0,
              "slot bytes differ from the bound allocation");
    };
    const auto counts = [&](size_t allocations, int spans, int fallbacks) {
        Check(runtime.allocations.size() == allocations, "unexpected stream allocations");
        Check(memory.spans == spans, "unexpected in-place compares");
        Check(memory.fallbacks == fallbacks, "unexpected scratch reads");
    };

    // Same bytes, same tick: rebound without a copy.
    bind(0, 0, 0x1100, 256);
    counts(1, 1, 0);
    bind(0, 0, 0x1100, 256);
    counts(1, 2, 0);
    // Changed bytes.
    memory.bytes[0x1100 + 255] ^= 0xff;
    bind(0, 0, 0x1100, 256);
    counts(2, 3, 0);
    // Same bytes on a new tick: copied from the slot.
    runtime.NextTick();
    bind(0, 0, 0x1100, 256);
    counts(1, 4, 0);
    // Same bytes, but the allocation moves to the next tick: tagged with that one, so the next
    // bind in it reuses the allocation.
    runtime.NextTick();
    runtime.advance_on_allocation = true;
    bind(0, 0, 0x1100, 256);
    counts(1, 5, 0);
    bind(0, 0, 0x1100, 256);
    counts(1, 6, 0);
    // A shorter size, another stage and another slot each copy.
    bind(0, 0, 0x1100, 128);
    counts(2, 7, 0);
    bind(1, 0, 0x1100, 128);
    bind(0, 1, 0x1100, 128);
    counts(4, 9, 0);
    // Another address with the same bytes is reused (by content); with other bytes it copies.
    std::memcpy(memory.bytes.data() + 0x1800, memory.bytes.data() + 0x1100, 128);
    bind(0, 0, 0x1800, 128);
    counts(4, 10, 0);
    bind(0, 0, 0x1910, 128);
    counts(5, 11, 0);
    // A session reset forgets the slots.
    VideoCommon::ResetUniformStreamReuse<Runtime>();
    bind(0, 0, 0x1910, 128);
    counts(6, 12, 0);

    // No span (unmapped first page; two pages that are not one host run): the scratch path, which
    // shares the slots with the in-place compare.
    memory.discontiguous = true;
    bind(2, 3, 0xf80, 256);
    counts(7, 12, 1);
    bind(2, 3, 0xf80, 256);
    counts(7, 12, 2);
    memory.discontiguous = false;
    bind(2, 3, 0xf80, 256);
    counts(7, 13, 2);
    memory.bytes[0xf80] ^= 0xff;
    memory.discontiguous = true;
    bind(2, 3, 0xf80, 256);
    counts(8, 13, 3);
    memory.discontiguous = false;
    memory.first_page_unmapped = true;
    bind(2, 3, 0xf80, 256); // Reads zeros for the unmapped part: changed
    counts(9, 13, 4);
    bind(2, 3, 0xf80, 256);
    counts(9, 13, 5);
    memory.first_page_unmapped = false;
    bind(2, 3, 0xf80, 256);
    counts(10, 14, 5);
}
