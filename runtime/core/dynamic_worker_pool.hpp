#pragma once

// ============================================================
// Lamp Framework
// Core Runtime Module - Dynamic Elastic Worker Pool & Parallel
// Subtask Runner
//
// File: runtime/core/dynamic_worker_pool.hpp
//
// Purpose:
//   Implements dynamic thread scaling (min_threads .. max_threads)
//   that automatically spawns extra worker threads when request or
//   subtask load spikes and scales idle threads back down cleanly
//   without detached threads or nested-parallelism deadlocks.
// ============================================================

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lamp::core {

struct DynamicWorkerPoolConfig final {
    std::size_t min_threads = 2;
    std::size_t max_threads = 0; // 0 => max(4, hardware_concurrency * 2)
    std::chrono::milliseconds idle_timeout{2000};
    std::size_t max_queued_tasks = 8192;
};

class DynamicWorkerPool final {
public:
    using Task = std::function<void()>;

    explicit DynamicWorkerPool(DynamicWorkerPoolConfig config = {})
        : config_(SanitizeConfig(config)) {
        std::lock_guard lock(mutex_);
        for (std::size_t i = 0; i < config_.min_threads; ++i) {
            SpawnWorkerLocked();
        }
    }

    ~DynamicWorkerPool() noexcept {
        try {
            Shutdown(true);
        } catch (...) {
        }
    }

    DynamicWorkerPool(const DynamicWorkerPool&) = delete;
    DynamicWorkerPool& operator=(const DynamicWorkerPool&) = delete;

    template <typename Fn>
    auto Submit(Fn&& fn) -> std::future<std::invoke_result_t<Fn>> {
        using ReturnType = std::invoke_result_t<Fn>;
        auto packaged = std::make_shared<std::packaged_task<ReturnType()>>(
            std::forward<Fn>(fn)
        );
        auto fut = packaged->get_future();

        Task wrapper = [packaged = std::move(packaged)]() {
            (*packaged)();
        };

        {
            std::lock_guard lock(mutex_);
            JoinRetiredLocked();

            if (!accepting_ || stopping_) {
                throw std::runtime_error("DynamicWorkerPool is stopped");
            }
            if (queue_.size() >= config_.max_queued_tasks) {
                throw std::runtime_error("DynamicWorkerPool queue limit exceeded");
            }

            queue_.push_back(std::move(wrapper));

            // Scale dynamically: if all current workers are busy (or queued work
            // exceeds idle workers) and capacity remains, spawn another worker.
            const std::size_t live = workers_.size();
            if (live < config_.max_threads &&
                queue_.size() > idle_workers_) {
                SpawnWorkerLocked();
            }
        }

        cv_.notify_one();
        return fut;
    }

    // Executes independent subtasks concurrently across dynamically scaled workers
    // and returns their results in submission order.
    template <typename T>
    [[nodiscard]]
    std::vector<T> RunParallel(std::vector<std::function<T()>> subtasks) {
        if (subtasks.empty()) {
            return {};
        }
        if (subtasks.size() == 1) {
            std::vector<T> single;
            single.reserve(1);
            single.push_back(subtasks[0]());
            return single;
        }

        // Submit N-1 subtasks to the dynamic pool and execute the last slice
        // inline so nested subtasks never deadlock even under heavy saturation.
        std::vector<std::future<T>> futures;
        futures.reserve(subtasks.size() - 1U);

        for (std::size_t i = 0; i + 1U < subtasks.size(); ++i) {
            futures.push_back(Submit(std::move(subtasks[i])));
        }

        std::exception_ptr first_error;
        std::vector<T> results;
        results.resize(subtasks.size());

        try {
            results.back() = subtasks.back()();
        } catch (...) {
            first_error = std::current_exception();
        }

        for (std::size_t i = 0; i < futures.size(); ++i) {
            try {
                results[i] = futures[i].get();
            } catch (...) {
                if (!first_error) {
                    first_error = std::current_exception();
                }
            }
        }

        if (first_error) {
            std::rethrow_exception(first_error);
        }
        return results;
    }

    // Overload for void subtasks.
    void RunParallelVoid(std::vector<std::function<void()>> subtasks) {
        if (subtasks.empty()) {
            return;
        }
        if (subtasks.size() == 1) {
            subtasks[0]();
            return;
        }

        std::vector<std::future<void>> futures;
        futures.reserve(subtasks.size() - 1U);

        for (std::size_t i = 0; i + 1U < subtasks.size(); ++i) {
            futures.push_back(Submit(std::move(subtasks[i])));
        }

        std::exception_ptr first_error;
        try {
            subtasks.back()();
        } catch (...) {
            first_error = std::current_exception();
        }

        for (auto& fut : futures) {
            try {
                fut.get();
            } catch (...) {
                if (!first_error) {
                    first_error = std::current_exception();
                }
            }
        }

        if (first_error) {
            std::rethrow_exception(first_error);
        }
    }

    [[nodiscard]]
    std::size_t CurrentThreadCount() const {
        std::lock_guard lock(mutex_);
        return workers_.size();
    }

    [[nodiscard]]
    std::size_t PeakThreadCount() const noexcept {
        return peak_threads_.load(std::memory_order_acquire);
    }

    [[nodiscard]]
    std::size_t ActiveTaskCount() const noexcept {
        return active_tasks_.load(std::memory_order_acquire);
    }

    [[nodiscard]]
    std::size_t PendingTaskCount() const {
        std::lock_guard lock(mutex_);
        return queue_.size();
    }

    [[nodiscard]]
    std::uint64_t TotalCompletedTasks() const noexcept {
        return completed_tasks_.load(std::memory_order_acquire);
    }

    void Shutdown(bool drain = true) {
        std::unordered_map<std::thread::id, std::thread> workers_to_join;
        {
            std::lock_guard lock(mutex_);
            if (stopping_ && workers_.empty() && retired_.empty()) {
                return;
            }
            accepting_ = false;
            drain_pending_ = drain;
            stopping_ = true;
            if (!drain) {
                queue_.clear();
            }
        }
        cv_.notify_all();

        // Drain workers outside lock after swapping maps progressively.
        while (true) {
            std::vector<std::thread> batch;
            {
                std::lock_guard lock(mutex_);
                for (auto& t : retired_) {
                    if (t.joinable()) {
                        batch.push_back(std::move(t));
                    }
                }
                retired_.clear();
                if (batch.empty() && workers_.empty()) {
                    break;
                }
                if (batch.empty()) {
                    // Extract remaining live worker threads once idle exit occurs.
                    for (auto& [id, t] : workers_) {
                        (void)id;
                        if (t.joinable()) {
                            batch.push_back(std::move(t));
                        }
                    }
                    workers_.clear();
                }
            }
            for (auto& t : batch) {
                if (t.joinable()) {
                    t.join();
                }
            }
        }
    }

private:
    [[nodiscard]]
    static DynamicWorkerPoolConfig SanitizeConfig(DynamicWorkerPoolConfig c) noexcept {
        if (c.min_threads == 0) {
            c.min_threads = 1;
        }
        if (c.max_threads == 0) {
            const std::size_t hw = static_cast<std::size_t>(std::thread::hardware_concurrency());
            c.max_threads = (std::max)(c.min_threads, (std::max)(std::size_t{4}, hw * 2U));
        }
        if (c.max_threads < c.min_threads) {
            c.max_threads = c.min_threads;
        }
        if (c.idle_timeout.count() <= 0) {
            c.idle_timeout = std::chrono::milliseconds{500};
        }
        if (c.max_queued_tasks == 0) {
            c.max_queued_tasks = 1;
        }
        return c;
    }

    void SpawnWorkerLocked() {
        std::thread worker([this]() { WorkerLoop(); });
        const auto id = worker.get_id();
        workers_.emplace(id, std::move(worker));

        const std::size_t now_live = workers_.size();
        std::size_t prev_peak = peak_threads_.load(std::memory_order_acquire);
        while (now_live > prev_peak &&
               !peak_threads_.compare_exchange_weak(
                   prev_peak,
                   now_live,
                   std::memory_order_release,
                   std::memory_order_acquire
               )) {
        }
    }

    void JoinRetiredLocked() {
        // Defer thread::join until lock can be safely released only if empty;
        // small swaps avoid blocking callers inside the mutex.
        if (retired_.empty()) {
            return;
        }
        std::vector<std::thread> tmp;
        tmp.swap(retired_);
        mutex_.unlock();
        for (auto& t : tmp) {
            if (t.joinable()) {
                t.join();
            }
        }
        mutex_.lock();
    }

    void WorkerLoop() {
        const auto self_id = std::this_thread::get_id();
        while (true) {
            Task task;
            {
                std::unique_lock lock(mutex_);
                ++idle_workers_;

                while (queue_.empty() && !stopping_) {
                    if (workers_.size() > config_.min_threads) {
                        const auto woke_up = cv_.wait_for(lock, config_.idle_timeout);
                        if (woke_up == std::cv_status::timeout &&
                            queue_.empty() &&
                            !stopping_ &&
                            workers_.size() > config_.min_threads) {
                            --idle_workers_;
                            const auto it = workers_.find(self_id);
                            if (it != workers_.end()) {
                                retired_.push_back(std::move(it->second));
                                workers_.erase(it);
                            }
                            return;
                        }
                    } else {
                        cv_.wait(lock);
                    }
                }

                --idle_workers_;

                if (stopping_ && (!drain_pending_ || queue_.empty())) {
                    const auto it = workers_.find(self_id);
                    if (it != workers_.end()) {
                        retired_.push_back(std::move(it->second));
                        workers_.erase(it);
                    }
                    return;
                }

                task = std::move(queue_.front());
                queue_.pop_front();
            }

            active_tasks_.fetch_add(1, std::memory_order_acq_rel);
            try {
                task();
            } catch (...) {
            }
            active_tasks_.fetch_sub(1, std::memory_order_acq_rel);
            completed_tasks_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    DynamicWorkerPoolConfig config_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Task> queue_;
    std::unordered_map<std::thread::id, std::thread> workers_;
    std::vector<std::thread> retired_;

    std::size_t idle_workers_ = 0;
    bool accepting_ = true;
    bool stopping_ = false;
    bool drain_pending_ = true;

    std::atomic<std::size_t> peak_threads_{0};
    std::atomic<std::size_t> active_tasks_{0};
    std::atomic<std::uint64_t> completed_tasks_{0};
};

} // namespace lamp::core
