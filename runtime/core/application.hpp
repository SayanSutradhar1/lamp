
#pragma once

#include "core/config.hpp"
#include "core/error.hpp"
#include "core/lifecycle.hpp"
#include "core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lamp::core {

// ============================================================
// Runtime Component Interface
// ============================================================

class IRuntimeComponent {
public:
    virtual ~IRuntimeComponent() = default;

    IRuntimeComponent(const IRuntimeComponent&) = delete;
    IRuntimeComponent& operator=(
        const IRuntimeComponent&
    ) = delete;

    IRuntimeComponent(IRuntimeComponent&&) = delete;
    IRuntimeComponent& operator=(
        IRuntimeComponent&&
    ) = delete;

    [[nodiscard]]
    virtual std::string name() const = 0;

    [[nodiscard]]
    virtual Result<void> initialize(
        const LampConfig& config
    ) = 0;

    [[nodiscard]]
    virtual Result<void> start() = 0;

    [[nodiscard]]
    virtual Result<void> stop() = 0;

protected:
    IRuntimeComponent() = default;
};

// ============================================================
// Component Cleanup State
// ============================================================

enum class ComponentCleanupState : std::uint8_t {
    // Registered but initialize() has not been called.
    NotRequired,

    // initialize() was entered and may have acquired
    // resources. Cleanup must be attempted.
    Required,

    // stop() completed successfully.
    Completed
};

[[nodiscard]]
constexpr std::string_view componentCleanupStateName(
    ComponentCleanupState state
) noexcept {
    switch (state) {
        case ComponentCleanupState::NotRequired:
            return "NOT_REQUIRED";

        case ComponentCleanupState::Required:
            return "REQUIRED";

        case ComponentCleanupState::Completed:
            return "COMPLETED";
    }

    return "UNKNOWN";
}

// ============================================================
// Application Lifecycle Events
// ============================================================

enum class ApplicationEvent : std::uint8_t {
    Initialized,
    Started,
    Stopping,
    Stopped,
    Failed
};

struct ApplicationEventInfo {
    ApplicationEvent event;
    LifecycleState state;
    std::string applicationName;
};

using ApplicationEventCallback =
    std::function<void(const ApplicationEventInfo&)>;

// ============================================================
// Lamp Application
// ============================================================

class LampApplication final {
public:
    // --------------------------------------------------------
    // Construction and Ownership
    // --------------------------------------------------------

    explicit LampApplication(
        LampConfig config = {}
    );

    ~LampApplication() noexcept;

    LampApplication(const LampApplication&) = delete;

    LampApplication& operator=(
        const LampApplication&
    ) = delete;

    LampApplication(LampApplication&&) = delete;

    LampApplication& operator=(
        LampApplication&&
    ) = delete;

    // --------------------------------------------------------
    // Configuration
    // --------------------------------------------------------

    [[nodiscard]]
    const LampConfig& config() const noexcept;

    [[nodiscard]]
    Result<void> configure(
        LampConfig config
    );

    // --------------------------------------------------------
    // Component Registration
    // --------------------------------------------------------

    [[nodiscard]]
    Result<void> registerComponent(
        std::unique_ptr<IRuntimeComponent> component
    );

    [[nodiscard]]
    std::size_t componentCount() const noexcept;

    // --------------------------------------------------------
    // Application Lifecycle
    // --------------------------------------------------------

    [[nodiscard]]
    Result<void> initialize();

    [[nodiscard]]
    Result<void> start();

    [[nodiscard]]
    Result<void> shutdown();

    // --------------------------------------------------------
    // Lifecycle Inspection
    // --------------------------------------------------------

    [[nodiscard]]
    LifecycleState state() const noexcept;

    [[nodiscard]]
    bool isRunning() const noexcept;

    [[nodiscard]]
    bool isStopping() const noexcept;

    [[nodiscard]]
    bool isStopped() const noexcept;

    [[nodiscard]]
    bool hasFailed() const noexcept;

    [[nodiscard]]
    bool isAcceptingRequests() const noexcept;

    // --------------------------------------------------------
    // Failure Inspection
    // --------------------------------------------------------

    [[nodiscard]]
    std::optional<LampError> failure() const;

    // --------------------------------------------------------
    // Application Information
    // --------------------------------------------------------

    [[nodiscard]]
    const std::string& name() const noexcept;

    // --------------------------------------------------------
    // Lifecycle Events
    // --------------------------------------------------------

    [[nodiscard]]
    Result<void> onEvent(
        ApplicationEventCallback callback
    );

private:
    // ========================================================
    // Component Bookkeeping
    // ========================================================

    struct ComponentEntry {
        std::unique_ptr<IRuntimeComponent> instance;

        // True only after initialize() succeeds.
        bool initialized = false;

        // True only after start() succeeds.
        bool started = false;

        // Tracks whether stop() is needed, including
        // partially completed initialization.
        ComponentCleanupState cleanup =
            ComponentCleanupState::NotRequired;
    };

    // ========================================================
    // Internal Lifecycle Operations
    // ========================================================

    [[nodiscard]]
    Result<void> validateConfiguration() const;

    [[nodiscard]]
    Result<void> initializeComponents();

    [[nodiscard]]
    Result<void> startComponents();

    [[nodiscard]]
    Result<void> stopComponents();

    // --------------------------------------------------------
    // Failure Recording
    // --------------------------------------------------------

    void recordFailure(
        LampError error
    );

    // --------------------------------------------------------
    // Event Dispatch
    // --------------------------------------------------------

    void emitEvent(
        ApplicationEvent event
    );

    // ========================================================
    // Application State
    // ========================================================

    LampConfig config_;

    Lifecycle lifecycle_;

    // Serializes configuration, registration,
    // initialization, startup and shutdown.
    //
    // Component lifecycle callbacks execute while this
    // mutex is held. Components must not synchronously
    // call back into mutating LampApplication methods.
    mutable std::mutex operationMutex_;

    std::vector<ComponentEntry> components_;

    // Protects callback registration and snapshots.
    mutable std::mutex eventMutex_;

    std::vector<ApplicationEventCallback> eventCallbacks_;
};

// ============================================================
// Native Application Factory
// ============================================================

[[nodiscard]]
std::unique_ptr<LampApplication> createLampApplication(
    LampConfig config = {}
);

} // namespace lamp::core
