// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <mutex>
#include <string_view>

#include <fmt/format.h>

#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/renderer_vulkan/vk_driver_build_report.h"

namespace Vulkan::DriverBuildReport {
namespace {

// Over this, the driver probably compiled the pipeline instead of taking it from its cache. It is
// a threshold on measured time: a build that waited for other work crosses it too.
constexpr std::chrono::milliseconds SlowThreshold{10};

struct Totals {
    u64 graphics{};
    u64 compute{};
    u64 slow{};
    std::chrono::steady_clock::duration total{};
    std::chrono::steady_clock::duration max{};

    void Add(Kind kind, std::chrono::steady_clock::duration time) {
        ++(kind == Kind::Graphics ? graphics : compute);
        slow += time > SlowThreshold ? 1 : 0;
        total += time;
        max = std::max(max, time);
    }

    void Log(std::string_view what) const {
        const auto ms = [](auto d) { return std::chrono::duration<double, std::milli>(d).count(); };
        const u64 count = graphics + compute;
        LOG_INFO(Render_Vulkan,
                 "Driver pipeline builds {}: {} graphics, {} compute; {} over {} ms (probably "
                 "compiled, not from the driver's cache); {:.1f} ms mean, {:.1f} ms max",
                 what, graphics, compute, slow, SlowThreshold.count(),
                 count != 0 ? ms(total) / static_cast<double>(count) : 0.0, ms(max));
    }
};

std::mutex mutex;
Totals window;
Totals summary;
std::chrono::steady_clock::time_point window_start{};

} // Anonymous namespace

void Record(Kind kind, std::chrono::steady_clock::duration time) {
    std::scoped_lock lock{mutex};
    const auto now = std::chrono::steady_clock::now();
    if (window.graphics + window.compute == 0) {
        // The window starts at its first build, not at the end of the one before.
        window_start = now;
    }
    window.Add(kind, time);
    summary.Add(kind, time);
    if (now - window_start >= std::chrono::seconds{10}) {
        window.Log(fmt::format("in the last {:.0f} s",
                               std::chrono::duration<double>(now - window_start).count()));
        window = {};
    }
}

void StartSummary() {
    std::scoped_lock lock{mutex};
    summary = {};
}

void LogSummary(const char* when) {
    std::scoped_lock lock{mutex};
    summary.Log(when);
    summary = {};
}

} // namespace Vulkan::DriverBuildReport
