#pragma once

// Lamp Framework - Phase 5: asynchronous execution dispatcher (C++20)
// File: runtime/execution/async_execution_dispatcher.hpp
//
// Routes requests to native or JavaScript execution schedulers. This class
// never executes user code or accesses V8 handles. Schedulers must outlive
// in-flight tasks; shared ownership keeps them alive through dispatch.

#include "execution/async_execution_contract.hpp"

#include <cstddef>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace lamp::execution {

struct AsyncExecutionDispatcherConfig final {
    // Automatic requests prefer native execution by default. This selects
    // a backend; it does not infer operation capabilities from the request.
    TaskExecutionTarget automatic_target = TaskExecutionTarget::NativeWorker;

    // Retry the other scheduler ONLY when the preferred backend explicitly
    // reports BackendUnavailable. Never retry after QueueFull, cancellation,
    // deadline expiry, or InternalError: a retry could change semantics.
    bool fallback_on_backend_unavailable = false;

    // If true, dispatcher Shutdown() also shuts down its schedulers.
    // Disable when schedulers are shared with other dispatchers.
    bool shutdown_backends = true;
};

class AsyncExecutionDispatcher final : public IAsyncExecutionScheduler {
public:
    using SchedulerPtr = std::shared_ptr<IAsyncExecutionScheduler>;

    explicit AsyncExecutionDispatcher(
        SchedulerPtr native_scheduler,
        SchedulerPtr javascript_scheduler,
        AsyncExecutionDispatcherConfig config = {})
        : native_(std::move(native_scheduler)),
          javascript_(std::move(javascript_scheduler)),
          config_(config) {
        if (!native_ && !javascript_) {
            throw std::invalid_argument(
                "Async dispatcher requires at least one scheduler");
        }
        if (config_.automatic_target == TaskExecutionTarget::Automatic) {
            throw std::invalid_argument(
                "Automatic dispatch preference must specify a backend");
        }
        if (native_ && javascript_ && native_ == javascript_) {
            throw std::invalid_argument(
                "Native and JavaScript schedulers must be distinct");
        }
    }

    AsyncExecutionDispatcher(const AsyncExecutionDispatcher&) = delete;
    AsyncExecutionDispatcher& operator=(const AsyncExecutionDispatcher&) = delete;

    ~AsyncExecutionDispatcher() override {
        Shutdown();
    }

    AsyncSubmitResult Submit(
        bridge::RequestEnvelope request,
        AsyncScheduleOptions options,
        AsyncTaskHandle& output) override {
        output = AsyncTaskHandle{};

        // Lock serializes submission with shutdown. In particular, Shutdown
        // cannot start destroying backend state between route selection and
        // the backend's Submit(). The backend must not call back into this
        // dispatcher synchronously from Submit().
        std::lock_guard lock(mutex_);
        if (stopping_) {
            return AsyncSubmitResult::Failure(
                AsyncSubmitStatus::SchedulerStopped,
                "Async execution dispatcher is stopped");
        }

        const bool automatic =
            options.target == TaskExecutionTarget::Automatic;
        const auto preferred = automatic
            ? config_.automatic_target : options.target;
        const SchedulerPtr& primary = Select(preferred);

        if (!primary) {
            return AsyncSubmitResult::Failure(
                AsyncSubmitStatus::BackendUnavailable,
                "Requested execution backend is not configured");
        }

        // Copy before moving: fallback must preserve the original request.
        // RequestEnvelope is copyable in the Phase 5 execution contract.
        if (automatic && config_.fallback_on_backend_unavailable) {
            auto backup_request = request;
            auto primary_options = options;
            primary_options.target = preferred;
            auto result = primary->Submit(
                std::move(request), primary_options, output);
            if (result.status != AsyncSubmitStatus::BackendUnavailable) {
                return result;
            }
            // Do not return a handle from a failed attempt.
            output = AsyncTaskHandle{};
            const auto alternative = Other(preferred);
            const SchedulerPtr& secondary = Select(alternative);
            if (!secondary) {
                return result;
            }
            auto secondary_options = options;
            secondary_options.target = alternative;
            return secondary->Submit(
                std::move(backup_request), secondary_options, output);
        }

        options.target = preferred;
        return primary->Submit(std::move(request), options, output);
    }

    [[nodiscard]] bool IsRunning() const noexcept override {
        std::lock_guard lock(mutex_);
        return !stopping_ &&
            ((native_ && native_->IsRunning()) ||
             (javascript_ && javascript_->IsRunning()));
    }

    [[nodiscard]] std::size_t PendingTasks() const noexcept override {
        std::lock_guard lock(mutex_);
        return (native_ ? native_->PendingTasks() : 0) +
               (javascript_ ? javascript_->PendingTasks() : 0);
    }

    [[nodiscard]] std::size_t ActiveTasks() const noexcept override {
        std::lock_guard lock(mutex_);
        return (native_ ? native_->ActiveTasks() : 0) +
               (javascript_ ? javascript_->ActiveTasks() : 0);
    }

    void Shutdown() override {
        // A separate mutex serializes concurrent Shutdown() calls. Do not
        // hold mutex_ while joining backend workers; their completion paths
        // may need to call this dispatcher's status methods.
        std::lock_guard shutdown_lock(shutdown_mutex_);
        SchedulerPtr native;
        SchedulerPtr javascript;
        {
            std::lock_guard lock(mutex_);
            if (stopping_) {
                return;
            }
            stopping_ = true;
            if (config_.shutdown_backends) {
                native = native_;
                javascript = javascript_;
            }
        }
        if (native) {
            native->Shutdown();
        }
        if (javascript) {
            javascript->Shutdown();
        }
    }

private:
    [[nodiscard]] const SchedulerPtr& Select(
        TaskExecutionTarget target) const noexcept {
        if (target == TaskExecutionTarget::NativeWorker) {
            return native_;
        }
        if (target == TaskExecutionTarget::JavaScriptWorker) {
            return javascript_;
        }
        return empty_;
    }

    [[nodiscard]] static TaskExecutionTarget Other(
        TaskExecutionTarget target) noexcept {
        return target == TaskExecutionTarget::NativeWorker
            ? TaskExecutionTarget::JavaScriptWorker
            : TaskExecutionTarget::NativeWorker;
    }

    SchedulerPtr native_;
    SchedulerPtr javascript_;
    AsyncExecutionDispatcherConfig config_;
    SchedulerPtr empty_;
    mutable std::mutex mutex_;
    std::mutex shutdown_mutex_;
    bool stopping_ = false;
};

} // namespace lamp::execution
