
#pragma once

#include "core/event_dispatcher.hpp"
#include "core/task_scheduler.hpp"
#include "core/worker_pool.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace lamp::core {

// ============================================================
// Runtime Context State
// ============================================================

enum class RuntimeContextState : std::uint8_t {
    Created,
    Running,
    Stopping,
    Stopped,
    Failed
};

// ============================================================
// Runtime Context Events
// ============================================================

enum class RuntimeContextEventType : std::uint8_t {
    Started,
    Stopping,
    Stopped,
    Failed
};

struct RuntimeContextEvent {
    RuntimeContextEventType type;
    RuntimeContextState state;
};

// ============================================================
// Runtime Context Configuration
// ============================================================

struct RuntimeContextConfig {
    std::size_t workerCount = 0;
};

// ============================================================
// Runtime Context Error
// ============================================================

class RuntimeContextError final
    : public std::runtime_error {

public:
    explicit RuntimeContextError(
        const std::string& message
    )
        : std::runtime_error(message) {}
};

// ============================================================
// Runtime Context
//
// Responsibilities:
//
// - Own the native WorkerPool
// - Own the TaskScheduler
// - Publish runtime lifecycle events
// - Coordinate initialization and shutdown
// - Prevent submissions after shutdown
// - Provide safe service access during runtime operation
//
// Ownership:
//
// RuntimeContext
//   |
//   +-- WorkerPool
//   |
//   +-- TaskScheduler
//   |
//   +-- EventDispatcher<RuntimeContextEvent>
//
// Lifecycle:
//
// Created
//    |
//    v
// Running
//    |
//    v
// Stopping
//    |
//    v
// Stopped
//
// Initialization failure:
// Created -> Failed
//
// The scheduler must shut down before the worker pool.
// ============================================================

class RuntimeContext final {
public:
    using EventCallback =
        std::function<void(const RuntimeContextEvent&)>;

    using EventSubscription =
        bool;

    // ========================================================
    // Construction
    // ========================================================

    explicit RuntimeContext(
        RuntimeContextConfig config = {}
    )
        : config_(std::move(config)) {}

    // ========================================================
    // Destruction
    // ========================================================

    ~RuntimeContext() noexcept {
        try {
            shutdown();
        } catch (...) {
            std::terminate();
        }
    }

    RuntimeContext(const RuntimeContext&) = delete;
    RuntimeContext& operator=(const RuntimeContext&) = delete;

    RuntimeContext(RuntimeContext&&) = delete;
    RuntimeContext& operator=(RuntimeContext&&) = delete;

    // ========================================================
    // Startup
    // ========================================================

    void start() {
        std::lock_guard<std::mutex> lifecycleLock(
            lifecycleMutex_
        );

        {
            std::lock_guard<std::mutex> lock(stateMutex_);

            if (state_ != RuntimeContextState::Created) {
                throw RuntimeContextError(
                    "RuntimeContext can only start "
                    "from the Created state"
                );
            }
        }

        try {
            auto workers =
                std::make_shared<WorkerPool>(
                    config_.workerCount
                );

            auto scheduler =
                std::make_shared<TaskScheduler>(
                    *workers
                );

            {
                std::lock_guard<std::mutex> lock(
                    stateMutex_
                );

                workers_ = std::move(workers);
                scheduler_ = std::move(scheduler);

                state_ = RuntimeContextState::Running;
            }

            publishEvent({
                RuntimeContextEventType::Started,
                RuntimeContextState::Running
            });

        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(
                    stateMutex_
                );

                state_ = RuntimeContextState::Failed;
            }

            publishEvent({
                RuntimeContextEventType::Failed,
                RuntimeContextState::Failed
            });

            throw;
        }
    }

    // ========================================================
    // Shutdown
    // ========================================================

    void shutdown(bool drain = true) {
        std::lock_guard<std::mutex> lifecycleLock(
            lifecycleMutex_
        );

        std::shared_ptr<TaskScheduler> scheduler;
        std::shared_ptr<WorkerPool> workers;

        {
            std::lock_guard<std::mutex> lock(stateMutex_);

            if (state_ == RuntimeContextState::Stopped) {
                return;
            }

            if (state_ == RuntimeContextState::Created) {
                state_ = RuntimeContextState::Stopped;

                publishEvent({
                    RuntimeContextEventType::Stopped,
                    RuntimeContextState::Stopped
                });

                return;
            }

            state_ = RuntimeContextState::Stopping;

            scheduler = scheduler_;
            workers = workers_;
        }

        publishEvent({
            RuntimeContextEventType::Stopping,
            RuntimeContextState::Stopping
        });

        try {
            // Scheduler shutdown must finish first.
            //
            // Scheduled callbacks may still need
            // the worker pool during shutdown.

            if (scheduler) {
                scheduler->shutdown(drain);
            }

            if (workers) {
                workers->shutdown(drain);
            }

            {
                std::lock_guard<std::mutex> lock(
                    stateMutex_
                );

                scheduler_.reset();
                workers_.reset();

                state_ = RuntimeContextState::Stopped;
            }

            publishEvent({
                RuntimeContextEventType::Stopped,
                RuntimeContextState::Stopped
            });

        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(
                    stateMutex_
                );

                state_ = RuntimeContextState::Failed;
            }

            publishEvent({
                RuntimeContextEventType::Failed,
                RuntimeContextState::Failed
            });

            throw;
        }
    }

    // ========================================================
    // Task Submission
    // ========================================================

    template <typename Function, typename... Args>
    [[nodiscard]]
    auto submit(
        Function&& function,
        Args&&... args
    ) -> std::future<
        std::invoke_result_t<
            std::decay_t<Function>,
            std::decay_t<Args>...
        >
    > {
        auto workers = acquireWorkers();

        return workers->submit(
            std::forward<Function>(function),
            std::forward<Args>(args)...
        );
    }

    // ========================================================
    // Service Access
    // ========================================================

    [[nodiscard]]
    std::shared_ptr<WorkerPool> workerPool() const {
        return acquireWorkers();
    }

    [[nodiscard]]
    std::shared_ptr<TaskScheduler> taskScheduler() const {
        std::lock_guard<std::mutex> lock(stateMutex_);

        if (state_ != RuntimeContextState::Running ||
            !scheduler_) {

            throw RuntimeContextError(
                "TaskScheduler is not available"
            );
        }

        return scheduler_;
    }

    // ========================================================
    // Event Subscription
    // ========================================================

    [[nodiscard]]
    EventSubscription onEvent(
        EventCallback callback
    ) {
        if (!callback) {
            return false;
        }

        return events_.subscribe(
            std::move(callback)
        );
    }

    // ========================================================
    // State Inspection
    // ========================================================

    [[nodiscard]]
    RuntimeContextState state() const noexcept {
        std::lock_guard<std::mutex> lock(stateMutex_);
        return state_;
    }

    [[nodiscard]]
    bool isRunning() const noexcept {
        return state() == RuntimeContextState::Running;
    }

    [[nodiscard]]
    bool isStopping() const noexcept {
        return state() == RuntimeContextState::Stopping;
    }

    [[nodiscard]]
    bool isStopped() const noexcept {
        return state() == RuntimeContextState::Stopped;
    }

    [[nodiscard]]
    bool hasFailed() const noexcept {
        return state() == RuntimeContextState::Failed;
    }

    [[nodiscard]]
    bool isAcceptingTasks() const noexcept {
        std::lock_guard<std::mutex> lock(stateMutex_);

        return state_ == RuntimeContextState::Running &&
               workers_ &&
               workers_->isAccepting();
    }

    [[nodiscard]]
    std::size_t workerCount() const noexcept {
        std::lock_guard<std::mutex> lock(stateMutex_);

        if (!workers_) {
            return 0;
        }

        return workers_->workerCount();
    }

    [[nodiscard]]
    RuntimeContextConfig config() const {
        return config_;
    }

private:
    // ========================================================
    // Worker Pool Acquisition
    // ========================================================

    [[nodiscard]]
    std::shared_ptr<WorkerPool>
    acquireWorkers() const {

        std::lock_guard<std::mutex> lock(stateMutex_);

        if (state_ != RuntimeContextState::Running ||
            !workers_) {

            throw RuntimeContextError(
                "WorkerPool is not available"
            );
        }

        return workers_;
    }

    // ========================================================
    // Event Publishing
    // ========================================================

    void publishEvent(
        RuntimeContextEvent event
    ) {
        events_.publish(std::move(event));
    }

    // ========================================================
    // Configuration
    // ========================================================

    RuntimeContextConfig config_;

    // ========================================================
    // Lifecycle Synchronization
    // ========================================================

    mutable std::mutex lifecycleMutex_;
    mutable std::mutex stateMutex_;

    RuntimeContextState state_ =
        RuntimeContextState::Created;

    // ========================================================
    // Runtime Services
    // ========================================================

    std::shared_ptr<WorkerPool> workers_;
    std::shared_ptr<TaskScheduler> scheduler_;

    // ========================================================
    // Runtime Events
    // ========================================================

    EventDispatcher<RuntimeContextEvent> events_;
};

} // namespace lamp::core
