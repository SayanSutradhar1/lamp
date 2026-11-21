
#pragma once

// ============================================================
// Lamp Framework
// Phase 5 - Native Asynchronous Worker Pool
//
// File: runtime/execution/native_worker_pool.hpp
//
// C++ Standard: C++20
//
// Responsibilities:
//   - Bounded native worker execution
//   - Priority-aware task scheduling
//   - Thread-safe submission
//   - Cooperative cancellation
//   - Request deadline enforcement
//   - Queue-wait timeout enforcement
//   - Exactly-once result publication
//   - Graceful shutdown
//
// Dependencies:
//   execution/async_execution_contract.hpp
//   bridge/execution_dispatcher.hpp
// ============================================================

#include "execution/async_execution_contract.hpp"
#include "bridge/execution_dispatcher.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace lamp::execution {

// ============================================================
// Configuration
// ============================================================

struct NativeWorkerPoolConfig final {
    // Zero selects hardware concurrency.
    std::size_t worker_count = 0;

    // Maximum waiting tasks, excluding running tasks.
    std::size_t max_pending_tasks = 4096;

    // Prevent sustained high-priority traffic from
    // indefinitely starving lower-priority tasks.
    std::size_t priority_burst_limit = 32;

    std::string thread_name_prefix = "lamp-native";
};

// ============================================================
// Native worker pool
// ============================================================

class NativeWorkerPool final
    : public IAsyncExecutionScheduler {
public:
    using Clock = std::chrono::steady_clock;
    using Completion = AsyncTaskCompletion;
    using CompletionPtr = std::shared_ptr<Completion>;

    explicit NativeWorkerPool(
        std::shared_ptr<bridge::ExecutionDispatcher>
            dispatcher,
        NativeWorkerPoolConfig config = {}
    )
        : dispatcher_(std::move(dispatcher)),
          config_(std::move(config)) {

        if (!dispatcher_) {
            throw std::invalid_argument(
                "Lamp native worker pool requires "
                "an execution dispatcher"
            );
        }

        if (config_.worker_count == 0) {
            config_.worker_count =
                std::thread::hardware_concurrency();

            if (config_.worker_count == 0) {
                config_.worker_count = 4;
            }
        }

        if (config_.max_pending_tasks == 0) {
            throw std::invalid_argument(
                "Lamp native worker pool requires "
                "a positive queue capacity"
            );
        }

        if (config_.priority_burst_limit == 0) {
            config_.priority_burst_limit = 1;
        }

        workers_.reserve(config_.worker_count);

        try {
            for (std::size_t i = 0;
                 i < config_.worker_count;
                 ++i) {

                workers_.emplace_back(
                    [this] {
                        WorkerLoop();
                    }
                );
            }
        } catch (...) {
            {
                std::lock_guard lock(mutex_);
                stopping_ = true;
            }

            queue_cv_.notify_all();

            for (auto& worker : workers_) {
                if (worker.joinable()) {
                    worker.join();
                }
            }

            throw;
        }
    }

    NativeWorkerPool(
        const NativeWorkerPool&
    ) = delete;

    NativeWorkerPool& operator=(
        const NativeWorkerPool&
    ) = delete;

    ~NativeWorkerPool() override {
        Shutdown();
    }

    // ========================================================
    // Task submission
    // ========================================================

    AsyncSubmitResult Submit(
        bridge::RequestEnvelope request,
        AsyncScheduleOptions options,
        AsyncTaskHandle& output
    ) override {

        output = AsyncTaskHandle{};

        // ----------------------------------------------------
        // Validate target
        // ----------------------------------------------------

        if (options.target ==
            TaskExecutionTarget::JavaScriptWorker) {

            return AsyncSubmitResult::Failure(
                AsyncSubmitStatus::BackendUnavailable,
                "Native worker pool cannot execute "
                "JavaScript-targeted tasks"
            );
        }

        // ----------------------------------------------------
        // Pre-submission cancellation
        // ----------------------------------------------------

        if (request.IsCancelled()) {
            return AsyncSubmitResult::Failure(
                AsyncSubmitStatus::Cancelled,
                "Request was cancelled before submission"
            );
        }

        // ----------------------------------------------------
        // Pre-submission deadline
        // ----------------------------------------------------

        if (options.reject_expired &&
            request.IsExpired()) {

            return AsyncSubmitResult::Failure(
                AsyncSubmitStatus::DeadlineExceeded,
                "Request deadline expired before submission"
            );
        }

        // ----------------------------------------------------
        // Validate request
        // ----------------------------------------------------

        if (auto error = request.Validate()) {
            return AsyncSubmitResult::Failure(
                AsyncSubmitStatus::InvalidRequest,
                error->message
            );
        }

        CompletionPtr completion;

        try {
            completion =
                std::make_shared<Completion>(
                    std::move(request)
                );
        } catch (const std::exception& error) {
            return AsyncSubmitResult::Failure(
                AsyncSubmitStatus::InternalError,
                error.what()
            );
        }

        Task task;
        task.completion = completion;
        task.options = options;
        task.submitted_at = Clock::now();

        {
            std::lock_guard lock(mutex_);

            if (stopping_) {
                return AsyncSubmitResult::Failure(
                    AsyncSubmitStatus::SchedulerStopped,
                    "Native worker pool is stopping"
                );
            }

            if (pending_tasks_ >=
                config_.max_pending_tasks) {

                return AsyncSubmitResult::Failure(
                    AsyncSubmitStatus::QueueFull,
                    "Native worker queue is full"
                );
            }

            if (!options.allow_queueing &&
                (pending_tasks_ != 0 ||
                 active_tasks_ >=
                     config_.worker_count)) {

                return AsyncSubmitResult::Failure(
                    AsyncSubmitStatus::QueueFull,
                    "Immediate worker execution unavailable"
                );
            }

            if (!completion->MarkQueued()) {
                return AsyncSubmitResult::Failure(
                    AsyncSubmitStatus::InternalError,
                    "Unable to queue asynchronous task"
                );
            }

            task.sequence = next_sequence_++;

            try {
                queues_[
                    PriorityIndex(options.priority)
                ].push_back(std::move(task));
            } catch (...) {
                return AsyncSubmitResult::Failure(
                    AsyncSubmitStatus::InternalError,
                    "Unable to enqueue native task"
                );
            }

            ++pending_tasks_;
        }

        output = AsyncTaskHandle(
            std::move(completion)
        );

        queue_cv_.notify_one();

        return AsyncSubmitResult::Success();
    }

    // ========================================================
    // Scheduler status
    // ========================================================

    [[nodiscard]]
    bool IsRunning() const noexcept override {
        std::lock_guard lock(mutex_);
        return !stopping_;
    }

    [[nodiscard]]
    std::size_t PendingTasks()
        const noexcept override {
        std::lock_guard lock(mutex_);
        return pending_tasks_;
    }

    [[nodiscard]]
    std::size_t ActiveTasks()
        const noexcept override {
        std::lock_guard lock(mutex_);
        return active_tasks_;
    }

    [[nodiscard]]
    std::size_t WorkerCount() const noexcept {
        return config_.worker_count;
    }

    [[nodiscard]]
    std::size_t QueueCapacity() const noexcept {
        return config_.max_pending_tasks;
    }

    // ========================================================
    // Graceful shutdown
    // ========================================================

    void Shutdown() override {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }

        queue_cv_.notify_all();

        // Shutdown must be called from a lifecycle thread,
        // not from one of this pool's worker threads.

        std::lock_guard shutdown_lock(
            shutdown_mutex_
        );

        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

private:
    // ========================================================
    // Task representation
    // ========================================================

    struct Task final {
        CompletionPtr completion;
        AsyncScheduleOptions options;

        Clock::time_point submitted_at{};
        std::uint64_t sequence = 0;
    };

    static constexpr std::size_t kPriorityCount = 4;

    [[nodiscard]]
    static constexpr std::size_t PriorityIndex(
        TaskPriority priority
    ) noexcept {
        return static_cast<std::size_t>(priority);
    }

    // ========================================================
    // Priority selection
    // ========================================================

    [[nodiscard]]
    std::optional<Task> PopNextTaskLocked() {
        if (pending_tasks_ == 0) {
            return std::nullopt;
        }

        std::size_t selected = kPriorityCount;

        if (high_priority_burst_ >=
            config_.priority_burst_limit) {

            std::uint64_t oldest =
                std::numeric_limits<
                    std::uint64_t
                >::max();

            for (std::size_t i = 0;
                 i < kPriorityCount - 1;
                 ++i) {

                if (!queues_[i].empty() &&
                    queues_[i].front().sequence <
                        oldest) {

                    oldest =
                        queues_[i].front().sequence;

                    selected = i;
                }
            }
        }

        if (selected == kPriorityCount) {
            for (std::size_t i = kPriorityCount;
                 i > 0;
                 --i) {

                const auto index = i - 1;

                if (!queues_[index].empty()) {
                    selected = index;
                    break;
                }
            }
        }

        if (selected == kPriorityCount) {
            return std::nullopt;
        }

        Task task =
            std::move(queues_[selected].front());

        queues_[selected].pop_front();

        --pending_tasks_;

        if (selected == kPriorityCount - 1) {
            ++high_priority_burst_;
        } else {
            high_priority_burst_ = 0;
        }

        return task;
    }

    // ========================================================
    // Worker loop
    // ========================================================

    void WorkerLoop() noexcept {
        while (true) {
            std::optional<Task> task;

            {
                std::unique_lock lock(mutex_);

                queue_cv_.wait(
                    lock,
                    [this] {
                        return stopping_ ||
                               pending_tasks_ > 0;
                    }
                );

                if (stopping_ &&
                    pending_tasks_ == 0) {
                    return;
                }

                task = PopNextTaskLocked();

                if (!task) {
                    continue;
                }

                ++active_tasks_;
            }

            ExecuteTask(*task);

            {
                std::lock_guard lock(mutex_);
                --active_tasks_;
            }

            queue_cv_.notify_all();
        }
    }

    // ========================================================
    // Execute task
    // ========================================================

    void ExecuteTask(Task& task) noexcept {
        const auto& completion = task.completion;

        if (!completion) {
            return;
        }

        // Synchronize external cancellation into the
        // task-owned cancellation source.
        completion->SynchronizeCancellation();

        const auto& request = completion->Request();

        // ----------------------------------------------------
        // Cancellation before execution
        // ----------------------------------------------------

        if (completion->CancellationRequested()) {
            CompleteSafely(
                completion,
                bridge::ResultEnvelope::Cancelled(
                    request
                )
            );

            return;
        }

        // ----------------------------------------------------
        // Deadline before execution
        // ----------------------------------------------------

        if (request.IsExpired()) {
            CompleteSafely(
                completion,
                bridge::ResultEnvelope::DeadlineExceeded(
                    request
                )
            );

            return;
        }

        // ----------------------------------------------------
        // Queue wait limit
        // ----------------------------------------------------

        if (task.options.max_queue_wait.count() > 0) {
            const auto waited =
                Clock::now() - task.submitted_at;

            if (waited >
                task.options.max_queue_wait) {

                CompleteSafely(
                    completion,
                    bridge::ResultEnvelope::Failure(
                        request,
                        {
                            bridge::OperationErrorCode::
                                ResourceExhausted,
                            "Task exceeded queue wait limit",
                            "QUEUE_WAIT_EXCEEDED",
                            true
                        }
                    )
                );

                return;
            }
        }

        // ----------------------------------------------------
        // Transition to running
        // ----------------------------------------------------

        if (!completion->MarkRunning()) {
            return;
        }

        // Close the race between queue checks and
        // entering the execution dispatcher.
        completion->SynchronizeCancellation();

        if (completion->CancellationRequested()) {
            CompleteSafely(
                completion,
                bridge::ResultEnvelope::Cancelled(
                    request
                )
            );

            return;
        }

        if (request.IsExpired()) {
            CompleteSafely(
                completion,
                bridge::ResultEnvelope::DeadlineExceeded(
                    request
                )
            );

            return;
        }

        // ----------------------------------------------------
        // Native execution
        // ----------------------------------------------------

        try {
            // The task-owned cancellation token is now
            // attached to request.cancellation.
            //
            // handle.Cancel() therefore becomes visible
            // to handlers that call request.IsCancelled().

            auto result =
                dispatcher_->Dispatch(request);

            CompleteSafely(
                completion,
                std::move(result)
            );

        } catch (const std::exception&) {
            CompleteSafely(
                completion,
                bridge::ResultEnvelope::Failure(
                    request,
                    {
                        bridge::OperationErrorCode::
                            ExecutionFailed,
                        "Native task execution failed",
                        "NATIVE_TASK_EXCEPTION",
                        false
                    },
                    bridge::ExecutionBackend::Native
                )
            );

        } catch (...) {
            CompleteSafely(
                completion,
                bridge::ResultEnvelope::Failure(
                    request,
                    {
                        bridge::OperationErrorCode::
                            ExecutionFailed,
                        "Unknown native task failure",
                        "NATIVE_TASK_UNKNOWN_EXCEPTION",
                        false
                    },
                    bridge::ExecutionBackend::Native
                )
            );
        }
    }

    // ========================================================
    // Exactly-once completion
    // ========================================================

    static void CompleteSafely(
        const CompletionPtr& completion,
        bridge::ResultEnvelope result
    ) noexcept {
        if (!completion) {
            return;
        }

        try {
            (void)completion->Complete(
                std::move(result)
            );
        } catch (...) {
            // A future telemetry integration should
            // record completion publication failures.
        }
    }

private:
    std::shared_ptr<bridge::ExecutionDispatcher>
        dispatcher_;

    NativeWorkerPoolConfig config_;

    mutable std::mutex mutex_;
    std::mutex shutdown_mutex_;

    std::condition_variable queue_cv_;

    std::deque<Task> queues_[kPriorityCount];

    std::vector<std::thread> workers_;

    std::size_t pending_tasks_ = 0;
    std::size_t active_tasks_ = 0;

    std::uint64_t next_sequence_ = 1;
    std::size_t high_priority_burst_ = 0;

    bool stopping_ = false;
};

} // namespace lamp::execution
