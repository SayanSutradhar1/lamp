#pragma once

#include "core/worker_pool.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lamp::core {

class TaskSchedulerError : public std::runtime_error {
public:
    explicit TaskSchedulerError(const char* message)
        : std::runtime_error(message) {}
};

// Thread-safe deadline scheduler. Callbacks execute on the supplied WorkerPool.
// The WorkerPool must outlive the scheduler and all submitted callbacks.
// Destruction and shutdown must be invoked outside scheduler callbacks.
// The WorkerPool must remain running until scheduler shutdown completes.
class TaskScheduler final {
public:
    using Clock = std::chrono::steady_clock;
    using Duration = Clock::duration;
    using Task = std::function<void()>;
    using TaskId = std::uint64_t;

    struct Handle {
        TaskId id = 0;
        [[nodiscard]] explicit operator bool() const noexcept { return id != 0; }
        friend bool operator==(const Handle&, const Handle&) = default;
    };

    explicit TaskScheduler(WorkerPool& workers)
        : workers_(workers), schedulerThread_([this] { run(); }) {}

    ~TaskScheduler() noexcept {
        try {
            shutdown(false);
        } catch (...) {
            std::terminate();
        }
    }

    TaskScheduler(const TaskScheduler&) = delete;
    TaskScheduler& operator=(const TaskScheduler&) = delete;
    TaskScheduler(TaskScheduler&&) = delete;
    TaskScheduler& operator=(TaskScheduler&&) = delete;

    // One-shot tasks become eligible after delay. A cancelled or discarded
    // task is never submitted to the worker pool, but already submitted tasks
    // cannot be interrupted.
    template <typename Rep, typename Period>
    [[nodiscard]] Handle scheduleAfter(
        std::chrono::duration<Rep, Period> delay, Task task) {
        if (delay < delay.zero()) {
            throw TaskSchedulerError("Task delay must not be negative");
        }
        return insert(Clock::now() + std::chrono::duration_cast<Duration>(delay),
                      Duration::zero(), std::move(task));
    }

    // Fixed-delay periodic execution: next deadline is scheduled only after
    // the previous callback finishes, so one periodic task never overlaps itself.
    template <typename Rep, typename Period>
    [[nodiscard]] Handle scheduleEvery(
        std::chrono::duration<Rep, Period> interval, Task task) {
        const auto period = std::chrono::duration_cast<Duration>(interval);
        if (interval <= interval.zero() || period <= Duration::zero()) {
            throw TaskSchedulerError("Task interval must be positive");
        }
        return insert(Clock::now() + period, period, std::move(task));
    }

    // Cancellation prevents future dispatch, not callbacks already running.
    [[nodiscard]] bool cancel(Handle handle) {
        std::lock_guard lock(mutex_);
        const auto it = tasks_.find(handle.id);
        if (it == tasks_.end()) return false;
        tasks_.erase(it);
        cv_.notify_all();
        return true;
    }

    [[nodiscard]] bool contains(Handle handle) const {
        std::lock_guard lock(mutex_);
        return tasks_.contains(handle.id);
    }

    [[nodiscard]] std::size_t pendingTasks() const {
        std::lock_guard lock(mutex_);
        return tasks_.size();
    }

    [[nodiscard]] bool isAccepting() const {
        std::lock_guard lock(mutex_);
        return accepting_;
    }

    // drain=true: submit currently scheduled one-shot tasks immediately,
    //             cancel periodic tasks; wait for dispatch thread to exit.
    // drain=false: discard all undispatched tasks.
    // Both modes wait for callbacks already submitted to the WorkerPool.
    // Neither mode shuts down the external WorkerPool.
    // Do not call shutdown() from a scheduled callback.
    void shutdown(bool drain = false) {
        if (std::this_thread::get_id() == schedulerThread_.get_id()) {
            throw TaskSchedulerError("Scheduler cannot join its own thread");
        }
        std::lock_guard shutdownLock(shutdownMutex_);
        {
            std::lock_guard lock(mutex_);
            if (!stopping_) {
                stopping_ = true;
                accepting_ = false;
                drain_ = drain;
            } else if (!drain) {
                drain_ = false;
            }
            cv_.notify_all();
        }
        if (schedulerThread_.joinable()) schedulerThread_.join();
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [this] { return inFlight_ == 0; });
    }

private:
    struct Entry {
        TaskId id;
        Clock::time_point due;
        Duration interval;
        Task callback;
        std::uint64_t generation = 0;
    };

    struct Deadline {
        Clock::time_point due;
        TaskId id;
        std::uint64_t generation;
        bool operator>(const Deadline& other) const noexcept {
            if (due != other.due) return due > other.due;
            return id > other.id;
        }
    };

    [[nodiscard]] Handle insert(Clock::time_point due, Duration interval, Task task) {
        if (!task) throw TaskSchedulerError("Cannot schedule an empty task");
        std::lock_guard lock(mutex_);
        if (!accepting_) throw TaskSchedulerError("Scheduler is not accepting tasks");
        if (nextId_ == 0) throw TaskSchedulerError("Scheduler task IDs exhausted");
        const TaskId id = nextId_++;
        auto entry = std::make_shared<Entry>(Entry{id, due, interval, std::move(task), 0});
        tasks_.emplace(id, entry);
        deadlines_.push(Deadline{due, id, 0});
        cv_.notify_all();
        return Handle{id};
    }

    void run() noexcept {
        std::unique_lock lock(mutex_);
        for (;;) {
            if (stopping_ && !drain_) {
                tasks_.clear();
                return;
            }
            if (stopping_ && drain_) {
                // Submit remaining one-shot tasks without waiting for deadlines.
                // Periodic entries are discarded to ensure finite shutdown.
                std::vector<std::shared_ptr<Entry>> ready;
                ready.reserve(tasks_.size());
                for (const auto& [id, entry] : tasks_) {
                    (void)id;
                    if (entry->interval == Duration::zero()) ready.push_back(entry);
                }
                tasks_.clear();
                lock.unlock();
                for (auto& entry : ready) dispatch(entry);
                return;
            }
            if (deadlines_.empty()) {
                cv_.wait(lock, [this] { return stopping_ || !deadlines_.empty(); });
                continue;
            }
            const Deadline top = deadlines_.top();
            const auto found = tasks_.find(top.id);
            if (found == tasks_.end() || found->second->generation != top.generation) {
                deadlines_.pop();
                continue;
            }
            if (Clock::now() < top.due) {
                cv_.wait_until(lock, top.due);
                continue;
            }
            deadlines_.pop();
            auto entry = found->second;
            if (entry->interval == Duration::zero()) tasks_.erase(found);
            lock.unlock();
            dispatch(entry);
            lock.lock();
        }
    }

    void dispatch(const std::shared_ptr<Entry>& entry) noexcept {
        {
            std::lock_guard lock(mutex_);
            ++inFlight_;
        }
        try {
            // A periodic callback captures its entry so it can rearm only
            // after completion. The scheduler must outlive worker callbacks.
            (void)workers_.submit([this, entry] {
                try { entry->callback(); } catch (...) { /* isolate callback failures */ }
                std::lock_guard lock(mutex_);
                if (entry->interval != Duration::zero() && !stopping_) {
                    const auto found = tasks_.find(entry->id);
                    if (found != tasks_.end()) {
                        try {
                            entry->due = Clock::now() + entry->interval;
                            ++entry->generation;
                            deadlines_.push(Deadline{entry->due, entry->id, entry->generation});
                        } catch (...) {
                            tasks_.erase(entry->id);
                        }
                    }
                }
                --inFlight_;
                cv_.notify_all();
            });
        } catch (...) {
            // If worker submission fails, remove the periodic registration.
            std::lock_guard lock(mutex_);
            tasks_.erase(entry->id);
            --inFlight_;
            cv_.notify_all();
        }
    }

    WorkerPool& workers_;
    mutable std::mutex mutex_;
    std::mutex shutdownMutex_;
    std::condition_variable cv_;
    std::unordered_map<TaskId, std::shared_ptr<Entry>> tasks_;
    std::priority_queue<Deadline, std::vector<Deadline>, std::greater<Deadline>> deadlines_;
    TaskId nextId_ = 1;
    std::size_t inFlight_ = 0;
    bool accepting_ = true;
    bool stopping_ = false;
    bool drain_ = false;
    std::thread schedulerThread_;
};

} // namespace lamp::core
