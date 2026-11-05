// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/common_types.h"
#include "common/logging.h"

// EDEN_NX2_TARGET_TRACE=<target>[,<target>...]: every draw, clear and pass on one of these
// render targets (colour or depth) logs a line "NX2 target ..." with the frame, the draw's index
// in the frame, its shaders and counts, how its native indirect pointers resolved and whether it
// was recorded or dropped (and why). A target is a GPU address (hex, "0x8172fd0000") or a size
// ("4096x4096", any target of that size: addresses move between runs). The split's front logs
// the macro writes of the task/vertex count while one of them is bound ("front count"), with the
// pushbuffer word the macro's first parameter came from. Compute dispatches that are dropped are
// logged too. EDEN_NX2_TARGET_TRACE_MAX caps the lines (default 4000). Off: one bool test.
namespace VideoCommon::Nx2TargetTrace {

struct Selector {
    u64 address{};
    u32 width{};
    u32 height{};
};

inline std::vector<Selector> Parse(const char* value) {
    std::vector<Selector> selectors;
    if (value == nullptr) {
        return selectors;
    }
    std::string_view rest{value};
    while (!rest.empty()) {
        const size_t comma = rest.find(',');
        const std::string_view token = rest.substr(0, comma);
        rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
        if (token.empty()) {
            continue;
        }
        const std::string text{token};
        const bool hex_prefix = token.size() > 2 && token[0] == '0' && (token[1] | 0x20) == 'x';
        const size_t x = token.find_first_of("xX");
        Selector selector{};
        if (!hex_prefix && x != std::string_view::npos) {
            selector.width = static_cast<u32>(std::strtoul(text.c_str(), nullptr, 10));
            selector.height = static_cast<u32>(std::strtoul(text.c_str() + x + 1, nullptr, 10));
        } else {
            selector.address = std::strtoull(text.c_str(), nullptr, 16);
        }
        if (selector.address != 0 || (selector.width != 0 && selector.height != 0)) {
            selectors.push_back(selector);
        }
    }
    return selectors;
}

inline const std::vector<Selector> g_selectors = Parse(std::getenv("EDEN_NX2_TARGET_TRACE"));
inline const bool g_enabled = !g_selectors.empty();
inline const u32 g_max_lines = [] {
    const char* value = std::getenv("EDEN_NX2_TARGET_TRACE_MAX");
    return value != nullptr ? static_cast<u32>(std::strtoul(value, nullptr, 10)) : 4000U;
}();
inline std::atomic<u32> g_lines{};
/// Frames the back has finished (RasterizerVulkan::TickFrame); the front runs ahead of it.
inline std::atomic<u64> g_frame{};

[[nodiscard]] inline bool Matches(u64 address, u32 width, u32 height) {
    for (const Selector& selector : g_selectors) {
        if (selector.address != 0 ? selector.address == address
                                  : selector.width == width && selector.height == height) {
            return true;
        }
    }
    return false;
}

/// Whether another line may be logged; says so once when the cap is reached.
[[nodiscard]] inline bool TakeLine() {
    const u32 line = g_lines.fetch_add(1, std::memory_order_relaxed);
    if (line == g_max_lines) {
        LOG_INFO(HW_GPU, "NX2 target trace: {} lines, no more (EDEN_NX2_TARGET_TRACE_MAX)",
                 g_max_lines);
    }
    return line < g_max_lines;
}

struct Hit {
    bool depth;   ///< the depth target, else colour target `slot`
    u32 slot;
    u64 address;
    u32 width;
    u32 height;
    u32 format;
};

/// The first traced target among the bound ones; `words` are the 3D registers (Regs::reg_array or
/// the split front's copy).
template <typename Regs>
[[nodiscard]] std::optional<Hit> MatchTargets(const u32* words, bool depth = true,
                                              bool colour = true) {
    const auto address = [words](size_t index) {
        return (u64{words[index]} << 32) | words[index + 1];
    };
    if (depth && words[offsetof(Regs, zeta_enable) / 4] != 0) {
        const size_t zeta = offsetof(Regs, zeta) / 4;
        const size_t size = offsetof(Regs, zeta_size) / 4;
        const Hit hit{true, 0, address(zeta), words[size], words[size + 1], words[zeta + 2]};
        if (Matches(hit.address, hit.width, hit.height)) {
            return hit;
        }
    }
    if (colour) {
        const u32 control = words[offsetof(Regs, rt_control) / 4];
        const u32 count = control & 0xf;
        constexpr size_t Stride = sizeof(std::declval<Regs>().rt[0]) / 4;
        for (u32 i = 0; i < count && i < 8; ++i) {
            const u32 slot = (control >> (4 + 3 * i)) & 7;
            const size_t rt = offsetof(Regs, rt) / 4 + slot * Stride;
            const Hit hit{false, slot, address(rt), words[rt + 2], words[rt + 3], words[rt + 4]};
            if (hit.address != 0 && Matches(hit.address, hit.width, hit.height)) {
                return hit;
            }
        }
    }
    return std::nullopt;
}

} // namespace VideoCommon::Nx2TargetTrace
