
#pragma once

// ============================================================
// Lamp Framework
// Phase 5 - Asynchronous Execution Contract
//
// File: runtime/execution/async_execution_contract.hpp
//
// C++ Standard: C++20
//
// Responsibilities:
//   - Async task lifecycle
//   - Thread-safe state transitions
//   - Exactly-once result publication
//   - Continuous cooperative cancellation
//   - Future-based result retrieval
//   - Scheduler interface
//
// Cancellation:
//   Original request token
//            OR
//   Async task handle token
//            |
//            v
//   Combined execution token
//
// No polling or background synchronization required.
// ============================================================

#include "bridge/request_envelope.hpp"
#include "bridge/result_envelope.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace lamp::execution {

// ============================================================
// Task state
// ============================================================

enum class AsyncTaskState : std::uint8_t {
    Created = 0,
    Queued,
    Running,
    Succeeded,
    Failed,
    Cancelled,
    DeadlineExceeded
};

[[nodiscard]]
constexpr bool IsTerminal(
    AsyncTaskState state
) noexcept {
    switch (state) {
        case AsyncTaskState::Succeeded:
        case AsyncTaskState::Failed:
        case AsyncTaskState::Cancelled:
        case AsyncTaskState::DeadlineExceeded:
            return true;

        default:
            return false;
    }
}

// ============================================================
// Scheduling configuration
// ============================================================

enum class TaskPriority : std::uint8_t {
    Low = 0,
    Normal,
    High,
    Critical
};

enum class TaskExecutionTarget : std::uint8_t {
    Automatic = 0,
    NativeWorker,
    JavaScriptWorker
};

struct AsyncScheduleOptions final {
    TaskPriority priority = TaskPriority::Normal;

    TaskExecutionTarget target =
        TaskExecutionTarget::Automatic;

    std::chrono::milliseconds max_queue_wait{0};

    bool allow_queueing = true;
    bool cancel_while_queued = true;
    bool reject_expired = true;
};

// ============================================================
// Submission result
// ============================================================

enum class AsyncSubmitStatus : std::uint8_t {
    Accepted = 0,
    InvalidRequest,
    SchedulerStopped,
    QueueFull,
    DeadlineExceeded,
    Cancelled,
    BackendUnavailable,
    InternalError
};

struct AsyncSubmitResult final {
    AsyncSubmitStatus status =
        AsyncSubmitStatus::InternalError;

    std::string message;

    [[nodiscard]]
    bool Accepted() const noexcept {
        return status == AsyncSubmitStatus::Accepted;
    }

    [[nodiscard]]
    explicit operator bool() const noexcept {
        return Accepted();
    }

    [[nodiscard]]
    static AsyncSubmitResult Success() {
        return {
            AsyncSubmitStatus::Accepted,
            {}
        };
    }

    [[nodiscard]]
    static AsyncSubmitResult Failure(
        AsyncSubmitStatus status,
        std::string message = {}
    ) {
        return {
            status,
            std::move(message)
        };
    }
};

// ============================================================
// Async task completion
// ============================================================

class AsyncTaskCompletion final {
public:
    using Result = bridge::ResultEnvelope;
    using Clock = std::chrono::steady_clock;

    explicit AsyncTaskCompletion(
        bridge::RequestEnvelope request
    )
        : request_(std::move(request)),
          original_cancellation_(
              request_.cancellation
          ),
          cancellation_source_(),
          future_(
              promise_.get_future().share()
          ) {

        // Preserve the caller's cancellation token
        // while adding task-handle cancellation.
        //
        // Both sources remain observable throughout
        // execution, including after Dispatch() begins.

        request_.cancellation =
            bridge::RequestCancellationToken::Combine(
                original_cancellation_,
                cancellation_source_.Token()
            );
    }

    AsyncTaskCompletion(
        const AsyncTaskCompletion&
    ) = delete;

    AsyncTaskCompletion& operator=(
        const AsyncTaskCompletion&
    ) = delete;

    // ========================================================
    // Request identity
    // ========================================================

    [[nodiscard]]
    bridge::RequestId RequestId() const noexcept {
        return request_.request_id;
    }

    [[nodiscard]]
    const bridge::OperationId&
    OperationId() const noexcept {
        return request_.operation_id;
    }

    [[nodiscard]]
    const bridge::RequestEnvelope&
    Request() const noexcept {
        return request_;
    }

    // ========================================================
    // Continuous cancellation
    // ========================================================

    [[nodiscard]]
    bool CancellationRequested() const noexcept {
        return request_.IsCancelled();
    }

    [[nodiscard]]
    bridge::RequestCancellationToken
    CancellationToken() const noexcept {
        return request_.cancellation;
    }

    [[nodiscard]]
    bool RequestCancellation() noexcept {
        std::lock_guard lock(mutex_);

        if (IsTerminal(state_)) {
            return false;
        }

        cancellation_source_.Cancel();

        return true;
    }

    // Compatibility with existing NativeWorkerPool.
    //
    // Previously this copied external cancellation
    // into the task-owned source.
    //
    // With composite tokens, synchronization is
    // automatic. This method is intentionally a no-op
    // and can be removed when the worker pool is
    // updated to stop calling it.
    void SynchronizeCancellation() noexcept {}

    // ========================================================
    // State inspection
    // ========================================================

    [[nodiscard]]
    AsyncTaskState State() const noexcept {
        std::lock_guard lock(mutex_);
        return state_;
    }

    [[nodiscard]]
    bool IsCompleted() const noexcept {
        std::lock_guard lock(mutex_);
        return IsTerminal(state_);
    }

    [[nodiscard]]
    bool IsRunning() const noexcept {
        std::lock_guard lock(mutex_);
        return state_ == AsyncTaskState::Running;
    }

    [[nodiscard]]
    bool IsQueued() const noexcept {
        std::lock_guard lock(mutex_);
        return state_ == AsyncTaskState::Queued;
    }

    // ========================================================
    // Lifecycle transitions
    // ========================================================

    [[nodiscard]]
    bool MarkQueued() noexcept {
        std::lock_guard lock(mutex_);

        if (state_ != AsyncTaskState::Created) {
            return false;
        }

        state_ = AsyncTaskState::Queued;
        queued_at_ = Clock::now();

        return true;
    }

    [[nodiscard]]
    bool MarkRunning() noexcept {
        std::lock_guard lock(mutex_);

        if (state_ != AsyncTaskState::Created &&
            state_ != AsyncTaskState::Queued) {
            return false;
        }

        state_ = AsyncTaskState::Running;
        started_at_ = Clock::now();

        return true;
    }

    // ========================================================
    // Exactly-once completion
    // ========================================================

    [[nodiscard]]
    bool Complete(Result result) {
        std::function<void()> onComplete;

        {
            std::lock_guard lock(mutex_);

            if (IsTerminal(state_) ||
                state_ == AsyncTaskState::Created) {
                return false;
            }

            const auto terminal =
                TerminalStateFor(result);

            // Publish the result before marking the
            // task terminal.
            //
            // The mutex prevents competing publishers
            // from fulfilling the promise twice.

            promise_.set_value(std::move(result));

            state_ = terminal;
            completed_at_ = Clock::now();
            onComplete = std::move(on_complete_);

            completion_cv_.notify_all();
        }

        if (onComplete) {
            try {
                onComplete();
            } catch (...) {
                // Completion wakeup callbacks must never fail
                // task completion.
            }
        }

        return true;
    }

    void SetOnComplete(std::function<void()> callback) {
        bool alreadyCompleted = false;

        {
            std::lock_guard lock(mutex_);

            if (IsTerminal(state_)) {
                alreadyCompleted = true;
            } else {
                on_complete_ = std::move(callback);
            }
        }

        if (alreadyCompleted && callback) {
            try {
                callback();
            } catch (...) {
            }
        }
    }

    // ========================================================
    // Future access
    // ========================================================

    [[nodiscard]]
    std::shared_future<Result> Future() const {
        return future_;
    }

    [[nodiscard]]
    bool WaitFor(
        std::chrono::milliseconds timeout
    ) const {
        return future_.wait_for(timeout) ==
               std::future_status::ready;
    }

    void Wait() const {
        future_.wait();
    }

    [[nodiscard]]
    Result Get() const {
        return future_.get();
    }

    // ========================================================
    // Lifecycle timestamps
    // ========================================================

    [[nodiscard]]
    std::optional<Clock::time_point>
    QueuedAt() const noexcept {
        std::lock_guard lock(mutex_);
        return queued_at_;
    }

    [[nodiscard]]
    std::optional<Clock::time_point>
    StartedAt() const noexcept {
        std::lock_guard lock(mutex_);
        return started_at_;
    }

    [[nodiscard]]
    std::optional<Clock::time_point>
    CompletedAt() const noexcept {
        std::lock_guard lock(mutex_);
        return completed_at_;
    }

private:
    [[nodiscard]]
    static AsyncTaskState TerminalStateFor(
        const Result& result
    ) noexcept {
        switch (result.outcome) {
            case bridge::ResultOutcome::Success:
                return AsyncTaskState::Succeeded;

            case bridge::ResultOutcome::Failure:
                return AsyncTaskState::Failed;

            case bridge::ResultOutcome::Cancelled:
                return AsyncTaskState::Cancelled;

            case bridge::ResultOutcome::DeadlineExceeded:
                return AsyncTaskState::DeadlineExceeded;
        }

        return AsyncTaskState::Failed;
    }

private:
    // Declared before the cancellation sources so
    // the execution request remains available
    // throughout task completion.

    bridge::RequestEnvelope request_;

    // Retains the original caller's token.
    bridge::RequestCancellationToken
        original_cancellation_;

    // Controlled by AsyncTaskHandle::Cancel().
    bridge::RequestCancellationSource
        cancellation_source_;

    mutable std::mutex mutex_;
    mutable std::condition_variable completion_cv_;

    AsyncTaskState state_ =
        AsyncTaskState::Created;

    std::promise<Result> promise_;
    std::shared_future<Result> future_;

    std::optional<Clock::time_point> queued_at_;
    std::optional<Clock::time_point> started_at_;
    std::optional<Clock::time_point> completed_at_;
    std::function<void()> on_complete_;
};

// ============================================================
// Public asynchronous task handle
// ============================================================

class AsyncTaskHandle final {
public:
    using Result = bridge::ResultEnvelope;

    AsyncTaskHandle() = default;

    explicit AsyncTaskHandle(
        std::shared_ptr<AsyncTaskCompletion> completion
    )
        : completion_(std::move(completion)) {}

    [[nodiscard]]
    bool Valid() const noexcept {
        return static_cast<bool>(completion_);
    }

    [[nodiscard]]
    explicit operator bool() const noexcept {
        return Valid();
    }

    [[nodiscard]]
    bridge::RequestId RequestId() const noexcept {
        return completion_
            ? completion_->RequestId()
            : 0;
    }

    [[nodiscard]]
    AsyncTaskState State() const noexcept {
        return completion_
            ? completion_->State()
            : AsyncTaskState::Created;
    }

    [[nodiscard]]
    bool IsCompleted() const noexcept {
        return completion_ &&
               completion_->IsCompleted();
    }

    [[nodiscard]]
    bool Cancel() noexcept {
        return completion_ &&
               completion_->RequestCancellation();
    }

    [[nodiscard]]
    bool CancellationRequested() const noexcept {
        return completion_ &&
               completion_->CancellationRequested();
    }

    [[nodiscard]]
    std::shared_future<Result> Future() const {
        EnsureValid();
        return completion_->Future();
    }

    [[nodiscard]]
    bool WaitFor(
        std::chrono::milliseconds timeout
    ) const {
        EnsureValid();
        return completion_->WaitFor(timeout);
    }

    void Wait() const {
        EnsureValid();
        completion_->Wait();
    }

    [[nodiscard]]
    Result Get() const {
        EnsureValid();
        return completion_->Get();
    }

    void SetOnComplete(std::function<void()> callback) const {
        EnsureValid();
        completion_->SetOnComplete(std::move(callback));
    }

private:
    void EnsureValid() const {
        if (!completion_) {
            throw std::logic_error(
                "Lamp async task handle is invalid"
            );
        }
    }

    std::shared_ptr<AsyncTaskCompletion> completion_;
};

// ============================================================
// Async scheduler interface
// ============================================================

class IAsyncExecutionScheduler {
public:
    virtual ~IAsyncExecutionScheduler() = default;

    virtual AsyncSubmitResult Submit(
        bridge::RequestEnvelope request,
        AsyncScheduleOptions options,
        AsyncTaskHandle& output
    ) = 0;

    [[nodiscard]]
    virtual bool IsRunning() const noexcept = 0;

    [[nodiscard]]
    virtual std::size_t PendingTasks()
        const noexcept = 0;

    [[nodiscard]]
    virtual std::size_t ActiveTasks()
        const noexcept = 0;

    virtual void Shutdown() = 0;
};

} // namespace lamp::execution
