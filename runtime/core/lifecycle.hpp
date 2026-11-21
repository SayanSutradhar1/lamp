
#pragma once

#include "core/error.hpp"
#include "core/result.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace lamp::core {

// ============================================================
// Runtime Lifecycle States
// ============================================================

enum class LifecycleState : std::uint8_t {
    Created,
    Initializing,
    Initialized,
    Starting,
    Running,
    Stopping,
    Stopped,
    Failed
};

// ============================================================
// State Name Conversion
// ============================================================

[[nodiscard]]
constexpr std::string_view lifecycleStateName(
    LifecycleState state
) noexcept {
    switch (state) {
        case LifecycleState::Created:
            return "CREATED";

        case LifecycleState::Initializing:
            return "INITIALIZING";

        case LifecycleState::Initialized:
            return "INITIALIZED";

        case LifecycleState::Starting:
            return "STARTING";

        case LifecycleState::Running:
            return "RUNNING";

        case LifecycleState::Stopping:
            return "STOPPING";

        case LifecycleState::Stopped:
            return "STOPPED";

        case LifecycleState::Failed:
            return "FAILED";
    }

    return "UNKNOWN";
}

// ============================================================
// State Transition Rules
// ============================================================

[[nodiscard]]
constexpr bool canTransition(
    LifecycleState from,
    LifecycleState to
) noexcept {
    if (from == to) {
        return false;
    }

    switch (from) {
        case LifecycleState::Created:
            return to == LifecycleState::Initializing ||
                   to == LifecycleState::Stopping ||
                   to == LifecycleState::Failed;

        case LifecycleState::Initializing:
            return to == LifecycleState::Initialized ||
                   to == LifecycleState::Stopping ||
                   to == LifecycleState::Failed;

        case LifecycleState::Initialized:
            return to == LifecycleState::Starting ||
                   to == LifecycleState::Stopping ||
                   to == LifecycleState::Failed;

        case LifecycleState::Starting:
            return to == LifecycleState::Running ||
                   to == LifecycleState::Stopping ||
                   to == LifecycleState::Failed;

        case LifecycleState::Running:
            return to == LifecycleState::Stopping ||
                   to == LifecycleState::Failed;

        case LifecycleState::Stopping:
            return to == LifecycleState::Stopped ||
                   to == LifecycleState::Failed;

        case LifecycleState::Failed:
            return to == LifecycleState::Stopping;

        case LifecycleState::Stopped:
            return false;
    }

    return false;
}

// ============================================================
// Lifecycle Controller
// ============================================================

class Lifecycle final {
public:
    Lifecycle() noexcept = default;

    Lifecycle(const Lifecycle&) = delete;
    Lifecycle& operator=(const Lifecycle&) = delete;

    Lifecycle(Lifecycle&&) = delete;
    Lifecycle& operator=(Lifecycle&&) = delete;

    ~Lifecycle() = default;

    // --------------------------------------------------------
    // State Inspection
    // --------------------------------------------------------

    [[nodiscard]]
    LifecycleState state() const noexcept {
        return state_.load(
            std::memory_order_acquire
        );
    }

    [[nodiscard]]
    bool isRunning() const noexcept {
        return state() == LifecycleState::Running;
    }

    [[nodiscard]]
    bool isStopping() const noexcept {
        return state() == LifecycleState::Stopping;
    }

    [[nodiscard]]
    bool isStopped() const noexcept {
        return state() == LifecycleState::Stopped;
    }

    [[nodiscard]]
    bool hasFailed() const noexcept {
        return state() == LifecycleState::Failed;
    }

    [[nodiscard]]
    bool isTerminal() const noexcept {
        return isStopped();
    }

    [[nodiscard]]
    bool isAcceptingRequests() const noexcept {
        return isRunning();
    }

    // --------------------------------------------------------
    // Atomic State Transition
    // --------------------------------------------------------

    [[nodiscard]]
    Result<void> transition(
        LifecycleState expected,
        LifecycleState desired
    ) {
        if (!canTransition(expected, desired)) {
            return Result<void>::failure(
                makeError(
                    ErrorCode::InvalidArgument,
                    "Invalid Lamp lifecycle transition from " +
                        std::string(lifecycleStateName(expected)) +
                        " to " +
                        std::string(lifecycleStateName(desired))
                )
            );
        }

        // Serialize state mutations with failure recording.
        // This prevents a transition from overwriting
        // a concurrently published failure state.
        std::lock_guard<std::mutex> lock(
            transitionMutex_
        );

        auto current = expected;

        if (!state_.compare_exchange_strong(
                current,
                desired,
                std::memory_order_acq_rel,
                std::memory_order_acquire
            )) {
            return Result<void>::failure(
                makeError(
                    ErrorCode::ExecutionFailed,
                    "Lamp lifecycle state mismatch. Expected " +
                        std::string(lifecycleStateName(expected)) +
                        ", actual " +
                        std::string(lifecycleStateName(current))
                )
            );
        }

        state_.notify_all();

        return Result<void>::success();
    }

    // --------------------------------------------------------
    // Initialization
    // --------------------------------------------------------

    [[nodiscard]]
    Result<void> beginInitialization() {
        return transition(
            LifecycleState::Created,
            LifecycleState::Initializing
        );
    }

    [[nodiscard]]
    Result<void> completeInitialization() {
        return transition(
            LifecycleState::Initializing,
            LifecycleState::Initialized
        );
    }

    // --------------------------------------------------------
    // Startup
    // --------------------------------------------------------

    [[nodiscard]]
    Result<void> beginStartup() {
        return transition(
            LifecycleState::Initialized,
            LifecycleState::Starting
        );
    }

    [[nodiscard]]
    Result<void> completeStartup() {
        return transition(
            LifecycleState::Starting,
            LifecycleState::Running
        );
    }

    // --------------------------------------------------------
    // Graceful Shutdown
    // --------------------------------------------------------

    [[nodiscard]]
    Result<void> beginShutdown() {
        std::lock_guard<std::mutex> lock(
            transitionMutex_
        );

        const auto current = state_.load(
            std::memory_order_acquire
        );

        if (current == LifecycleState::Stopping ||
            current == LifecycleState::Stopped) {
            return Result<void>::success();
        }

        if (!canTransition(
                current,
                LifecycleState::Stopping
            )) {
            return Result<void>::failure(
                makeError(
                    ErrorCode::ExecutionFailed,
                    "Cannot begin Lamp shutdown from state " +
                        std::string(lifecycleStateName(current))
                )
            );
        }

        state_.store(
            LifecycleState::Stopping,
            std::memory_order_release
        );

        state_.notify_all();

        return Result<void>::success();
    }

    [[nodiscard]]
    Result<void> completeShutdown() {
        return transition(
            LifecycleState::Stopping,
            LifecycleState::Stopped
        );
    }

    // --------------------------------------------------------
    // Failure Handling
    // --------------------------------------------------------

    [[nodiscard]]
    Result<void> fail(LampError error) {
        // Lock ordering:
        //
        // transitionMutex_ -> failureMutex_
        //
        // This order must remain consistent wherever
        // both locks are acquired.

        std::lock_guard<std::mutex> transitionLock(
            transitionMutex_
        );

        const auto current = state_.load(
            std::memory_order_acquire
        );

        if (current == LifecycleState::Failed) {
            return Result<void>::success();
        }

        if (!canTransition(
                current,
                LifecycleState::Failed
            )) {
            return Result<void>::failure(
                makeError(
                    ErrorCode::ExecutionFailed,
                    "Cannot mark Lamp runtime as failed "
                    "from state " +
                        std::string(lifecycleStateName(current))
                )
            );
        }

        {
            std::lock_guard<std::mutex> failureLock(
                failureMutex_
            );

            // Publish failure details before the Failed
            // state becomes visible to other threads.
            failure_.emplace(std::move(error));
        }

        state_.store(
            LifecycleState::Failed,
            std::memory_order_release
        );

        state_.notify_all();

        return Result<void>::success();
    }

    // --------------------------------------------------------
    // Failure Inspection
    // --------------------------------------------------------

    [[nodiscard]]
    bool hasFailureDetails() const {
        std::lock_guard<std::mutex> lock(
            failureMutex_
        );

        return failure_.has_value();
    }

    [[nodiscard]]
    std::optional<LampError> failure() const {
        std::lock_guard<std::mutex> lock(
            failureMutex_
        );

        return failure_;
    }

    // --------------------------------------------------------
    // State Waiting
    // --------------------------------------------------------

    void waitForStateChange(
        LifecycleState observed
    ) const noexcept {
        state_.wait(
            observed,
            std::memory_order_acquire
        );
    }

private:
    // ========================================================
    // Internal State
    // ========================================================

    std::atomic<LifecycleState> state_{
        LifecycleState::Created
    };

    // Serializes all lifecycle state mutations.
    mutable std::mutex transitionMutex_;

    // Protects failure details independently.
    mutable std::mutex failureMutex_;

    std::optional<LampError> failure_;
};

} // namespace lamp::core
