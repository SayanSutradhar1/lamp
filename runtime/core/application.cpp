
#include "core/application.hpp"

#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace lamp::core {

namespace {

// ============================================================
// Error Helpers
// ============================================================

[[nodiscard]]
LampError componentError(
    ErrorCode code,
    const std::string& component,
    const std::string& operation,
    const LampError& cause
) {
    return makeError(
        code,
        "Lamp component '" + component +
            "' failed during " + operation +
            ": " + cause.message()
    ).withCause(
        std::make_shared<LampError>(cause)
    );
}

[[nodiscard]]
LampError exceptionError(
    const std::string& operation,
    const std::exception& exception
) {
    return makeError(
        ErrorCode::InternalError,
        "Unexpected exception during " +
            operation + ": " + exception.what()
    );
}

[[nodiscard]]
LampError unknownExceptionError(
    const std::string& operation
) {
    return makeError(
        ErrorCode::InternalError,
        "Unknown exception during " + operation
    );
}

[[nodiscard]]
Result<void> failureResult(
    LampError error
) {
    return Result<void>::failure(
        std::move(error)
    );
}

} // anonymous namespace

// ============================================================
// Construction
// ============================================================

LampApplication::LampApplication(
    LampConfig config
)
    : config_(std::move(config)),
      originPolicy_(std::make_shared<OriginAllowancePolicy>(config_.originAllowance)) {
}

// ============================================================
// Destruction
// ============================================================

LampApplication::~LampApplication() noexcept {
    try {
        if (!lifecycle_.isStopped()) {
            (void)shutdown();
        }
    } catch (...) {
        // Destructors must never propagate exceptions.
    }
}

// ============================================================
// Configuration
// ============================================================

const LampConfig&
LampApplication::config() const noexcept {
    return config_;
}

Result<void> LampApplication::configure(
    LampConfig config
) {
    std::lock_guard<std::mutex> lock(
        operationMutex_
    );

    if (lifecycle_.state() != LifecycleState::Created) {
        return failureResult(
            makeError(
                ErrorCode::InvalidConfiguration,
                "Lamp configuration can only be changed "
                "before initialization."
            )
        );
    }

    const auto errors = config.validate();

    if (!errors.empty()) {
        return failureResult(
            makeError(
                ErrorCode::InvalidConfiguration,
                errors.front()
            )
        );
    }

    config_ = std::move(config);
    originPolicy_->configure(config_.originAllowance);

    return Result<void>::success();
}

// ============================================================
// Component Registration
// ============================================================

Result<void> LampApplication::registerComponent(
    std::unique_ptr<IRuntimeComponent> component
) {
    std::lock_guard<std::mutex> lock(
        operationMutex_
    );

    if (!component) {
        return failureResult(
            makeError(
                ErrorCode::InvalidArgument,
                "Cannot register a null Lamp component."
            )
        );
    }

    if (lifecycle_.state() != LifecycleState::Created) {
        return failureResult(
            makeError(
                ErrorCode::InvalidArgument,
                "Components must be registered "
                "before initialization."
            )
        );
    }

    components_.push_back(
        ComponentEntry{
            std::move(component),
            false,
            false,
            ComponentCleanupState::NotRequired
        }
    );

    return Result<void>::success();
}

// ============================================================
// Configuration Validation
// ============================================================

Result<void>
LampApplication::validateConfiguration() const {
    const auto errors = config_.validate();

    if (!errors.empty()) {
        return failureResult(
            makeError(
                ErrorCode::InvalidConfiguration,
                errors.front()
            )
        );
    }

    return Result<void>::success();
}

// ============================================================
// Component Initialization
// ============================================================

Result<void>
LampApplication::initializeComponents() {
    for (auto& entry : components_) {
        // Cleanup becomes required before entering
        // initialize(), since partial initialization
        // may acquire native resources.
        entry.cleanup =
            ComponentCleanupState::Required;

        try {
            const auto componentName =
                entry.instance->name();

            auto result =
                entry.instance->initialize(config_);

            if (!result) {
                return failureResult(
                    componentError(
                        ErrorCode::ExecutionFailed,
                        componentName,
                        "initialization",
                        result.error()
                    )
                );
            }

            entry.initialized = true;
        } catch (const std::exception& exception) {
            return failureResult(
                exceptionError(
                    "component initialization",
                    exception
                )
            );
        } catch (...) {
            return failureResult(
                unknownExceptionError(
                    "component initialization"
                )
            );
        }
    }

    return Result<void>::success();
}

// ============================================================
// Component Startup
// ============================================================

Result<void>
LampApplication::startComponents() {
    for (auto& entry : components_) {
        if (!entry.initialized) {
            return failureResult(
                makeError(
                    ErrorCode::ExecutionFailed,
                    "Cannot start an uninitialized "
                    "Lamp component."
                )
            );
        }

        try {
            const auto componentName =
                entry.instance->name();

            auto result = entry.instance->start();

            if (!result) {
                return failureResult(
                    componentError(
                        ErrorCode::ExecutionFailed,
                        componentName,
                        "startup",
                        result.error()
                    )
                );
            }

            entry.started = true;
        } catch (const std::exception& exception) {
            return failureResult(
                exceptionError(
                    "component startup",
                    exception
                )
            );
        } catch (...) {
            return failureResult(
                unknownExceptionError(
                    "component startup"
                )
            );
        }
    }

    return Result<void>::success();
}

// ============================================================
// Component Cleanup
// ============================================================

Result<void>
LampApplication::stopComponents() {
    std::optional<LampError> firstError;

    for (auto it = components_.rbegin();
         it != components_.rend();
         ++it) {

        auto& entry = *it;

        if (entry.cleanup !=
            ComponentCleanupState::Required) {
            continue;
        }

        try {
            const auto componentName =
                entry.instance->name();

            auto result = entry.instance->stop();

            if (!result) {
                if (!firstError) {
                    firstError = componentError(
                        ErrorCode::ExecutionFailed,
                        componentName,
                        "shutdown",
                        result.error()
                    );
                }

                // Retain Required for a later retry.
                continue;
            }

            entry.started = false;
            entry.initialized = false;
            entry.cleanup =
                ComponentCleanupState::Completed;

        } catch (const std::exception& exception) {
            if (!firstError) {
                firstError = exceptionError(
                    "component shutdown",
                    exception
                );
            }
        } catch (...) {
            if (!firstError) {
                firstError = unknownExceptionError(
                    "component shutdown"
                );
            }
        }
    }

    if (firstError) {
        return failureResult(
            std::move(*firstError)
        );
    }

    return Result<void>::success();
}

// ============================================================
// Application Initialization
// ============================================================

Result<void> LampApplication::initialize() {
    std::lock_guard<std::mutex> lock(
        operationMutex_
    );

    auto transition =
        lifecycle_.beginInitialization();

    if (!transition) {
        return transition;
    }

    Result<void> outcome = Result<void>::success();

    try {
        outcome = validateConfiguration();

        if (outcome) {
            outcome = initializeComponents();
        }

        if (outcome) {
            outcome =
                lifecycle_.completeInitialization();
        }
    } catch (const std::exception& exception) {
        outcome = failureResult(
            exceptionError(
                "application initialization",
                exception
            )
        );
    } catch (...) {
        outcome = failureResult(
            unknownExceptionError(
                "application initialization"
            )
        );
    }

    if (!outcome) {
        recordFailure(outcome.error());
    }

    // Dispatch while holding operationMutex_ so another
    // lifecycle operation cannot overtake this event.
    //
    // Callbacks must not call mutating LampApplication
    // methods from this synchronous notification.

    emitEvent(
        outcome
            ? ApplicationEvent::Initialized
            : ApplicationEvent::Failed
    );

    return outcome;
}

// ============================================================
// Application Startup
// ============================================================

Result<void> LampApplication::start() {
    std::lock_guard<std::mutex> lock(
        operationMutex_
    );

    auto transition = lifecycle_.beginStartup();

    if (!transition) {
        return transition;
    }

    Result<void> outcome = Result<void>::success();

    try {
        outcome = startComponents();

        if (outcome) {
            outcome = lifecycle_.completeStartup();
        }
    } catch (const std::exception& exception) {
        outcome = failureResult(
            exceptionError(
                "application startup",
                exception
            )
        );
    } catch (...) {
        outcome = failureResult(
            unknownExceptionError(
                "application startup"
            )
        );
    }

    if (!outcome) {
        recordFailure(outcome.error());
    }

    emitEvent(
        outcome
            ? ApplicationEvent::Started
            : ApplicationEvent::Failed
    );

    return outcome;
}

// ============================================================
// Graceful Shutdown
// ============================================================

Result<void> LampApplication::shutdown() {
    std::lock_guard<std::mutex> lock(
        operationMutex_
    );

    if (lifecycle_.isStopped()) {
        return Result<void>::success();
    }

    auto transition = lifecycle_.beginShutdown();

    if (!transition) {
        return transition;
    }

    // Publish the Stopping event before component cleanup.
    // The lifecycle state is already Stopping here.
    emitEvent(ApplicationEvent::Stopping);

    Result<void> outcome = Result<void>::success();

    try {
        outcome = stopComponents();

        if (outcome) {
            outcome = lifecycle_.completeShutdown();
        }
    } catch (const std::exception& exception) {
        outcome = failureResult(
            exceptionError(
                "application shutdown",
                exception
            )
        );
    } catch (...) {
        outcome = failureResult(
            unknownExceptionError(
                "application shutdown"
            )
        );
    }

    if (!outcome) {
        recordFailure(outcome.error());
    }

    emitEvent(
        outcome
            ? ApplicationEvent::Stopped
            : ApplicationEvent::Failed
    );

    return outcome;
}

// ============================================================
// Failure Recording
// ============================================================

void LampApplication::recordFailure(
    LampError error
) {
    (void)lifecycle_.fail(
        std::move(error)
    );
}

// ============================================================
// Event Registration
// ============================================================

Result<void> LampApplication::onEvent(
    ApplicationEventCallback callback
) {
    std::lock_guard<std::mutex> operationLock(
        operationMutex_
    );

    if (lifecycle_.state() != LifecycleState::Created) {
        return failureResult(
            makeError(
                ErrorCode::InvalidArgument,
                "Event callbacks must be registered "
                "before initialization."
            )
        );
    }

    if (!callback) {
        return failureResult(
            makeError(
                ErrorCode::InvalidArgument,
                "Cannot register an empty event callback."
            )
        );
    }

    std::lock_guard<std::mutex> eventLock(
        eventMutex_
    );

    eventCallbacks_.push_back(
        std::move(callback)
    );

    return Result<void>::success();
}

// ============================================================
// Event Dispatch
// ============================================================

void LampApplication::emitEvent(
    ApplicationEvent event
) {
    std::vector<ApplicationEventCallback> callbacks;

    {
        std::lock_guard<std::mutex> lock(
            eventMutex_
        );

        callbacks = eventCallbacks_;
    }

    ApplicationEventInfo info{
        event,
        lifecycle_.state(),
        config_.applicationName
    };

    for (const auto& callback : callbacks) {
        try {
            callback(info);
        } catch (...) {
            // Event listener exceptions must not affect
            // application lifecycle operations.
        }
    }
}

// ============================================================
// Lifecycle Inspection
// ============================================================

LifecycleState
LampApplication::state() const noexcept {
    return lifecycle_.state();
}

bool LampApplication::isRunning() const noexcept {
    return lifecycle_.isRunning();
}

bool LampApplication::isStopping() const noexcept {
    return lifecycle_.isStopping();
}

bool LampApplication::isStopped() const noexcept {
    return lifecycle_.isStopped();
}

bool LampApplication::hasFailed() const noexcept {
    return lifecycle_.hasFailed();
}

bool LampApplication::isAcceptingRequests() const noexcept {
    return lifecycle_.isAcceptingRequests();
}

// ============================================================
// Failure Inspection
// ============================================================

std::optional<LampError>
LampApplication::failure() const {
    return lifecycle_.failure();
}

// ============================================================
// Application Information
// ============================================================

const std::string&
LampApplication::name() const noexcept {
    return config_.applicationName;
}

std::shared_ptr<OriginAllowancePolicy>
LampApplication::originPolicy() const noexcept {
    return originPolicy_;
}

bool LampApplication::isOriginAllowed(
    std::string_view origin
) const {
    return originPolicy_->isOriginAllowed(origin);
}

OriginEvaluationResult LampApplication::evaluateOrigin(
    std::optional<std::string_view> origin
) const {
    return originPolicy_->evaluate(origin);
}

std::size_t
LampApplication::componentCount() const noexcept {
    return components_.size();
}

// ============================================================
// Native Application Factory
// ============================================================

std::unique_ptr<LampApplication>
createLampApplication(
    LampConfig config
) {
    return std::make_unique<LampApplication>(
        std::move(config)
    );
}

} // namespace lamp::core
