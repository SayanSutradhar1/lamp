
#include "core/application.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace lamp::core;
using namespace std::chrono_literals;

// ============================================================
// Minimal Test Framework
// ============================================================

class TestFailure final : public std::runtime_error {
public:
    explicit TestFailure(std::string message)
        : std::runtime_error(std::move(message)) {}
};

#define LAMP_ASSERT(condition)                              \
    do {                                                    \
        if (!(condition)) {                                 \
            throw TestFailure(                              \
                std::string("Assertion failed: ") +         \
                #condition + " at line " +                  \
                std::to_string(__LINE__)                    \
            );                                              \
        }                                                   \
    } while (false)

#define LAMP_ASSERT_EQ(actual, expected)                    \
    do {                                                    \
        const auto& lamp_actual = (actual);                  \
        const auto& lamp_expected = (expected);              \
        LAMP_ASSERT(lamp_actual == lamp_expected);           \
    } while (false)

void requireSuccess(Result<void> result) {
    if (!result) {
        throw TestFailure(
            "Unexpected failure: " +
            result.error().message()
        );
    }
}

// ============================================================
// Thread-Safe Mock State
// ============================================================

struct MockState {
    std::atomic<int> initializeCalls{0};
    std::atomic<int> startCalls{0};
    std::atomic<int> stopCalls{0};

    std::atomic<int> activeStops{0};
    std::atomic<int> maximumActiveStops{0};

    std::atomic<int> initializeFailuresRemaining{0};
    std::atomic<int> startFailuresRemaining{0};
    std::atomic<int> stopFailuresRemaining{0};

    std::atomic<bool> throwOnInitialize{false};
    std::atomic<bool> throwOnStart{false};
    std::atomic<bool> throwOnStop{false};

    std::atomic<bool> initializationEntered{false};
    std::atomic<bool> stopEntered{false};

    std::mutex gateMutex;
    std::condition_variable gateCondition;
    bool releaseStop = true;
};

// ============================================================
// Mock Helpers
// ============================================================

bool consumeFailure(std::atomic<int>& remaining) {
    int current = remaining.load(
        std::memory_order_acquire
    );

    while (current > 0) {
        if (remaining.compare_exchange_weak(
                current,
                current - 1,
                std::memory_order_acq_rel,
                std::memory_order_acquire
            )) {
            return true;
        }
    }

    return false;
}

void updateMaximum(
    std::atomic<int>& maximum,
    int value
) {
    int current = maximum.load(
        std::memory_order_relaxed
    );

    while (
        current < value &&
        !maximum.compare_exchange_weak(
            current,
            value,
            std::memory_order_relaxed
        )
    ) {
    }
}

// ============================================================
// Mock Runtime Component
// ============================================================

class MockRuntimeComponent final
    : public IRuntimeComponent {
public:
    MockRuntimeComponent(
        std::string componentName,
        std::shared_ptr<MockState> state,
        std::vector<std::string>* operationLog = nullptr
    )
        : name_(std::move(componentName)),
          state_(std::move(state)),
          operationLog_(operationLog) {}

    [[nodiscard]]
    std::string name() const override {
        return name_;
    }

    [[nodiscard]]
    Result<void> initialize(
        const LampConfig&
    ) override {
        state_->initializeCalls.fetch_add(1);

        state_->initializationEntered.store(
            true,
            std::memory_order_release
        );

        log("initialize:" + name_);

        if (state_->throwOnInitialize.load()) {
            throw std::runtime_error(
                "Mock initialization exception"
            );
        }

        if (consumeFailure(
                state_->initializeFailuresRemaining
            )) {
            return Result<void>::failure(
                makeError(
                    ErrorCode::ExecutionFailed,
                    "Mock initialization failure"
                )
            );
        }

        return Result<void>::success();
    }

    [[nodiscard]]
    Result<void> start() override {
        state_->startCalls.fetch_add(1);

        log("start:" + name_);

        if (state_->throwOnStart.load()) {
            throw std::runtime_error(
                "Mock startup exception"
            );
        }

        if (consumeFailure(
                state_->startFailuresRemaining
            )) {
            return Result<void>::failure(
                makeError(
                    ErrorCode::ExecutionFailed,
                    "Mock startup failure"
                )
            );
        }

        return Result<void>::success();
    }

    [[nodiscard]]
    Result<void> stop() override {
        state_->stopCalls.fetch_add(1);

        const int active =
            state_->activeStops.fetch_add(1) + 1;

        updateMaximum(
            state_->maximumActiveStops,
            active
        );

        struct StopGuard {
            std::atomic<int>& counter;

            ~StopGuard() {
                counter.fetch_sub(1);
            }
        } guard{state_->activeStops};

        log("stop:" + name_);

        state_->stopEntered.store(
            true,
            std::memory_order_release
        );

        {
            std::unique_lock<std::mutex> lock(
                state_->gateMutex
            );

            state_->gateCondition.wait(
                lock,
                [&]() {
                    return state_->releaseStop;
                }
            );
        }

        if (state_->throwOnStop.load()) {
            throw std::runtime_error(
                "Mock shutdown exception"
            );
        }

        if (consumeFailure(
                state_->stopFailuresRemaining
            )) {
            return Result<void>::failure(
                makeError(
                    ErrorCode::ExecutionFailed,
                    "Mock shutdown failure"
                )
            );
        }

        return Result<void>::success();
    }

private:
    void log(const std::string& operation) {
        // Logs are only used by sequential tests.
        // Their lifetime must exceed LampApplication's.
        if (operationLog_ != nullptr) {
            operationLog_->push_back(operation);
        }
    }

    std::string name_;
    std::shared_ptr<MockState> state_;
    std::vector<std::string>* operationLog_;
};

// ============================================================
// Application Helpers
// ============================================================

std::shared_ptr<MockState> addMock(
    LampApplication& app,
    const std::string& name,
    std::vector<std::string>* log = nullptr
) {
    auto state = std::make_shared<MockState>();

    requireSuccess(
        app.registerComponent(
            std::make_unique<MockRuntimeComponent>(
                name,
                state,
                log
            )
        )
    );

    return state;
}

void initializeAndStart(LampApplication& app) {
    requireSuccess(app.initialize());
    requireSuccess(app.start());
}

// ============================================================
// Test 1: Construction
// ============================================================

void testConstruction() {
    auto app = createLampApplication(LampConfig{});

    LAMP_ASSERT(app != nullptr);

    LAMP_ASSERT_EQ(
        app->state(),
        LifecycleState::Created
    );

    LAMP_ASSERT_EQ(app->componentCount(), 0U);
}

// ============================================================
// Test 2: Application Factory
// ============================================================

void testApplicationFactory() {
    auto app = createLampApplication(LampConfig{});

    LAMP_ASSERT(app != nullptr);
    LAMP_ASSERT(!app->isRunning());
    LAMP_ASSERT(!app->isStopped());
}

// ============================================================
// Test 3: Null Component
// ============================================================

void testNullComponent() {
    LampApplication app(LampConfig{});

    std::unique_ptr<IRuntimeComponent> component;

    auto result = app.registerComponent(
        std::move(component)
    );

    LAMP_ASSERT(!result);
    LAMP_ASSERT_EQ(app.componentCount(), 0U);
}

// ============================================================
// Test 4: Component Registration
// ============================================================

void testComponentRegistration() {
    LampApplication app(LampConfig{});

    addMock(app, "first");
    addMock(app, "second");

    LAMP_ASSERT_EQ(app.componentCount(), 2U);
}

// ============================================================
// Test 5: Initialization
// ============================================================

void testInitialization() {
    LampApplication app(LampConfig{});

    auto state = addMock(app, "component");

    requireSuccess(app.initialize());

    LAMP_ASSERT_EQ(
        app.state(),
        LifecycleState::Initialized
    );

    LAMP_ASSERT_EQ(
        state->initializeCalls.load(),
        1
    );
}

// ============================================================
// Test 6: Startup
// ============================================================

void testStartup() {
    LampApplication app(LampConfig{});

    auto state = addMock(app, "component");

    initializeAndStart(app);

    LAMP_ASSERT(app.isRunning());
    LAMP_ASSERT(app.isAcceptingRequests());
    LAMP_ASSERT_EQ(state->startCalls.load(), 1);
}

// ============================================================
// Test 7: Normal Shutdown
// ============================================================

void testNormalShutdown() {
    LampApplication app(LampConfig{});

    auto state = addMock(app, "component");

    initializeAndStart(app);
    requireSuccess(app.shutdown());

    LAMP_ASSERT(app.isStopped());
    LAMP_ASSERT(!app.isAcceptingRequests());
    LAMP_ASSERT_EQ(state->stopCalls.load(), 1);
}

// ============================================================
// Test 8: Repeated Shutdown
// ============================================================

void testRepeatedShutdown() {
    LampApplication app(LampConfig{});

    auto state = addMock(app, "component");

    initializeAndStart(app);

    requireSuccess(app.shutdown());
    requireSuccess(app.shutdown());
    requireSuccess(app.shutdown());

    LAMP_ASSERT_EQ(state->stopCalls.load(), 1);
}

// ============================================================
// Test 9: Initialization Order
// ============================================================

void testInitializationOrder() {
    std::vector<std::string> log;
    LampApplication app(LampConfig{});

    addMock(app, "first", &log);
    addMock(app, "second", &log);

    requireSuccess(app.initialize());

    LAMP_ASSERT_EQ(log.size(), 2U);
    LAMP_ASSERT_EQ(log.at(0), "initialize:first");
    LAMP_ASSERT_EQ(log.at(1), "initialize:second");

    requireSuccess(app.shutdown());
}

// ============================================================
// Test 10: Startup Order
// ============================================================

void testStartupOrder() {
    std::vector<std::string> log;
    LampApplication app(LampConfig{});

    addMock(app, "first", &log);
    addMock(app, "second", &log);

    initializeAndStart(app);

    LAMP_ASSERT_EQ(log.size(), 4U);
    LAMP_ASSERT_EQ(log.at(2), "start:first");
    LAMP_ASSERT_EQ(log.at(3), "start:second");

    requireSuccess(app.shutdown());
}

// ============================================================
// Test 11: Reverse Shutdown Order
// ============================================================

void testReverseShutdownOrder() {
    std::vector<std::string> log;
    LampApplication app(LampConfig{});

    addMock(app, "first", &log);
    addMock(app, "second", &log);

    initializeAndStart(app);
    requireSuccess(app.shutdown());

    LAMP_ASSERT_EQ(log.size(), 6U);
    LAMP_ASSERT_EQ(log.at(4), "stop:second");
    LAMP_ASSERT_EQ(log.at(5), "stop:first");
}

// ============================================================
// Test 12: Invalid Startup
// ============================================================

void testInvalidStartup() {
    LampApplication app(LampConfig{});

    auto result = app.start();

    LAMP_ASSERT(!result);

    LAMP_ASSERT_EQ(
        app.state(),
        LifecycleState::Created
    );
}

// ============================================================
// Test 13: Late Registration
// ============================================================

void testLateRegistration() {
    LampApplication app(LampConfig{});

    requireSuccess(app.initialize());

    auto result = app.registerComponent(
        std::make_unique<MockRuntimeComponent>(
            "late",
            std::make_shared<MockState>()
        )
    );

    LAMP_ASSERT(!result);
}

// ============================================================
// Test 14: Lifecycle Events
// ============================================================

void testLifecycleEvents() {
    std::vector<ApplicationEvent> events;
    LampApplication app(LampConfig{});

    requireSuccess(
        app.onEvent(
            [&](const ApplicationEventInfo& info) {
                events.push_back(info.event);
            }
        )
    );

    initializeAndStart(app);
    requireSuccess(app.shutdown());

    LAMP_ASSERT_EQ(events.size(), 4U);

    LAMP_ASSERT_EQ(
        events.at(0),
        ApplicationEvent::Initialized
    );

    LAMP_ASSERT_EQ(
        events.at(1),
        ApplicationEvent::Started
    );

    LAMP_ASSERT_EQ(
        events.at(2),
        ApplicationEvent::Stopping
    );

    LAMP_ASSERT_EQ(
        events.at(3),
        ApplicationEvent::Stopped
    );
}

// ============================================================
// Test 15: Initialization Failure Cleanup
// ============================================================

void testInitializationFailureCleanup() {
    LampApplication app(LampConfig{});

    auto state = addMock(app, "failing-init");

    state->initializeFailuresRemaining.store(1);

    auto result = app.initialize();

    LAMP_ASSERT(!result);
    LAMP_ASSERT(app.hasFailed());
    LAMP_ASSERT(app.failure().has_value());

    requireSuccess(app.shutdown());

    // initialize() was entered, so cleanup is required
    // even though initialization did not succeed.
    LAMP_ASSERT_EQ(state->initializeCalls.load(), 1);
    LAMP_ASSERT_EQ(state->stopCalls.load(), 1);
    LAMP_ASSERT(app.isStopped());
}

// ============================================================
// Test 16: Partial Initialization Cleanup
// ============================================================

void testPartialInitializationCleanup() {
    LampApplication app(LampConfig{});

    auto first = addMock(app, "first");
    auto second = addMock(app, "second");
    auto third = addMock(app, "third");

    second->initializeFailuresRemaining.store(1);

    auto result = app.initialize();

    LAMP_ASSERT(!result);
    LAMP_ASSERT(app.hasFailed());

    LAMP_ASSERT_EQ(first->initializeCalls.load(), 1);
    LAMP_ASSERT_EQ(second->initializeCalls.load(), 1);
    LAMP_ASSERT_EQ(third->initializeCalls.load(), 0);

    requireSuccess(app.shutdown());

    // First initialized successfully.
    LAMP_ASSERT_EQ(first->stopCalls.load(), 1);

    // Second entered initialize(), then failed.
    // Its partial resources must be cleaned up.
    LAMP_ASSERT_EQ(second->stopCalls.load(), 1);

    // Third never entered initialize().
    LAMP_ASSERT_EQ(third->stopCalls.load(), 0);

    LAMP_ASSERT(app.isStopped());
}

// ============================================================
// Test 17: Startup Failure Cleanup
// ============================================================

void testStartupFailureCleanup() {
    LampApplication app(LampConfig{});

    auto first = addMock(app, "first");
    auto second = addMock(app, "second");
    auto third = addMock(app, "third");

    second->startFailuresRemaining.store(1);

    requireSuccess(app.initialize());

    auto result = app.start();

    LAMP_ASSERT(!result);
    LAMP_ASSERT(app.hasFailed());

    LAMP_ASSERT_EQ(first->startCalls.load(), 1);
    LAMP_ASSERT_EQ(second->startCalls.load(), 1);
    LAMP_ASSERT_EQ(third->startCalls.load(), 0);

    requireSuccess(app.shutdown());

    // All components completed initialization,
    // so all must be cleaned up.
    LAMP_ASSERT_EQ(first->stopCalls.load(), 1);
    LAMP_ASSERT_EQ(second->stopCalls.load(), 1);
    LAMP_ASSERT_EQ(third->stopCalls.load(), 1);
}

// ============================================================
// Test 18: Shutdown Failure Recovery
// ============================================================

void testShutdownFailureRecovery() {
    LampApplication app(LampConfig{});

    auto state = addMock(app, "retry");

    state->stopFailuresRemaining.store(1);

    initializeAndStart(app);

    auto firstAttempt = app.shutdown();

    LAMP_ASSERT(!firstAttempt);
    LAMP_ASSERT(app.hasFailed());
    LAMP_ASSERT_EQ(state->stopCalls.load(), 1);

    requireSuccess(app.shutdown());

    LAMP_ASSERT(app.isStopped());
    LAMP_ASSERT_EQ(state->stopCalls.load(), 2);
    LAMP_ASSERT(app.failure().has_value());
}

// ============================================================
// Test 19: Shutdown Exception Recovery
// ============================================================

void testShutdownExceptionRecovery() {
    LampApplication app(LampConfig{});

    auto state = addMock(app, "throwing-stop");

    state->throwOnStop.store(true);

    initializeAndStart(app);

    auto firstAttempt = app.shutdown();

    LAMP_ASSERT(!firstAttempt);
    LAMP_ASSERT(app.hasFailed());

    state->throwOnStop.store(false);

    requireSuccess(app.shutdown());

    LAMP_ASSERT_EQ(state->stopCalls.load(), 2);
    LAMP_ASSERT(app.isStopped());
}

// ============================================================
// Test 20: Selective Cleanup Retry
// ============================================================

void testSelectiveCleanupRetry() {
    LampApplication app(LampConfig{});

    auto first = addMock(app, "first");
    auto second = addMock(app, "second");
    auto third = addMock(app, "third");

    second->stopFailuresRemaining.store(1);

    initializeAndStart(app);

    auto result = app.shutdown();

    LAMP_ASSERT(!result);
    LAMP_ASSERT(app.hasFailed());

    // Cleanup continues even after one component fails.
    LAMP_ASSERT_EQ(first->stopCalls.load(), 1);
    LAMP_ASSERT_EQ(second->stopCalls.load(), 1);
    LAMP_ASSERT_EQ(third->stopCalls.load(), 1);

    requireSuccess(app.shutdown());

    // Only the failed component is retried.
    LAMP_ASSERT_EQ(first->stopCalls.load(), 1);
    LAMP_ASSERT_EQ(second->stopCalls.load(), 2);
    LAMP_ASSERT_EQ(third->stopCalls.load(), 1);

    requireSuccess(app.shutdown());

    // Completed components remain completed.
    LAMP_ASSERT_EQ(first->stopCalls.load(), 1);
    LAMP_ASSERT_EQ(second->stopCalls.load(), 2);
    LAMP_ASSERT_EQ(third->stopCalls.load(), 1);
}

// ============================================================
// Test 21: Concurrent Shutdown Serialization
// ============================================================

void testConcurrentShutdownSerialization() {
    LampApplication app(LampConfig{});

    auto state = addMock(app, "concurrent-stop");

    initializeAndStart(app);

    constexpr int threadCount = 8;

    std::atomic<int> successfulCalls{0};
    std::atomic<int> failedCalls{0};

    std::vector<std::thread> threads;
    threads.reserve(threadCount);

    for (int i = 0; i < threadCount; ++i) {
        threads.emplace_back([&]() {
            auto result = app.shutdown();

            if (result) {
                successfulCalls.fetch_add(1);
            } else {
                failedCalls.fetch_add(1);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    LAMP_ASSERT(app.isStopped());
    LAMP_ASSERT_EQ(state->stopCalls.load(), 1);
    LAMP_ASSERT_EQ(state->maximumActiveStops.load(), 1);
    LAMP_ASSERT_EQ(successfulCalls.load(), threadCount);
    LAMP_ASSERT_EQ(failedCalls.load(), 0);
}

// ============================================================
// Test 22: Blocked Shutdown Coordination
// ============================================================

void testBlockedShutdownCoordination() {
    LampApplication app(LampConfig{});

    auto state = addMock(app, "blocking-stop");

    initializeAndStart(app);

    {
        std::lock_guard<std::mutex> lock(
            state->gateMutex
        );
        state->releaseStop = false;
    }

    std::atomic<bool> shutdownSucceeded{false};

    std::thread shutdownThread([&]() {
        auto result = app.shutdown();
        shutdownSucceeded.store(
            static_cast<bool>(result)
        );
    });

    // Bound the wait so a broken shutdown cannot spin forever.
    const auto deadline =
        std::chrono::steady_clock::now() + 5s;

    while (!state->stopEntered.load(
               std::memory_order_acquire
           ) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }

    const bool stopWasEntered = state->stopEntered.load(
        std::memory_order_acquire
    );
    const bool observedStopping = app.isStopping();

    {
        std::lock_guard<std::mutex> lock(
            state->gateMutex
        );
        state->releaseStop = true;
    }

    state->gateCondition.notify_all();
    shutdownThread.join();

    LAMP_ASSERT(stopWasEntered);
    LAMP_ASSERT(observedStopping);
    LAMP_ASSERT(shutdownSucceeded.load());
    LAMP_ASSERT(app.isStopped());
    LAMP_ASSERT_EQ(state->maximumActiveStops.load(), 1);
}

// ============================================================
// Test 23: Concurrent Failure Recovery
// ============================================================

void testConcurrentFailureRecovery() {
    LampApplication app(LampConfig{});

    auto state = addMock(app, "recovery-stop");

    state->stopFailuresRemaining.store(1);

    initializeAndStart(app);

    auto initialAttempt = app.shutdown();

    LAMP_ASSERT(!initialAttempt);
    LAMP_ASSERT(app.hasFailed());

    constexpr int threadCount = 8;

    std::vector<std::thread> threads;
    threads.reserve(threadCount);

    std::atomic<int> successfulCalls{0};

    for (int i = 0; i < threadCount; ++i) {
        threads.emplace_back([&]() {
            auto result = app.shutdown();

            if (result) {
                successfulCalls.fetch_add(1);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    LAMP_ASSERT(app.isStopped());
    LAMP_ASSERT_EQ(state->stopCalls.load(), 2);
    LAMP_ASSERT_EQ(state->maximumActiveStops.load(), 1);
    LAMP_ASSERT_EQ(successfulCalls.load(), threadCount);
}

// ============================================================
// Test 24: Callback Exception Isolation
// ============================================================

void testCallbackExceptionIsolation() {
    std::atomic<int> successfulCallbacks{0};
    LampApplication app(LampConfig{});

    requireSuccess(
        app.onEvent(
            [](const ApplicationEventInfo&) {
                throw std::runtime_error(
                    "Listener failure"
                );
            }
        )
    );

    requireSuccess(
        app.onEvent(
            [&](const ApplicationEventInfo&) {
                successfulCallbacks.fetch_add(1);
            }
        )
    );

    initializeAndStart(app);
    requireSuccess(app.shutdown());

    LAMP_ASSERT_EQ(successfulCallbacks.load(), 4);
}

// ============================================================
// Test 25: Initialization Exception Cleanup
// ============================================================

void testInitializationExceptionCleanup() {
    LampApplication app(LampConfig{});

    auto state = addMock(app, "throwing-init");

    state->throwOnInitialize.store(true);

    auto result = app.initialize();

    LAMP_ASSERT(!result);
    LAMP_ASSERT(app.hasFailed());

    requireSuccess(app.shutdown());

    // Even throwing initialize() calls require cleanup.
    LAMP_ASSERT_EQ(state->initializeCalls.load(), 1);
    LAMP_ASSERT_EQ(state->stopCalls.load(), 1);
    LAMP_ASSERT(app.isStopped());
}

// ============================================================
// Test 26: Startup Exception Cleanup
// ============================================================

void testStartupExceptionCleanup() {
    LampApplication app(LampConfig{});

    auto state = addMock(app, "throwing-start");

    state->throwOnStart.store(true);

    requireSuccess(app.initialize());

    auto result = app.start();

    LAMP_ASSERT(!result);
    LAMP_ASSERT(app.hasFailed());

    requireSuccess(app.shutdown());

    LAMP_ASSERT_EQ(state->stopCalls.load(), 1);
    LAMP_ASSERT(app.isStopped());
}

// ============================================================
// Test 27: Destructor Cleanup
// ============================================================

void testDestructorCleanup() {
    auto state = std::make_shared<MockState>();

    {
        LampApplication app(LampConfig{});

        requireSuccess(
            app.registerComponent(
                std::make_unique<MockRuntimeComponent>(
                    "destructor-stop",
                    state
                )
            )
        );

        initializeAndStart(app);
    }

    LAMP_ASSERT_EQ(state->stopCalls.load(), 1);
}

// ============================================================
// Test 28: Failure Details Preservation
// ============================================================

void testFailureDetailsPreservation() {
    LampApplication app(LampConfig{});

    auto state = addMock(app, "failure-details");

    state->startFailuresRemaining.store(1);

    requireSuccess(app.initialize());

    auto result = app.start();

    LAMP_ASSERT(!result);
    LAMP_ASSERT(app.hasFailed());

    const auto beforeShutdown = app.failure();

    LAMP_ASSERT(beforeShutdown.has_value());

    requireSuccess(app.shutdown());

    const auto afterShutdown = app.failure();

    LAMP_ASSERT(afterShutdown.has_value());

    LAMP_ASSERT_EQ(
        beforeShutdown->message(),
        afterShutdown->message()
    );
}

// ============================================================
// Test 29: Partial Initialization Retry
// ============================================================

void testPartialInitializationRetry() {
    LampApplication app(LampConfig{});

    auto first = addMock(app, "first");
    auto second = addMock(app, "second");
    auto third = addMock(app, "third");

    second->initializeFailuresRemaining.store(1);
    second->stopFailuresRemaining.store(1);

    auto initResult = app.initialize();

    LAMP_ASSERT(!initResult);
    LAMP_ASSERT(app.hasFailed());

    auto firstShutdown = app.shutdown();

    LAMP_ASSERT(!firstShutdown);
    LAMP_ASSERT(app.hasFailed());

    // The first initialized successfully.
    // The second failed during initialization.
    // The third was never initialized.
    LAMP_ASSERT_EQ(first->stopCalls.load(), 1);
    LAMP_ASSERT_EQ(second->stopCalls.load(), 1);
    LAMP_ASSERT_EQ(third->stopCalls.load(), 0);

    requireSuccess(app.shutdown());

    // Only the partially initialized component
    // whose cleanup failed should be retried.
    LAMP_ASSERT_EQ(first->stopCalls.load(), 1);
    LAMP_ASSERT_EQ(second->stopCalls.load(), 2);
    LAMP_ASSERT_EQ(third->stopCalls.load(), 0);

    LAMP_ASSERT(app.isStopped());
}

// ============================================================
// Test 30: Cleanup State Name
// ============================================================

void testCleanupStateNames() {
    LAMP_ASSERT_EQ(
        componentCleanupStateName(
            ComponentCleanupState::NotRequired
        ),
        std::string_view("NOT_REQUIRED")
    );

    LAMP_ASSERT_EQ(
        componentCleanupStateName(
            ComponentCleanupState::Required
        ),
        std::string_view("REQUIRED")
    );

    LAMP_ASSERT_EQ(
        componentCleanupStateName(
            ComponentCleanupState::Completed
        ),
        std::string_view("COMPLETED")
    );
}

// ============================================================
// Test 31: No Cleanup Before Initialization
// ============================================================

void testNoCleanupBeforeInitialization() {
    LampApplication app(LampConfig{});

    auto state = addMock(app, "never-initialized");

    requireSuccess(app.shutdown());

    LAMP_ASSERT(app.isStopped());
    LAMP_ASSERT_EQ(state->initializeCalls.load(), 0);
    LAMP_ASSERT_EQ(state->startCalls.load(), 0);
    LAMP_ASSERT_EQ(state->stopCalls.load(), 0);
}

// ============================================================
// Test 32: Destructor Partial Initialization Cleanup
// ============================================================

void testDestructorPartialInitializationCleanup() {
    auto state = std::make_shared<MockState>();

    state->initializeFailuresRemaining.store(1);

    {
        LampApplication app(LampConfig{});

        requireSuccess(
            app.registerComponent(
                std::make_unique<MockRuntimeComponent>(
                    "partial-destructor",
                    state
                )
            )
        );

        auto result = app.initialize();

        LAMP_ASSERT(!result);
        LAMP_ASSERT(app.hasFailed());
    }

    // Destructor must clean up the component whose
    // initialization was attempted but failed.
    LAMP_ASSERT_EQ(state->initializeCalls.load(), 1);
    LAMP_ASSERT_EQ(state->stopCalls.load(), 1);
}

// ============================================================
// Test 33: Stopping Event State And Timing
// ============================================================

void testStoppingEventStateAndTiming() {
    std::vector<ApplicationEventInfo> events;
    LampApplication app(LampConfig{});
    auto state = addMock(app, "event-timing");

    std::atomic<bool> stopCalledAtStoppingEvent{false};

    requireSuccess(app.onEvent(
        [&](const ApplicationEventInfo& info) {
            events.push_back(info);
            if (info.event == ApplicationEvent::Stopping) {
                stopCalledAtStoppingEvent.store(
                    state->stopCalls.load() != 0
                );
            }
        }
    ));

    initializeAndStart(app);
    requireSuccess(app.shutdown());

    LAMP_ASSERT_EQ(events.size(), 4U);
    LAMP_ASSERT_EQ(events.at(0).event, ApplicationEvent::Initialized);
    LAMP_ASSERT_EQ(events.at(0).state, LifecycleState::Initialized);
    LAMP_ASSERT_EQ(events.at(1).event, ApplicationEvent::Started);
    LAMP_ASSERT_EQ(events.at(1).state, LifecycleState::Running);
    LAMP_ASSERT_EQ(events.at(2).event, ApplicationEvent::Stopping);
    LAMP_ASSERT_EQ(events.at(2).state, LifecycleState::Stopping);
    LAMP_ASSERT(!stopCalledAtStoppingEvent.load());
    LAMP_ASSERT_EQ(events.at(3).event, ApplicationEvent::Stopped);
    LAMP_ASSERT_EQ(events.at(3).state, LifecycleState::Stopped);
    LAMP_ASSERT_EQ(state->stopCalls.load(), 1);
}

// ============================================================
// Test 34: Concurrent Shutdown Event Ordering
// ============================================================

void testConcurrentShutdownEventOrdering() {
    std::vector<ApplicationEvent> events;
    LampApplication app(LampConfig{});
    auto state = addMock(app, "event-order");

    // File 23 serializes callbacks with operationMutex_.
    // The vector is written only during callback dispatch.
    requireSuccess(app.onEvent(
        [&](const ApplicationEventInfo& info) {
            events.push_back(info.event);
        }
    ));

    initializeAndStart(app);

    constexpr int threadCount = 8;
    std::vector<std::thread> threads;
    threads.reserve(threadCount);
    std::atomic<int> failures{0};

    for (int i = 0; i < threadCount; ++i) {
        threads.emplace_back([&]() {
            if (!app.shutdown()) {
                failures.fetch_add(1);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    LAMP_ASSERT_EQ(failures.load(), 0);
    LAMP_ASSERT_EQ(state->stopCalls.load(), 1);
    LAMP_ASSERT_EQ(events.size(), 4U);
    LAMP_ASSERT_EQ(events.at(0), ApplicationEvent::Initialized);
    LAMP_ASSERT_EQ(events.at(1), ApplicationEvent::Started);
    LAMP_ASSERT_EQ(events.at(2), ApplicationEvent::Stopping);
    LAMP_ASSERT_EQ(events.at(3), ApplicationEvent::Stopped);
}

// ============================================================
// Test Runner
// ============================================================

struct TestCase {
    std::string_view name;
    std::function<void()> execute;
};

int runTests() {
    const std::vector<TestCase> tests{
        {"Construction", testConstruction},
        {"Application factory", testApplicationFactory},
        {"Null component", testNullComponent},
        {"Component registration", testComponentRegistration},
        {"Initialization", testInitialization},
        {"Startup", testStartup},
        {"Normal shutdown", testNormalShutdown},
        {"Repeated shutdown", testRepeatedShutdown},
        {"Initialization order", testInitializationOrder},
        {"Startup order", testStartupOrder},
        {"Reverse shutdown order", testReverseShutdownOrder},
        {"Invalid startup", testInvalidStartup},
        {"Late registration", testLateRegistration},
        {"Lifecycle events", testLifecycleEvents},
        {"Initialization failure cleanup",
         testInitializationFailureCleanup},
        {"Partial initialization cleanup",
         testPartialInitializationCleanup},
        {"Startup failure cleanup",
         testStartupFailureCleanup},
        {"Shutdown failure recovery",
         testShutdownFailureRecovery},
        {"Shutdown exception recovery",
         testShutdownExceptionRecovery},
        {"Selective cleanup retry",
         testSelectiveCleanupRetry},
        {"Concurrent shutdown serialization",
         testConcurrentShutdownSerialization},
        {"Blocked shutdown coordination",
         testBlockedShutdownCoordination},
        {"Concurrent failure recovery",
         testConcurrentFailureRecovery},
        {"Callback exception isolation",
         testCallbackExceptionIsolation},
        {"Initialization exception cleanup",
         testInitializationExceptionCleanup},
        {"Startup exception cleanup",
         testStartupExceptionCleanup},
        {"Destructor cleanup", testDestructorCleanup},
        {"Failure details preservation",
         testFailureDetailsPreservation},
        {"Partial initialization retry",
         testPartialInitializationRetry},
        {"Cleanup state names",
         testCleanupStateNames},
        {"No cleanup before initialization",
         testNoCleanupBeforeInitialization},
        {"Destructor partial initialization cleanup",
         testDestructorPartialInitializationCleanup},
        {"Stopping event state and timing",
         testStoppingEventStateAndTiming},
        {"Concurrent shutdown event ordering",
         testConcurrentShutdownEventOrdering},
        {"Origin allowance policy modes",
         []() {
             // Default: DenyAll (allowOrigin: false)
             LampApplication defaultApp(LampConfig{});
             LAMP_ASSERT(!defaultApp.isOriginAllowed("https://example.com"));
             auto denied = defaultApp.evaluateOrigin(std::string_view("https://example.com"));
             LAMP_ASSERT(!denied.allowed);
             LAMP_ASSERT_EQ(denied.errorCode, std::string("CORS_ORIGIN_DENIED"));

             // Requests without Origin header are allowed by default
             auto noOrigin = defaultApp.evaluateOrigin(std::nullopt);
             LAMP_ASSERT(noOrigin.allowed);

             // AllowAll (allowOrigin: true)
             LampConfig allowAllCfg;
             allowAllCfg.originAllowance.mode = OriginAllowanceMode::AllowAll;
             LampApplication allowAllApp(allowAllCfg);
             LAMP_ASSERT(allowAllApp.isOriginAllowed("https://any.example.org"));
             auto allEval = allowAllApp.evaluateOrigin(std::string_view("https://any.example.org"));
             LAMP_ASSERT(allEval.allowed);
             LAMP_ASSERT_EQ(allEval.accessControlAllowOrigin, std::string("*"));

             // AllowList with normalization
             LampConfig allowListCfg;
             allowListCfg.originAllowance.mode = OriginAllowanceMode::AllowList;
             allowListCfg.originAllowance.allowedOrigins = {
                 "https://App.Example.com:8443/",
                 "http://localhost:3000"
             };
             LampApplication allowListApp(allowListCfg);
             LAMP_ASSERT(allowListApp.isOriginAllowed("https://app.example.com:8443"));
             LAMP_ASSERT(allowListApp.isOriginAllowed("http://localhost:3000/"));
             LAMP_ASSERT(!allowListApp.isOriginAllowed("https://evil.example.com"));
             LAMP_ASSERT(!allowListApp.isOriginAllowed("null"));
         }}
    };

    std::size_t passed = 0;
    std::size_t failed = 0;

    std::cout
        << "\n====================================\n"
        << " Lamp Application Tests\n"
        << "====================================\n\n";

    for (const auto& test : tests) {
        try {
            test.execute();

            ++passed;
            std::cout
                << "[PASS] " << test.name << '\n';
        } catch (const std::exception& exception) {
            ++failed;
            std::cout
                << "[FAIL] " << test.name
                << "\n       "
                << exception.what() << '\n';
        } catch (...) {
            ++failed;
            std::cout
                << "[FAIL] " << test.name
                << "\n       Unknown exception\n";
        }

        std::cout.flush();
    }

    std::cout
        << "\n====================================\n"
        << " Passed: " << passed << '\n'
        << " Failed: " << failed << '\n'
        << " Total : " << tests.size() << '\n'
        << "====================================\n";

    return failed == 0
        ? EXIT_SUCCESS
        : EXIT_FAILURE;
}

} // anonymous namespace

int main() {
    return runTests();
}
