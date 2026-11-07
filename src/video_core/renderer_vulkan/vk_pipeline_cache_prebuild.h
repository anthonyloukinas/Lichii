// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>

#include "common/unique_function.h"
#include "video_core/renderer_vulkan/pipeline_build.h"

namespace Vulkan {

/// Cached pipelines the boot did not build, built on low-priority workers after it and handed to
/// the first draw that needs one (Take). A draw that comes first claims the key and builds it
/// itself, as without the prebuild.
template <typename Key, typename Pipeline>
class PipelinePrebuild {
    struct Entry {
        std::unique_ptr<EmissionBudget::Reservation> reservation;
        std::unique_ptr<Pipeline> pipeline;
        bool claimed{};
    };
    struct Task {
        Key key;
        size_t capture_bytes;
        Common::UniqueFunction<void> run;
    };

public:
    using Build = Common::UniqueFunction<std::unique_ptr<Pipeline>>;
    /// Asked before each build: false skips it (the live pipeline count reached its cap).
    using Gate = std::function<bool()>;

    /// Admits at most `capacity` pipelines and `capture_limit_` bytes of captures. Without
    /// `newest_first_`, the first records added are kept and built in that order; with it, the
    /// oldest queued records make room for newer ones over the capture limit, and the newest are
    /// built first.
    PipelinePrebuild(size_t capacity, size_t capture_limit_, bool broken_parallel_compilation,
                     bool newest_first_ = false)
        : budget{broken_parallel_compilation ? 0 : capacity}, capture_limit{capture_limit_},
          newest_first{newest_first_} {}

    ~PipelinePrebuild() {
        Stop();
    }

    bool Add(const Key& key, size_t capture_bytes, Build build) {
        if (started || stopped || entries.contains(key) || capture_bytes > capture_limit) {
            return false;
        }
        if (capture_bytes > capture_limit - captured_bytes) {
            if (!newest_first) {
                return false;
            }
            while (capture_bytes > capture_limit - captured_bytes) {
                DropOldest();
            }
        }
        auto reservation = budget.TryAcquire(true);
        if (!reservation) {
            return false;
        }
        auto entry = std::make_shared<Entry>();
        entry->reservation = std::move(reservation);
        entries.emplace(key, entry);
        captured_bytes += capture_bytes;
        tasks.push_back({key, capture_bytes, [this, key, entry, build = std::move(build)]() mutable {
            bool run;
            {
                std::scoped_lock lock{mutex};
                run = !stopped && !entry->claimed;
            }
            const bool gated = run && gate && !gate();
            std::unique_ptr<Pipeline> pipeline;
            if (run && !gated) {
                try {
                    pipeline = build();
                } catch (const std::exception& exception) {
                    LOG_ERROR(Render_Vulkan, "Cached pipeline prebuild failed: {}",
                              exception.what());
                } catch (...) {
                    LOG_ERROR(Render_Vulkan, "Cached pipeline prebuild failed");
                }
            }
            build = {};
            bool last;
            {
                std::scoped_lock lock{mutex};
                if (!stopped && !entry->claimed && pipeline) {
                    entry->pipeline = std::move(pipeline);
                    held.fetch_add(1, std::memory_order_relaxed);
                    ++stats.built;
                } else {
                    if (!entry->claimed) {
                        entries.erase(key);
                    }
                    if (entry->claimed) {
                        ++stats.taken;
                    } else if (gated) {
                        ++stats.skipped;
                    } else if (run && !stopped) {
                        ++stats.failed;
                    }
                }
                last = --remaining == 0;
            }
            if (last) {
                LogSummary("done");
            }
        }});
        return true;
    }

    /// Starts building on `threads` low-priority workers; `gate_`, when set, is asked before
    /// each build.
    void Start(size_t threads, Gate gate_ = {}) {
        if (started || stopped || tasks.empty() || threads == 0) {
            return;
        }
        started = true;
        gate = std::move(gate_);
        started_at = std::chrono::steady_clock::now();
        stats.queued = tasks.size();
        remaining = tasks.size();
        LOG_INFO(Render_Vulkan,
                 "Background pipeline build: {} cached pipelines queued ({} dropped over the "
                 "{} MiB capture limit), {} threads, {}",
                 stats.queued, stats.dropped, capture_limit >> 20, threads,
                 newest_first ? "newest first" : "in cache order");
        workers.reset(new Common::ThreadWorker(threads, "VkPipelinePrebuild", {},
                                               Common::ThreadPlacement::Background));
        if (newest_first) {
            for (auto it = tasks.rbegin(); it != tasks.rend(); ++it) {
                workers->QueueWork(std::move(it->run));
            }
        } else {
            for (auto& task : tasks) {
                workers->QueueWork(std::move(task.run));
            }
        }
        tasks.clear();
    }

    std::unique_ptr<Pipeline> Take(const Key& key) {
        std::shared_ptr<Entry> entry;
        {
            std::scoped_lock lock{mutex};
            const auto it = entries.find(key);
            if (it == entries.end()) {
                return nullptr;
            }
            entry = std::move(it->second);
            entries.erase(it);
            entry->claimed = true;
            if (entry->pipeline) {
                held.fetch_sub(1, std::memory_order_relaxed);
                ++stats.drawn;
            }
        }
        return std::move(entry->pipeline);
    }

    /// Built pipelines no draw has taken yet.
    [[nodiscard]] size_t Held() const noexcept {
        return held.load(std::memory_order_relaxed);
    }

    void Stop() {
        bool unfinished;
        {
            std::scoped_lock lock{mutex};
            stopped = true;
            unfinished = started && remaining != 0;
        }
        workers.reset();
        if (unfinished) {
            LogSummary("stopped");
        }
        tasks.clear();
        entries.clear();
        held.store(0, std::memory_order_relaxed);
    }

private:
    void DropOldest() {
        Task& oldest = tasks.front();
        entries.erase(oldest.key);
        captured_bytes -= oldest.capture_bytes;
        tasks.pop_front();
        ++stats.dropped;
    }

    void LogSummary(const char* how) {
        size_t queued, built, drawn, taken, skipped, failed, left;
        {
            std::scoped_lock lock{mutex};
            if (logged) {
                return;
            }
            logged = true;
            queued = stats.queued;
            built = stats.built;
            drawn = stats.drawn;
            taken = stats.taken;
            skipped = stats.skipped;
            failed = stats.failed;
            left = remaining;
        }
        const double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started_at).count();
        LOG_INFO(Render_Vulkan,
                 "Background pipeline build {} after {:.1f} s: {} queued, {} built ({} taken by a "
                 "draw, {} not drawn yet), {} claimed by a draw first, {} skipped at the live "
                 "pipeline cap, {} failed, {} not run",
                 how, seconds, queued, built, drawn, Held(), taken, skipped, failed, left);
    }

    EmissionBudget budget;
    const size_t capture_limit;
    const bool newest_first;
    size_t captured_bytes{};
    bool started{};
    bool stopped{};
    bool logged{};
    std::mutex mutex;
    std::unordered_map<Key, std::shared_ptr<Entry>> entries;
    std::deque<Task> tasks;
    std::unique_ptr<Common::ThreadWorker> workers;
    Gate gate;
    std::atomic<size_t> held{};
    size_t remaining{};
    std::chrono::steady_clock::time_point started_at{};
    struct {
        size_t queued{};
        size_t dropped{};
        size_t built{};
        size_t drawn{};
        size_t taken{};
        size_t skipped{};
        size_t failed{};
    } stats;
};

} // namespace Vulkan
