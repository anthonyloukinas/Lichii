// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>

namespace Vulkan::DriverBuildReport {

enum class Kind { Graphics, Compute };

/// Records one vkCreate*Pipelines call. From the first build of a window, the first build 10 s or
/// more later logs how many there were and how many took over 10 ms: a driver that finds the
/// pipeline in its cache builds it in about 1 ms, one that compiles it takes tens of ms (NVIDIA:
/// 0.6-1.5 ms against ~48 ms), so tester logs suggest whether the driver's own cache works on
/// their system. Thread safe.
void Record(Kind kind, std::chrono::steady_clock::duration time);

/// Starts the summary over: the builds before a boot belong to the game that ran before it.
void StartSummary();

/// Logs the builds since the summary started (the pipelines built at boot) and starts over.
void LogSummary(const char* when);

} // namespace Vulkan::DriverBuildReport
