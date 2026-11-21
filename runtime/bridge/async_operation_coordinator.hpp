
#pragma once

// ============================================================
// Lamp Framework - Phase 6
// File: runtime/bridge/async_operation_coordinator.hpp
//
// Transport-independent asynchronous operation coordination.
//
// Responsibilities:
// - Submit operations to the Phase 5 scheduler
// - Bound outstanding operation count
// - Retain task handles until completion
// - Deliver completions on the polling thread
// - Cancel outstanding tasks during shutdown
//
// This component does NOT:
// - Perform HTTP parsing or socket I/O
// - Block waiting for operation results
// - Create worker threads
// - Expose an Express-style application interface
//
// The callback receives a completed AsyncTaskHandle.
// It may call handle.Get() to obtain the ResultEnvelope.
// ============================================================

#include "execution/async_execution_contract.hpp"

#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace lamp::bridge {

class AsyncOperationCoordinatorError final
    : public std::runtime_error {
public:
    explicit AsyncOperationCoordinatorError(
        const std::string& message
    )
        : std::runtime_error(message) {}
};

class AsyncOperationCoordinator final {
public:
    using Scheduler =
        execution::IAsyncExecutionScheduler;

    using TaskHandle =
        execution::AsyncTaskHandle;

    using ScheduleOptions =
        execution::AsyncScheduleOptions;

    using SubmitResult =
        execution::AsyncSubmitResult;

    using SubmitStatus =
        execution::AsyncSubmitStatus;

    using CompletionCallback =
        std::function<void(TaskHandle)>;

    explicit AsyncOperationCoordinator(
        std::shared_ptr<Scheduler> scheduler,
        std::size_t maxOutstanding = 4096
    )
        : scheduler_(std::move(scheduler)),
          maxOutstanding_(maxOutstanding) {
        if (!scheduler_) {
            throw AsyncOperationCoordinatorError(
                "Async operation coordinator requires "
                "a scheduler"
            );
        }

        if (maxOutstanding_ == 0) {
            throw AsyncOperationCoordinatorError(
                "Maximum outstanding operations "
                "must be positive"
            );
        }
    }

    AsyncOperationCoordinator(
        const AsyncOperationCoordinator&
    ) = delete;

    AsyncOperationCoordinator& operator=(
        const AsyncOperationCoordinator&
    ) = delete;

    ~AsyncOperationCoordinator() noexcept {
        Close();
    }

    // ========================================================
    // Optional Completion Wakeup
    //
    // Registers a lightweight callback invoked when any
    // accepted task completes (including if it completed
    // synchronously during Submit). Used to wake an event loop.
    // ========================================================

    void SetWakeupCallback(std::function<void()> wakeup) {
        std::lock_guard lock(mutex_);
        wakeupCallback_ = std::move(wakeup);
    }

    // ========================================================
    // Submit an operation
    // ========================================================

    [[nodiscard]]
    SubmitResult Submit(
        bridge::RequestEnvelope request,
        ScheduleOptions options,
        CompletionCallback callback
    ) {
        if (!callback) {
            return SubmitResult::Failure(
                SubmitStatus::InvalidRequest,
                "Completion callback is required"
            );
        }

        // Reserve capacity before invoking the scheduler.
        // The reservation prevents concurrent submissions
        // from exceeding maxOutstanding_.

        {
            std::lock_guard lock(mutex_);

            if (closed_) {
                return SubmitResult::Failure(
                    SubmitStatus::SchedulerStopped,
                    "Operation coordinator is closed"
                );
            }

            if (entries_.size() + reservations_
                >= maxOutstanding_) {
                return SubmitResult::Failure(
                    SubmitStatus::QueueFull,
                    "Outstanding operation limit reached"
                );
            }

            ++reservations_;
        }

        TaskHandle handle;
        SubmitResult result;

        try {
            result = scheduler_->Submit(
                std::move(request),
                options,
                handle
            );
        } catch (const std::exception& error) {
            result = SubmitResult::Failure(
                SubmitStatus::InternalError,
                error.what()
            );
        } catch (...) {
            result = SubmitResult::Failure(
                SubmitStatus::InternalError,
                "Scheduler submission failed"
            );
        }

        TaskHandle notifyHandle;
        std::function<void()> wakeup;

        {
            std::lock_guard lock(mutex_);

            --reservations_;

            if (!result.Accepted()) {
                return result;
            }

            if (!handle.Valid()) {
                return SubmitResult::Failure(
                    SubmitStatus::InternalError,
                    "Scheduler accepted without a task handle"
                );
            }

            if (closed_) {
                (void)handle.Cancel();

                return SubmitResult::Failure(
                    SubmitStatus::SchedulerStopped,
                    "Coordinator closed during submission"
                );
            }

            if (wakeupCallback_) {
                notifyHandle = handle;
                wakeup = wakeupCallback_;
            }

            try {
                entries_.push_back(
                    Entry{
                        std::move(handle),
                        std::move(callback)
                    }
                );
            } catch (...) {
                (void)handle.Cancel();

                return SubmitResult::Failure(
                    SubmitStatus::InternalError,
                    "Failed to retain operation completion"
                );
            }
        }

        if (notifyHandle.Valid() && wakeup) {
            notifyHandle.SetOnComplete(std::move(wakeup));
        }

        return result;
    }

    // ========================================================
    // Deliver completed operations
    // ========================================================
    //
    // Call from the transport owner thread.
    //
    // Callbacks are invoked without holding mutex_.
    // A callback may submit another operation.
    //
    // Callback exceptions are isolated so they cannot
    // prevent delivery of other completions.
    //
    // Returns the number of callbacks attempted.
    // ========================================================

    [[nodiscard]]
    std::size_t Poll() {
        std::vector<Entry> ready;

        {
            std::lock_guard lock(mutex_);

            if (closed_) {
                return 0;
            }

            auto iterator = entries_.begin();

            while (iterator != entries_.end()) {
                if (!iterator->handle.IsCompleted()) {
                    ++iterator;
                    continue;
                }

                ready.push_back(std::move(*iterator));

                iterator = entries_.erase(iterator);
            }
        }

        for (auto& entry : ready) {
            try {
                entry.callback(std::move(entry.handle));
            } catch (...) {
                // Completion callbacks are application/
                // transport boundaries. Their failures
                // must not escape the coordinator.
            }
        }

        return ready.size();
    }

    // ========================================================
    // Cancel outstanding work
    // ========================================================

    [[nodiscard]]
    bool CancelRequest(bridge::RequestId requestId) noexcept {
        std::lock_guard lock(mutex_);

        bool cancelled = false;

        for (auto& entry : entries_) {
            if (entry.handle.RequestId() == requestId) {
                if (entry.handle.Cancel()) {
                    cancelled = true;
                }
            }
        }

        return cancelled;
    }

    void CancelOutstanding() noexcept {
        std::lock_guard lock(mutex_);

        for (auto& entry : entries_) {
            (void)entry.handle.Cancel();
        }
    }

    // ========================================================
    // Close
    // ========================================================
    //
    // Closing cancels retained tasks and discards callbacks.
    //
    // It does not shut down the shared scheduler.
    // The scheduler lifecycle belongs to its owner.
    // ========================================================

    void Close() noexcept {
        std::lock_guard lock(mutex_);

        if (closed_) {
            return;
        }

        closed_ = true;

        for (auto& entry : entries_) {
            (void)entry.handle.Cancel();
        }

        entries_.clear();
    }

    // ========================================================
    // Inspection
    // ========================================================

    [[nodiscard]]
    std::size_t Outstanding() const noexcept {
        std::lock_guard lock(mutex_);

        return entries_.size() + reservations_;
    }

    [[nodiscard]]
    std::size_t Capacity() const noexcept {
        return maxOutstanding_;
    }

    [[nodiscard]]
    bool Closed() const noexcept {
        std::lock_guard lock(mutex_);

        return closed_;
    }

private:
    struct Entry final {
        TaskHandle handle;
        CompletionCallback callback;
    };

    std::shared_ptr<Scheduler> scheduler_;

    const std::size_t maxOutstanding_;

    mutable std::mutex mutex_;

    std::vector<Entry> entries_;

    std::size_t reservations_ = 0;

    bool closed_ = false;

    std::function<void()> wakeupCallback_;
};

} // namespace lamp::bridge
