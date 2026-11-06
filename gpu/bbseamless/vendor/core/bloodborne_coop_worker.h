// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <string_view>
#include <thread>
#include <utility>

#include "common/types.h"

namespace Core::Bloodborne {

// Read-only co-op work off the game's thread. The game thread copies what a part needs (it alone
// may touch game memory safely in step) and posts a job; the job scans and decides on this worker,
// and what must touch the game comes back through OnGameThread, which the game thread runs at the
// start of its next tick. A game thread never waits: a full queue refuses the job, and the part
// tries again on its next round.
class CoopWorker {
public:
    using Job = std::function<void()>;
    static constexpr std::size_t MaxWaitingJobs = 8;

    // on_start runs on the worker before its first job; on_idle at least every idle_period.
    explicit CoopWorker(Job on_start = {}, Job on_idle = {},
                        std::chrono::milliseconds idle_period = std::chrono::seconds{2})
        : on_start{std::move(on_start)}, on_idle{std::move(on_idle)}, idle_period{idle_period} {}

    ~CoopWorker() {
        {
            std::scoped_lock lock{mutex};
            stopping = true;
        }
        wake.notify_all();
        if (thread.joinable()) {
            thread.join();
        }
    }

    // Game thread: false when the queue is full (the job did not run and will not).
    bool Post(Job job) {
        {
            std::scoped_lock lock{mutex};
            if (jobs.size() >= MaxWaitingJobs) {
                return false;
            }
            jobs.push_back(std::move(job));
            if (!thread.joinable()) {
                thread = std::thread{[this] { Run(); }};
            }
        }
        wake.notify_one();
        return true;
    }

    // Worker: a step for the game thread's next tick.
    void OnGameThread(Job step) {
        std::scoped_lock lock{mutex};
        steps.push_back(std::move(step));
    }

    // Game thread, at the start of its tick: the steps that came, in their order.
    std::size_t ApplyPending() {
        std::deque<Job> taken;
        {
            std::scoped_lock lock{mutex};
            taken.swap(steps);
        }
        for (Job& step : taken) {
            step();
        }
        return taken.size();
    }

private:
    void Run() {
        if (on_start) {
            on_start();
        }
        auto idle_at = std::chrono::steady_clock::now() + idle_period;
        std::unique_lock lock{mutex};
        while (true) {
            wake.wait_until(lock, idle_at, [this] { return stopping || !jobs.empty(); });
            if (stopping) {
                return;
            }
            if (std::chrono::steady_clock::now() >= idle_at) {
                idle_at = std::chrono::steady_clock::now() + idle_period;
                if (on_idle) {
                    lock.unlock();
                    on_idle();
                    lock.lock();
                }
            }
            if (jobs.empty()) {
                continue;
            }
            Job job = std::move(jobs.front());
            jobs.pop_front();
            lock.unlock();
            job();
            job = nullptr;
            lock.lock();
        }
    }

    Job on_start;
    Job on_idle;
    std::chrono::milliseconds idle_period;
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Job> jobs;
    std::deque<Job> steps;
    std::thread thread;
    bool stopping = false;
};

// Which parts run on the worker. On by default; BB_COOP_OFFMAIN=0 starts with all off, and lines
// "<part>=0" or "<part>=1" in user/log/coop-offmain.txt change one while the game runs ("all" is
// every part), so a part under suspicion can be tried both ways in one session.
enum class OffMainPart : std::size_t { Flags, WorldFile, Count };

class OffMainSwitches {
public:
    explicit OffMainSwitches(bool all_on = true) {
        for (auto& part : on) {
            part.store(all_on, std::memory_order_relaxed);
        }
    }

    bool On(OffMainPart part) const {
        return on[static_cast<std::size_t>(part)].load(std::memory_order_relaxed);
    }

    void Set(OffMainPart part, bool value) {
        on[static_cast<std::size_t>(part)].store(value, std::memory_order_relaxed);
    }

    void Apply(std::string_view text) {
        while (!text.empty()) {
            const auto end = text.find('\n');
            std::string_view line = text.substr(0, end);
            text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
            if (!line.empty() && line.back() == '\r') {
                line.remove_suffix(1);
            }
            if (line.size() < 3 || line[line.size() - 2] != '=') {
                continue;
            }
            const char value = line.back();
            if (value != '0' && value != '1') {
                continue;
            }
            const std::string_view name = line.substr(0, line.size() - 2);
            for (std::size_t part = 0; part < on.size(); ++part) {
                if (name == "all" || name == Names[part]) {
                    on[part].store(value == '1', std::memory_order_relaxed);
                }
            }
        }
    }

private:
    static constexpr std::array<std::string_view, static_cast<std::size_t>(OffMainPart::Count)>
        Names{"flags", "worldfile"};
    std::array<std::atomic<bool>, static_cast<std::size_t>(OffMainPart::Count)> on{};
};

} // namespace Core::Bloodborne
