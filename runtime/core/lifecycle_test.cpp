
#include "core/lifecycle.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using lamp::core::ErrorCode;
using lamp::core::Lifecycle;
using lamp::core::LifecycleState;
using lamp::core::Result;
using lamp::core::canTransition;
using lamp::core::lifecycleStateName;
using lamp::core::makeError;

// ============================================================
// Minimal Test Framework
// ============================================================

class TestFailure final : public std::runtime_error {
public:
    explicit TestFailure(std::string message)
        : std::runtime_error(std::move(message)) {}
};

#define LAMP_ASSERT(condition)                           \
    do {                                                 \
        if (!(condition)) {                              \
            throw TestFailure(                           \
                std::string("Assertion failed: ") +      \
                #condition +                             \
                " at line " +                            \
                std::to_string(__LINE__)                 \
            );                                           \
        }                                                \
    } while (false)

#define LAMP_ASSERT_EQ(actual, expected)                 \
    do {                                                 \
        const auto& lamp_actual = (actual);               \
        const auto& lamp_expected = (expected);           \
        LAMP_ASSERT(lamp_actual == lamp_expected);        \
    } while (false)

// ============================================================
// Test Utilities
// ============================================================

void requireSuccess(Result<void> result) {
    if (!result) {
        throw TestFailure(
            "Unexpected lifecycle failure: " +
            result.error().message()
        );
    }
}

void moveToRunning(Lifecycle& lifecycle) {
    requireSuccess(
        lifecycle.beginInitialization()
    );

    requireSuccess(
        lifecycle.completeInitialization()
    );

    requireSuccess(
        lifecycle.beginStartup()
    );

    requireSuccess(
        lifecycle.completeStartup()
    );
}

// ============================================================
// Test 1: Initial State
// ============================================================

void testInitialState() {
    Lifecycle lifecycle;

    LAMP_ASSERT_EQ(
        lifecycle.state(),
        LifecycleState::Created
    );

    LAMP_ASSERT(!lifecycle.isRunning());
    LAMP_ASSERT(!lifecycle.isStopping());
    LAMP_ASSERT(!lifecycle.isStopped());
    LAMP_ASSERT(!lifecycle.hasFailed());
    LAMP_ASSERT(!lifecycle.isTerminal());
    LAMP_ASSERT(!lifecycle.isAcceptingRequests());
    LAMP_ASSERT(!lifecycle.hasFailureDetails());
}

// ============================================================
// Test 2: State Names
// ============================================================

void testStateNames() {
    LAMP_ASSERT_EQ(
        lifecycleStateName(LifecycleState::Created),
        std::string_view("CREATED")
    );

    LAMP_ASSERT_EQ(
        lifecycleStateName(LifecycleState::Initializing),
        std::string_view("INITIALIZING")
    );

    LAMP_ASSERT_EQ(
        lifecycleStateName(LifecycleState::Initialized),
        std::string_view("INITIALIZED")
    );

    LAMP_ASSERT_EQ(
        lifecycleStateName(LifecycleState::Starting),
        std::string_view("STARTING")
    );

    LAMP_ASSERT_EQ(
        lifecycleStateName(LifecycleState::Running),
        std::string_view("RUNNING")
    );

    LAMP_ASSERT_EQ(
        lifecycleStateName(LifecycleState::Stopping),
        std::string_view("STOPPING")
    );

    LAMP_ASSERT_EQ(
        lifecycleStateName(LifecycleState::Stopped),
        std::string_view("STOPPED")
    );

    LAMP_ASSERT_EQ(
        lifecycleStateName(LifecycleState::Failed),
        std::string_view("FAILED")
    );
}

// ============================================================
// Test 3: Transition Rules
// ============================================================

void testTransitionRules() {
    LAMP_ASSERT(canTransition(
        LifecycleState::Created,
        LifecycleState::Initializing
    ));

    LAMP_ASSERT(canTransition(
        LifecycleState::Initializing,
        LifecycleState::Initialized
    ));

    LAMP_ASSERT(canTransition(
        LifecycleState::Initialized,
        LifecycleState::Starting
    ));

    LAMP_ASSERT(canTransition(
        LifecycleState::Starting,
        LifecycleState::Running
    ));

    LAMP_ASSERT(canTransition(
        LifecycleState::Running,
        LifecycleState::Stopping
    ));

    LAMP_ASSERT(canTransition(
        LifecycleState::Stopping,
        LifecycleState::Stopped
    ));

    LAMP_ASSERT(canTransition(
        LifecycleState::Failed,
        LifecycleState::Stopping
    ));

    LAMP_ASSERT(!canTransition(
        LifecycleState::Created,
        LifecycleState::Running
    ));

    LAMP_ASSERT(!canTransition(
        LifecycleState::Stopped,
        LifecycleState::Running
    ));

    LAMP_ASSERT(!canTransition(
        LifecycleState::Running,
        LifecycleState::Running
    ));
}

// ============================================================
// Test 4: Initialization
// ============================================================

void testInitialization() {
    Lifecycle lifecycle;

    requireSuccess(
        lifecycle.beginInitialization()
    );

    LAMP_ASSERT_EQ(
        lifecycle.state(),
        LifecycleState::Initializing
    );

    requireSuccess(
        lifecycle.completeInitialization()
    );

    LAMP_ASSERT_EQ(
        lifecycle.state(),
        LifecycleState::Initialized
    );
}

// ============================================================
// Test 5: Startup
// ============================================================

void testStartup() {
    Lifecycle lifecycle;

    moveToRunning(lifecycle);

    LAMP_ASSERT(lifecycle.isRunning());
    LAMP_ASSERT(lifecycle.isAcceptingRequests());

    LAMP_ASSERT_EQ(
        lifecycle.state(),
        LifecycleState::Running
    );
}

// ============================================================
// Test 6: Normal Shutdown
// ============================================================

void testNormalShutdown() {
    Lifecycle lifecycle;

    moveToRunning(lifecycle);

    requireSuccess(
        lifecycle.beginShutdown()
    );

    LAMP_ASSERT(lifecycle.isStopping());
    LAMP_ASSERT(!lifecycle.isAcceptingRequests());

    requireSuccess(
        lifecycle.completeShutdown()
    );

    LAMP_ASSERT(lifecycle.isStopped());
    LAMP_ASSERT(lifecycle.isTerminal());
    LAMP_ASSERT(!lifecycle.isRunning());
}

// ============================================================
// Test 7: Shutdown Before Initialization
// ============================================================

void testShutdownBeforeInitialization() {
    Lifecycle lifecycle;

    requireSuccess(
        lifecycle.beginShutdown()
    );

    requireSuccess(
        lifecycle.completeShutdown()
    );

    LAMP_ASSERT(lifecycle.isStopped());
}

// ============================================================
// Test 8: Repeated Shutdown
// ============================================================

void testRepeatedShutdown() {
    Lifecycle lifecycle;

    moveToRunning(lifecycle);

    requireSuccess(
        lifecycle.beginShutdown()
    );

    requireSuccess(
        lifecycle.beginShutdown()
    );

    requireSuccess(
        lifecycle.completeShutdown()
    );

    requireSuccess(
        lifecycle.beginShutdown()
    );

    LAMP_ASSERT(lifecycle.isStopped());
}

// ============================================================
// Test 9: Invalid Transition
// ============================================================

void testInvalidTransition() {
    Lifecycle lifecycle;

    auto result = lifecycle.completeStartup();

    LAMP_ASSERT(result.hasError());

    LAMP_ASSERT_EQ(
        lifecycle.state(),
        LifecycleState::Created
    );

    LAMP_ASSERT_EQ(
        result.error().code(),
        ErrorCode::ExecutionFailed
    );
}

// ============================================================
// Test 10: Duplicate Initialization
// ============================================================

void testDuplicateInitialization() {
    Lifecycle lifecycle;

    requireSuccess(
        lifecycle.beginInitialization()
    );

    auto result = lifecycle.beginInitialization();

    LAMP_ASSERT(result.hasError());

    LAMP_ASSERT_EQ(
        lifecycle.state(),
        LifecycleState::Initializing
    );
}

// ============================================================
// Test 11: Failure Recording
// ============================================================

void testFailureRecording() {
    Lifecycle lifecycle;

    auto error = makeError(
        ErrorCode::ExecutionFailed,
        "Mock runtime failure."
    );

    requireSuccess(
        lifecycle.fail(std::move(error))
    );

    LAMP_ASSERT(lifecycle.hasFailed());
    LAMP_ASSERT(lifecycle.hasFailureDetails());

    auto failure = lifecycle.failure();

    LAMP_ASSERT(failure.has_value());

    LAMP_ASSERT_EQ(
        failure->code(),
        ErrorCode::ExecutionFailed
    );

    LAMP_ASSERT_EQ(
        failure->message(),
        std::string("Mock runtime failure.")
    );
}

// ============================================================
// Test 12: Preserve First Failure
// ============================================================

void testPreserveFirstFailure() {
    Lifecycle lifecycle;

    requireSuccess(
        lifecycle.fail(
            makeError(
                ErrorCode::ExecutionFailed,
                "First failure."
            )
        )
    );

    requireSuccess(
        lifecycle.fail(
            makeError(
                ErrorCode::InternalError,
                "Second failure."
            )
        )
    );

    auto failure = lifecycle.failure();

    LAMP_ASSERT(failure.has_value());

    LAMP_ASSERT_EQ(
        failure->message(),
        std::string("First failure.")
    );
}

// ============================================================
// Test 13: Failure Recovery Through Shutdown
// ============================================================

void testFailureRecovery() {
    Lifecycle lifecycle;

    moveToRunning(lifecycle);

    requireSuccess(
        lifecycle.fail(
            makeError(
                ErrorCode::ExecutionFailed,
                "Runtime execution failed."
            )
        )
    );

    LAMP_ASSERT(lifecycle.hasFailed());

    requireSuccess(
        lifecycle.beginShutdown()
    );

    LAMP_ASSERT(lifecycle.isStopping());

    requireSuccess(
        lifecycle.completeShutdown()
    );

    LAMP_ASSERT(lifecycle.isStopped());

    // Preserve diagnostics after cleanup.
    LAMP_ASSERT(lifecycle.hasFailureDetails());
}

// ============================================================
// Test 14: Failure After Stopping
// ============================================================

void testFailureDuringShutdown() {
    Lifecycle lifecycle;

    moveToRunning(lifecycle);

    requireSuccess(
        lifecycle.beginShutdown()
    );

    requireSuccess(
        lifecycle.fail(
            makeError(
                ErrorCode::ExecutionFailed,
                "Shutdown failure."
            )
        )
    );

    LAMP_ASSERT(lifecycle.hasFailed());

    // A later cleanup attempt can resume shutdown.
    requireSuccess(
        lifecycle.beginShutdown()
    );

    requireSuccess(
        lifecycle.completeShutdown()
    );

    LAMP_ASSERT(lifecycle.isStopped());
}

// ============================================================
// Test 15: No Restart After Shutdown
// ============================================================

void testNoRestartAfterShutdown() {
    Lifecycle lifecycle;

    moveToRunning(lifecycle);

    requireSuccess(
        lifecycle.beginShutdown()
    );

    requireSuccess(
        lifecycle.completeShutdown()
    );

    auto result = lifecycle.beginStartup();

    LAMP_ASSERT(result.hasError());
    LAMP_ASSERT(lifecycle.isStopped());
}

// ============================================================
// Test 16: Concurrent Shutdown Requests
// ============================================================

void testConcurrentShutdown() {
    Lifecycle lifecycle;

    moveToRunning(lifecycle);

    constexpr std::size_t threadCount = 8;

    std::atomic<std::size_t> successCount{0};
    std::atomic<std::size_t> failureCount{0};

    std::vector<std::thread> threads;
    threads.reserve(threadCount);

    for (std::size_t i = 0; i < threadCount; ++i) {
        threads.emplace_back([&]() {
            auto result = lifecycle.beginShutdown();

            if (result) {
                successCount.fetch_add(
                    1,
                    std::memory_order_relaxed
                );
            } else {
                failureCount.fetch_add(
                    1,
                    std::memory_order_relaxed
                );
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    LAMP_ASSERT_EQ(
        successCount.load(),
        threadCount
    );

    LAMP_ASSERT_EQ(
        failureCount.load(),
        std::size_t{0}
    );

    LAMP_ASSERT(lifecycle.isStopping());

    requireSuccess(
        lifecycle.completeShutdown()
    );
}

// ============================================================
// Test 17: Concurrent Transition Competition
// ============================================================

void testConcurrentTransitionCompetition() {
    Lifecycle lifecycle;

    constexpr std::size_t threadCount = 8;

    std::atomic<std::size_t> successCount{0};
    std::atomic<std::size_t> failureCount{0};

    std::vector<std::thread> threads;
    threads.reserve(threadCount);

    for (std::size_t i = 0; i < threadCount; ++i) {
        threads.emplace_back([&]() {
            auto result =
                lifecycle.beginInitialization();

            if (result) {
                successCount.fetch_add(
                    1,
                    std::memory_order_relaxed
                );
            } else {
                failureCount.fetch_add(
                    1,
                    std::memory_order_relaxed
                );
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    // Exactly one thread can move Created -> Initializing.
    LAMP_ASSERT_EQ(
        successCount.load(),
        std::size_t{1}
    );

    LAMP_ASSERT_EQ(
        failureCount.load(),
        threadCount - 1
    );

    LAMP_ASSERT_EQ(
        lifecycle.state(),
        LifecycleState::Initializing
    );
}

// ============================================================
// Test 18: Atomic State Notification
// ============================================================

void testStateNotification() {
    Lifecycle lifecycle;

    std::atomic<bool> waiting{false};
    std::atomic<bool> awakened{false};

    std::thread observer([&]() {
        const auto observed = lifecycle.state();

        waiting.store(
            true,
            std::memory_order_release
        );

        if (observed == LifecycleState::Created) {
            lifecycle.waitForStateChange(observed);
        }

        awakened.store(
            true,
            std::memory_order_release
        );
    });

    // Wait until the observer has captured its state.
    while (!waiting.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    requireSuccess(
        lifecycle.beginInitialization()
    );

    observer.join();

    LAMP_ASSERT(awakened.load());
    LAMP_ASSERT_EQ(
        lifecycle.state(),
        LifecycleState::Initializing
    );
}

// ============================================================
// Test 19: Concurrent Failure Recording
// ============================================================

void testConcurrentFailureRecording() {
    Lifecycle lifecycle;

    constexpr std::size_t threadCount = 8;

    std::atomic<std::size_t> successCount{0};

    std::vector<std::thread> threads;
    threads.reserve(threadCount);

    for (std::size_t i = 0; i < threadCount; ++i) {
        threads.emplace_back([&, i]() {
            auto result = lifecycle.fail(
                makeError(
                    ErrorCode::ExecutionFailed,
                    "Concurrent failure " +
                        std::to_string(i)
                )
            );

            if (result) {
                successCount.fetch_add(
                    1,
                    std::memory_order_relaxed
                );
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    LAMP_ASSERT_EQ(
        successCount.load(),
        threadCount
    );

    LAMP_ASSERT(lifecycle.hasFailed());

    auto failure = lifecycle.failure();

    LAMP_ASSERT(failure.has_value());

    LAMP_ASSERT_EQ(
        failure->code(),
        ErrorCode::ExecutionFailed
    );

    // The first thread to acquire the transition lock
    // determines the stored failure.
    LAMP_ASSERT(
        failure->message().starts_with(
            "Concurrent failure "
        )
    );
}

// ============================================================
// Test 20: Failure Details Visibility
// ============================================================

void testFailureDetailsVisibility() {
    Lifecycle lifecycle;

    std::atomic<bool> observedFailure{false};
    std::atomic<bool> missingDetails{false};

    std::thread observer([&]() {
        while (!lifecycle.hasFailed()) {
            std::this_thread::yield();
        }

        observedFailure.store(
            true,
            std::memory_order_release
        );

        if (!lifecycle.hasFailureDetails()) {
            missingDetails.store(
                true,
                std::memory_order_release
            );
        }
    });

    requireSuccess(
        lifecycle.fail(
            makeError(
                ErrorCode::InternalError,
                "Published failure."
            )
        )
    );

    observer.join();

    LAMP_ASSERT(observedFailure.load());
    LAMP_ASSERT(!missingDetails.load());

    auto failure = lifecycle.failure();

    LAMP_ASSERT(failure.has_value());

    LAMP_ASSERT_EQ(
        failure->message(),
        std::string("Published failure.")
    );
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
        {"Initial state", testInitialState},
        {"State names", testStateNames},
        {"Transition rules", testTransitionRules},
        {"Initialization", testInitialization},
        {"Startup", testStartup},
        {"Normal shutdown", testNormalShutdown},
        {"Shutdown before initialization",
         testShutdownBeforeInitialization},
        {"Repeated shutdown", testRepeatedShutdown},
        {"Invalid transition", testInvalidTransition},
        {"Duplicate initialization",
         testDuplicateInitialization},
        {"Failure recording", testFailureRecording},
        {"Preserve first failure",
         testPreserveFirstFailure},
        {"Failure recovery", testFailureRecovery},
        {"Failure during shutdown",
         testFailureDuringShutdown},
        {"No restart after shutdown",
         testNoRestartAfterShutdown},
        {"Concurrent shutdown",
         testConcurrentShutdown},
        {"Concurrent transition competition",
         testConcurrentTransitionCompetition},
        {"Atomic state notification",
         testStateNotification},
        {"Concurrent failure recording",
         testConcurrentFailureRecording},
        {"Failure details visibility",
         testFailureDetailsVisibility}
    };

    std::size_t passed = 0;
    std::size_t failed = 0;

    std::cout
        << "\n====================================\n"
        << " Lamp Lifecycle Tests\n"
        << "====================================\n\n";

    for (const auto& test : tests) {
        try {
            test.execute();

            ++passed;

            std::cout
                << "[PASS] "
                << test.name
                << '\n';
        } catch (const std::exception& exception) {
            ++failed;

            std::cout
                << "[FAIL] "
                << test.name
                << "\n       "
                << exception.what()
                << '\n';
        } catch (...) {
            ++failed;

            std::cout
                << "[FAIL] "
                << test.name
                << "\n       Unknown exception\n";
        }
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

// ============================================================
// Test Entry Point
// ============================================================

int main() {
    return runTests();
}
