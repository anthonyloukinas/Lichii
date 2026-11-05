// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>

#include <QThread>
#include "common/common_types.h"
#include "common/logging.h"
#include "common/thread.h"

namespace Core {
class System;
} // namespace Core

namespace VideoCore {
enum class LoadCallbackStage;
} // namespace VideoCore

class EmuThread final : public QThread {
    Q_OBJECT

public:
    explicit EmuThread();
    ~EmuThread() override;

    /**
     * Start emulation (on new thread)
     * @warning Only call when not running!
     */
    void run() override;

    /**
     * Sets whether the emulation thread should run or not
     * @param should_run Boolean value, set the emulation thread to running if true
     */
    void SetRunning(bool should_run) {
        // TODO: Prevent other threads from modifying the state until we finish.
        {
            // Notify the running thread to change state.
            std::unique_lock run_lk{m_should_run_mutex};
            m_should_run = should_run;
            m_should_run_cv.notify_one();
        }

        // Wait until paused, if pausing.
        if (!should_run) {
            m_stopped.Wait();
        }
    }

    /**
     * Check if the emulation thread is running or not
     * @return True if the emulation thread is running, otherwise false
     */
    bool IsRunning() const {
        return m_should_run;
    }

    /**
     * Requests for the emulation thread to immediately stop running
     */
    void ForceStop() {
        LOG_WARNING(Frontend, "Force stopping EmuThread");
        m_stop_source.request_stop();
    }

    /**
     * Requests that the disk shader cache be reloaded for a different title.
     */
    void RequestDiskShaderCacheReload(u64 program_id) {
        std::unique_lock run_lk{m_should_run_mutex};
        m_pending_shader_cache_title = program_id;
        m_should_run_cv.notify_one();
    }

    /**
     * Precompile run: emit PrecompileFinished once the disk shader cache is loaded, so the
     * frontend can stop emulation right away. Call before start().
     */
    void SetPrecompileOnly(bool precompile_only) {
        m_precompile_only = precompile_only;
    }

    bool IsPrecompileOnly() const {
        return m_precompile_only;
    }

private:
    void EmulationPaused(std::unique_lock<std::mutex>& lk);
    void EmulationResumed(std::unique_lock<std::mutex>& lk);
    void ReloadDiskShaderCache(u64 program_id);

private:
    std::stop_source m_stop_source;
    std::mutex m_should_run_mutex;
    std::condition_variable_any m_should_run_cv;
    Common::Event m_stopped;
    bool m_should_run{true};
    std::optional<u64> m_pending_shader_cache_title;
    bool m_precompile_only{false};

signals:
    /**
     * Emitted when the CPU has halted execution
     *
     * @warning When connecting to this signal from other threads, make sure to specify either
     * Qt::QueuedConnection (invoke slot within the destination object's message thread) or even
     * Qt::BlockingQueuedConnection (additionally block source thread until slot returns)
     */
    void DebugModeEntered();

    /**
     * Emitted right before the CPU continues execution
     *
     * @warning When connecting to this signal from other threads, make sure to specify either
     * Qt::QueuedConnection (invoke slot within the destination object's message thread) or even
     * Qt::BlockingQueuedConnection (additionally block source thread until slot returns)
     */
    void DebugModeLeft();

    void LoadProgress(VideoCore::LoadCallbackStage stage, std::size_t value, std::size_t total);

    void ShaderCacheReloadStarted();
    void ShaderCacheReloadFinished();

    /// Precompile run: the disk shader cache finished loading; total is the pipelines built.
    void PrecompileFinished(std::size_t total);
};
