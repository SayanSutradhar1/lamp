
#pragma once

#include <algorithm>
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
#include <unordered_set>
#include <utility>
#include <vector>

namespace lamp::core {

// ============================================================
// Worker Pool Errors
// ============================================================

class WorkerPoolError : public std::runtime_error {
public:
    explicit WorkerPoolError(const char* message)
        : std::runtime_error(message) {}
};

// ============================================================
// Worker Pool
//
// Thread-safe C++20 worker pool.
//
// Features:
// - Configurable worker count
// - FIFO task queue
// - Typed task submission using std::future
// - Exception propagation through futures
// - Graceful shutdown
// - Immediate shutdown with queued-task cancellation
// - Idempotent shutdown
// - Worker-thread detection
// - Constructor failure recovery
//
// Important:
// - Running tasks are never forcibly interrupted.
// - Immediate shutdown discards pending tasks.
// - Destroying a discarded packaged_task causes its
//   future to become ready with broken_promise.
// - Tasks must not synchronously wait for work that
//   requires the same fully occupied pool.
// ============================================================

class WorkerPool final {
public:
    using Task = std::function<void()>;

    // ========================================================
    // Construction
    // ========================================================

    explicit WorkerPool(
        std::size_t workerCount =
            std::thread::hardware_concurrency()
    ) {
        if (workerCount == 0) {
            workerCount = 1;
        }

        workers_.reserve(workerCount);

        try {
            for (std::size_t i = 0; i < workerCount; ++i) {
                workers_.emplace_back(
                    [this]() {
                        workerLoop();
                    }
                );
            }

            // NEW: Wait until all workers have registered.
            {
                std::unique_lock<std::mutex> lock(mutex_);

                startupCondition_.wait(
                    lock,
                    [this, workerCount]() {
                        return activeWorkerCount_ == workerCount;
                    }
                );
            }

        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                accepting_ = false;
                stopping_ = true;
                drainPending_ = false;
            }

            condition_.notify_all();

            for (auto& worker : workers_) {
                if (worker.joinable()) {
                    worker.join();
                }
            }

            throw;
        }
    }

    // ========================================================
    // Destruction
    // ========================================================

    ~WorkerPool() noexcept {
        // The owner must not destroy the pool from one
        // of its own worker threads.
        //
        // All externally submitted tasks must respect
        // the pool's lifetime.

        try {
            shutdown(true);
        } catch (...) {
            std::terminate();
        }
    }

    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    WorkerPool(WorkerPool&&) = delete;
    WorkerPool& operator=(WorkerPool&&) = delete;

    // ========================================================
    // Task Submission
    // ========================================================

    template <typename Function, typename... Args>
    [[nodiscard]]
    auto submit(Function&& function, Args&&... args)
        -> std::future<
            std::invoke_result_t<
                std::decay_t<Function>,
                std::decay_t<Args>...
            >
        > {

        using ReturnType = std::invoke_result_t<
            std::decay_t<Function>,
            std::decay_t<Args>...
        >;

        auto boundTask =
            [callable = std::forward<Function>(function),
             ...arguments = std::forward<Args>(args)]()
                mutable -> ReturnType {

                if constexpr (std::is_void_v<ReturnType>) {
                    std::invoke(
                        std::move(callable),
                        std::move(arguments)...
                    );
                } else {
                    return std::invoke(
                        std::move(callable),
                        std::move(arguments)...
                    );
                }
            };

        auto packagedTask =
            std::make_shared<
                std::packaged_task<ReturnType()>
            >(
                std::move(boundTask)
            );

        auto future = packagedTask->get_future();

        {
            std::lock_guard<std::mutex> lock(mutex_);

            if (!accepting_) {
                throw WorkerPoolError(
                    "WorkerPool is not accepting tasks"
                );
            }

            tasks_.emplace_back(
                [packagedTask]() {
                    (*packagedTask)();
                }
            );
        }

        condition_.notify_one();

        return future;
    }

    // ========================================================
    // Graceful / Immediate Shutdown
    // ========================================================

    // drain = true:
    //   Stop accepting tasks and finish queued tasks.
    //
    // drain = false:
    //   Stop accepting tasks and discard queued tasks.
    //
    // Running tasks always finish before shutdown returns.
    //
    // Shutdown must be called from outside pool workers.
    //
    // Concurrent shutdown calls are serialized.

    void shutdown(bool drain = true) {
        std::unique_lock<std::mutex> shutdownLock(
            shutdownMutex_
        );

        // Detect worker-initiated shutdown before changing
        // any state. Joining the current worker would
        // otherwise deadlock.
        if (isWorkerThread()) {
            throw WorkerPoolError(
                "WorkerPool shutdown cannot be called "
                "from a worker thread"
            );
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);

            if (!stopping_) {
                stopping_ = true;
                accepting_ = false;
                drainPending_ = drain;
            } else if (!drain) {
                // Escalate an earlier graceful shutdown
                // into immediate shutdown if requested.
                drainPending_ = false;
            }

            if (!drainPending_) {
                tasks_.clear();
            }
        }

        condition_.notify_all();

        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }

        workers_.clear();
    }

    // ========================================================
    // State Inspection
    // ========================================================

    [[nodiscard]]
    bool isAccepting() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return accepting_;
    }

    [[nodiscard]]
    bool isStopping() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return stopping_;
    }

    [[nodiscard]]
    std::size_t pendingTasks() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return tasks_.size();
    }

    [[nodiscard]]
    std::size_t workerCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return activeWorkerCount_;
    }

    [[nodiscard]]
    bool isWorkerThread() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return workerIds_.contains(
            std::this_thread::get_id()
        );
    }

private:
    // ========================================================
    // Worker Execution Loop
    // ========================================================

    void workerLoop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);

            workerIds_.insert(
                std::this_thread::get_id()
            );

            ++activeWorkerCount_;
        }

        // NEW: Notify constructor that a worker has registered.
        startupCondition_.notify_all();

        for (;;) {
            Task task;

            {
                std::unique_lock<std::mutex> lock(
                    mutex_
                );

                condition_.wait(
                    lock,
                    [this]() {
                        return stopping_ ||
                               !tasks_.empty();
                    }
                );

                if (stopping_) {
                    if (!drainPending_ || tasks_.empty()) {
                        break;
                    }
                }

                if (tasks_.empty()) {
                    continue;
                }

                task = std::move(tasks_.front());
                tasks_.pop_front();
            }

            // std::packaged_task captures task exceptions
            // and stores them in the corresponding future.
            //
            // The outer catch also protects the worker
            // against exceptions from other Task objects.
            try {
                task();
            } catch (...) {
                // Keep the worker alive.
            }
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);

            workerIds_.erase(
                std::this_thread::get_id()
            );

            --activeWorkerCount_;
        }
    }

    // ========================================================
    // Internal State
    // ========================================================

    mutable std::mutex mutex_;
    mutable std::mutex shutdownMutex_;

    std::condition_variable condition_;
    std::condition_variable startupCondition_; // NEW

    std::deque<Task> tasks_;
    std::vector<std::thread> workers_;

    std::unordered_set<std::thread::id> workerIds_;

    std::size_t activeWorkerCount_ = 0;

    bool accepting_ = true;
    bool stopping_ = false;
    bool drainPending_ = true;
};

} // namespace lamp::core
