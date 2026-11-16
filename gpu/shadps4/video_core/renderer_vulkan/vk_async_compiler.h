// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace Vulkan {

// gow3: bounded compiler queue. Stop drains accepted work before Vulkan resources are released.
class AsyncCompiler {
public:
    explicit AsyncCompiler(unsigned count, size_t capacity = 64,
                           std::function<void(unsigned)> initialize = {}) : capacity{capacity} {
        for (unsigned i = 0; i < count; ++i) {
            workers.emplace_back([this, i, initialize] {
                worker_index = i;
                if (initialize) {
                    initialize(i);
                }
                for (;;) {
                    std::function<void()> task;
                    {
                        std::unique_lock lock{mutex};
                        cv.wait(lock, [&] { return stopping || !tasks.empty() || !urgent.empty(); });
                        if (tasks.empty() && urgent.empty()) {
                            return;
                        }
                        auto& queue = urgent.empty() ? tasks : urgent;
                        auto next = std::move(queue.front());
                        queue.pop_front();
                        queue_wait_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - next.enqueued).count();
                        task = std::move(next.function);
                    }
                    task();
                }
            });
        }
    }
    ~AsyncCompiler() { Stop(); }

    bool Submit(std::function<void()> task, bool priority = false) {
        {
            std::scoped_lock lock{mutex};
            if (stopping || tasks.size() + urgent.size() >= capacity) {
                return false;
            }
            (priority ? urgent : tasks).push_back({std::move(task), std::chrono::steady_clock::now()});
        }
        cv.notify_one();
        return true;
    }
    bool HasCapacity() {
        std::scoped_lock lock{mutex};
        return !stopping && tasks.size() + urgent.size() < capacity;
    }
    static unsigned WorkerIndex() { return worker_index; }
    size_t Depth() {
        std::scoped_lock lock{mutex};
        return tasks.size() + urgent.size();
    }
    uint64_t QueueWaitNs() { return queue_wait_ns.exchange(0); }
    void Stop() {
        {
            std::scoped_lock lock{mutex};
            stopping = true;
        }
        cv.notify_all();
        for (auto& worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

private:
    size_t capacity;
    std::mutex mutex;
    std::condition_variable cv;
    struct Task {
        std::function<void()> function;
        std::chrono::steady_clock::time_point enqueued;
    };
    std::deque<Task> tasks, urgent;
    static inline thread_local unsigned worker_index = 0;
    std::atomic<uint64_t> queue_wait_ns{};
    bool stopping = false;
    std::vector<std::thread> workers;
};

/// gow3: compile jobs the waiting thread ran itself instead of waiting in the queue
/// (AsyncJob::Wait); reported with the "Shaders: ... draws skipped" line.
inline std::atomic<uint64_t> g_gow3_jobs_run_by_waiter{0};

// gow3: one job per cache entry; failures are terminal until the next session.
template <typename T>
class AsyncJob {
public:
    template <typename F>
    bool Start(AsyncCompiler& compiler, F&& function, bool priority = false) {
        if (started) {
            return false;
        }
        auto state = std::make_shared<State>(std::forward<F>(function));
        auto completion = state->task.get_future();
        if (!compiler.Submit([state] { state->Run(); }, priority)) {
            return false;
        }
        future = std::move(completion);
        this->state = std::move(state);
        started = true;
        return true;
    }
    bool Pending() const { return future.valid(); }
    bool Failed() const { return failed; }
    const std::string& Error() const { return error; }
    /// Waits for the job. One still queued runs on the waiting thread at once: a draw waiting
    /// for its pipeline no longer waits behind the whole queue (a new area queued ~300 compiles
    /// and a live draw waited 1.2 s, 452 ms of it in the queue). The pipeline caches the jobs
    /// use are internally synchronized. GOW3_WAIT_RUNS_JOB=0 waits as before.
    void Wait() const {
        if (!future.valid()) {
            return;
        }
        static const bool run_here = [] {
            const char* env = std::getenv("GOW3_WAIT_RUNS_JOB");
            return !env || env[0] != '0';
        }();
        if (run_here && state && state->Run()) {
            g_gow3_jobs_run_by_waiter.fetch_add(1, std::memory_order_relaxed);
        }
        future.wait();
    }
    std::optional<T> Poll() {
        if (!future.valid() || future.wait_for(std::chrono::seconds{0}) != std::future_status::ready) {
            return std::nullopt;
        }
        try {
            return future.get();
        } catch (const std::exception& exception) {
            error = exception.what();
        } catch (...) {
            error = "Unknown compiler error";
        }
        failed = true;
        return std::nullopt;
    }

private:
    struct State {
        template <typename F>
        explicit State(F&& function) : task(std::forward<F>(function)) {}
        /// Runs the task unless another thread claimed it; true when this call ran it.
        bool Run() {
            if (claimed.exchange(true, std::memory_order_acq_rel)) {
                return false;
            }
            task();
            return true;
        }
        std::packaged_task<T()> task;
        std::atomic<bool> claimed{false};
    };
    std::shared_ptr<State> state;
    std::future<T> future;
    bool started = false;
    bool failed = false;
    std::string error;
};

// gow3: IR retains its pools until emission finishes, then returns them for another translation.
template <typename T>
class CompilerPool {
public:
    explicit CompilerPool(size_t capacity) : capacity{capacity} {}
    std::pair<std::unique_ptr<T>, bool> Acquire() {
        std::scoped_lock lock{mutex};
        if (available.empty()) {
            return {std::make_unique<T>(), false};
        }
        auto pool = std::move(available.back());
        available.pop_back();
        return {std::move(pool), true};
    }
    void Recycle(std::unique_ptr<T> pool) noexcept {
        try {
            pool->ReleaseContents();
            std::scoped_lock lock{mutex};
            if (available.size() < capacity) {
                available.push_back(std::move(pool));
            }
        } catch (...) {
            // Reuse is optional; discard the pool if resetting or retaining it fails.
        }
    }
    size_t Retained() {
        std::scoped_lock lock{mutex};
        return available.size();
    }
private:
    size_t capacity;
    std::mutex mutex;
    std::vector<std::unique_ptr<T>> available;
};

} // namespace Vulkan
