
#include "core/runtime_context.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <future>
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

// ============================================================
// Synchronization Helpers
// ============================================================

class Gate final {
public:
    void wait() {
        std::unique_lock<std::mutex> lock(mutex_);

        condition_.wait(lock, [this]() {
            return open_;
        });
    }

    void open() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            open_ = true;
        }

        condition_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool open_ = false;
};

template <typename Predicate>
void waitUntil(
    Predicate predicate,
    std::chrono::milliseconds timeout = 3000ms
) {
    const auto deadline =
        std::chrono::steady_clock::now() + timeout;

    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            throw TestFailure(
                "Timed out waiting for condition"
            );
        }

        std::this_thread::sleep_for(1ms);
    }
}

// ============================================================
// Test 1: Initial State
// ============================================================

void testInitialState() {
    RuntimeContext context;

    LAMP_ASSERT_EQ(
        context.state(),
        RuntimeContextState::Created
    );

    LAMP_ASSERT(!context.isRunning());
    LAMP_ASSERT(!context.isStopping());
    LAMP_ASSERT(!context.isStopped());
    LAMP_ASSERT(!context.hasFailed());
    LAMP_ASSERT(!context.isAcceptingTasks());

    LAMP_ASSERT_EQ(context.workerCount(), 0U);
}

// ============================================================
// Test 2: Startup
// ============================================================

void testStartup() {
    RuntimeContext context({
        .workerCount = 2
    });

    context.start();

    LAMP_ASSERT(context.isRunning());
    LAMP_ASSERT(context.isAcceptingTasks());

    waitUntil([&]() {
        return context.workerCount() == 2;
    });

    context.shutdown();

    LAMP_ASSERT(context.isStopped());
}

// ============================================================
// Test 3: Default Worker Configuration
// ============================================================

void testDefaultWorkerConfiguration() {
    RuntimeContext context;

    LAMP_ASSERT_EQ(
        context.config().workerCount,
        0U
    );

    context.start();

    LAMP_ASSERT(context.isRunning());

    auto future = context.submit([]() {
        return 42;
    });

    LAMP_ASSERT_EQ(future.get(), 42);

    context.shutdown();
}

// ============================================================
// Test 4: Typed Task Submission
// ============================================================

void testTypedTaskSubmission() {
    RuntimeContext context({
        .workerCount = 2
    });

    context.start();

    auto result = context.submit(
        [](int a, int b) {
            return a + b;
        },
        20,
        30
    );

    LAMP_ASSERT_EQ(result.get(), 50);

    context.shutdown();
}

// ============================================================
// Test 5: Void Task Submission
// ============================================================

void testVoidTaskSubmission() {
    RuntimeContext context({
        .workerCount = 2
    });

    context.start();

    std::atomic<int> value{0};

    auto future = context.submit([&]() {
        value.store(100);
    });

    future.get();

    LAMP_ASSERT_EQ(value.load(), 100);

    context.shutdown();
}

// ============================================================
// Test 6: Move-Only Task Submission
// ============================================================

void testMoveOnlyTaskSubmission() {
    RuntimeContext context({
        .workerCount = 2
    });

    context.start();

    auto data = std::make_unique<int>(77);

    auto result = context.submit(
        [value = std::move(data)]() {
            return *value;
        }
    );

    LAMP_ASSERT_EQ(result.get(), 77);

    context.shutdown();
}

// ============================================================
// Test 7: Task Exception Propagation
// ============================================================

void testTaskExceptionPropagation() {
    RuntimeContext context({
        .workerCount = 2
    });

    context.start();

    auto future = context.submit([]() -> int {
        throw std::runtime_error(
            "Expected task failure"
        );
    });

    bool caught = false;

    try {
        (void)future.get();
    } catch (const std::runtime_error& error) {
        caught =
            std::string_view(error.what()) ==
            "Expected task failure";
    }

    LAMP_ASSERT(caught);
    LAMP_ASSERT(context.isRunning());

    context.shutdown();
}

// ============================================================
// Test 8: Submission Before Startup Rejected
// ============================================================

void testSubmissionBeforeStartupRejected() {
    RuntimeContext context;

    bool rejected = false;

    try {
        auto future = context.submit([]() {
            return 1;
        });

        (void)future;
    } catch (const RuntimeContextError&) {
        rejected = true;
    }

    LAMP_ASSERT(rejected);
}

// ============================================================
// Test 9: Worker Access Before Startup Rejected
// ============================================================

void testWorkerAccessBeforeStartupRejected() {
    RuntimeContext context;

    bool rejected = false;

    try {
        (void)context.workerPool();
    } catch (const RuntimeContextError&) {
        rejected = true;
    }

    LAMP_ASSERT(rejected);
}

// ============================================================
// Test 10: Scheduler Access Before Startup Rejected
// ============================================================

void testSchedulerAccessBeforeStartupRejected() {
    RuntimeContext context;

    bool rejected = false;

    try {
        (void)context.taskScheduler();
    } catch (const RuntimeContextError&) {
        rejected = true;
    }

    LAMP_ASSERT(rejected);
}

// ============================================================
// Test 11: Worker Pool Access
// ============================================================

void testWorkerPoolAccess() {
    RuntimeContext context({
        .workerCount = 3
    });

    context.start();

    auto workers = context.workerPool();

    LAMP_ASSERT(workers != nullptr);

    auto result = workers->submit([]() {
        return 123;
    });

    LAMP_ASSERT_EQ(result.get(), 123);

    context.shutdown();
}

// ============================================================
// Test 12: Scheduler Access
// ============================================================

void testSchedulerAccess() {
    RuntimeContext context({
        .workerCount = 2
    });

    context.start();

    auto scheduler = context.taskScheduler();

    LAMP_ASSERT(scheduler != nullptr);

    context.shutdown();
}

// ============================================================
// Test 13: Started Event
// ============================================================

void testStartedEvent() {
    RuntimeContext context({
        .workerCount = 2
    });

    std::vector<RuntimeContextEvent> events;

    const bool subscribed =
        context.onEvent(
            [&](const RuntimeContextEvent& event) {
                events.push_back(event);
            }
        );

    LAMP_ASSERT(subscribed);

    context.start();

    LAMP_ASSERT_EQ(events.size(), 1U);

    LAMP_ASSERT_EQ(
        events[0].type,
        RuntimeContextEventType::Started
    );

    LAMP_ASSERT_EQ(
        events[0].state,
        RuntimeContextState::Running
    );

    context.shutdown();
}

// ============================================================
// Test 14: Lifecycle Event Ordering
// ============================================================

void testLifecycleEventOrdering() {
    RuntimeContext context({
        .workerCount = 2
    });

    std::vector<RuntimeContextEventType> events;

    LAMP_ASSERT(context.onEvent(
        [&](const RuntimeContextEvent& event) {
            events.push_back(event.type);
        }
    ));

    context.start();
    context.shutdown();

    LAMP_ASSERT_EQ(events.size(), 3U);

    LAMP_ASSERT_EQ(
        events[0],
        RuntimeContextEventType::Started
    );

    LAMP_ASSERT_EQ(
        events[1],
        RuntimeContextEventType::Stopping
    );

    LAMP_ASSERT_EQ(
        events[2],
        RuntimeContextEventType::Stopped
    );
}

// ============================================================
// Test 15: Invalid Event Subscription
// ============================================================

void testInvalidEventSubscription() {
    RuntimeContext context;

    RuntimeContext::EventCallback callback;

    LAMP_ASSERT(!context.onEvent(callback));
}

// ============================================================
// Test 16: Duplicate Startup Rejected
// ============================================================

void testDuplicateStartupRejected() {
    RuntimeContext context({
        .workerCount = 2
    });

    context.start();

    bool rejected = false;

    try {
        context.start();
    } catch (const RuntimeContextError&) {
        rejected = true;
    }

    LAMP_ASSERT(rejected);
    LAMP_ASSERT(context.isRunning());

    context.shutdown();
}

// ============================================================
// Test 17: Shutdown Before Startup
// ============================================================

void testShutdownBeforeStartup() {
    RuntimeContext context;

    std::vector<RuntimeContextEventType> events;

    LAMP_ASSERT(context.onEvent(
        [&](const RuntimeContextEvent& event) {
            events.push_back(event.type);
        }
    ));

    context.shutdown();

    LAMP_ASSERT(context.isStopped());

    LAMP_ASSERT_EQ(events.size(), 1U);

    LAMP_ASSERT_EQ(
        events[0],
        RuntimeContextEventType::Stopped
    );
}

// ============================================================
// Test 18: Repeated Shutdown
// ============================================================

void testRepeatedShutdown() {
    RuntimeContext context({
        .workerCount = 2
    });

    context.start();

    context.shutdown();
    context.shutdown();
    context.shutdown(false);

    LAMP_ASSERT(context.isStopped());
    LAMP_ASSERT_EQ(context.workerCount(), 0U);
}

// ============================================================
// Test 19: Submission After Shutdown Rejected
// ============================================================

void testSubmissionAfterShutdownRejected() {
    RuntimeContext context({
        .workerCount = 2
    });

    context.start();
    context.shutdown();

    bool rejected = false;

    try {
        auto future = context.submit([]() {
            return 1;
        });

        (void)future;
    } catch (const RuntimeContextError&) {
        rejected = true;
    }

    LAMP_ASSERT(rejected);
}

// ============================================================
// Test 20: Restart After Shutdown Rejected
// ============================================================

void testRestartAfterShutdownRejected() {
    RuntimeContext context({
        .workerCount = 2
    });

    context.start();
    context.shutdown();

    bool rejected = false;

    try {
        context.start();
    } catch (const RuntimeContextError&) {
        rejected = true;
    }

    LAMP_ASSERT(rejected);
    LAMP_ASSERT(context.isStopped());
}

// ============================================================
// Test 21: Graceful Shutdown Drains Tasks
// ============================================================

void testGracefulShutdownDrainsTasks() {
    RuntimeContext context({
        .workerCount = 2
    });

    context.start();

    constexpr int taskCount = 50;

    std::atomic<int> executed{0};

    std::vector<std::future<void>> futures;

    for (int i = 0; i < taskCount; ++i) {
        futures.push_back(
            context.submit([&]() {
                executed.fetch_add(1);
            })
        );
    }

    context.shutdown(true);

    LAMP_ASSERT_EQ(executed.load(), taskCount);

    for (auto& future : futures) {
        future.get();
    }

    LAMP_ASSERT(context.isStopped());
}

// ============================================================
// Test 22: Immediate Shutdown Cancels Pending
// ============================================================

void testImmediateShutdownCancelsPending() {
    RuntimeContext context({
        .workerCount = 1
    });

    context.start();

    Gate gate;

    std::atomic<bool> running{false};

    auto active = context.submit([&]() {
        running.store(true);
        gate.wait();
        return 1;
    });

    waitUntil([&]() {
        return running.load();
    });

    auto pending = context.submit([]() {
        return 2;
    });

    std::thread shutdownThread([&]() {
        context.shutdown(false);
    });

    waitUntil([&]() {
        return context.isStopping();
    });

    gate.open();
    shutdownThread.join();

    LAMP_ASSERT_EQ(active.get(), 1);

    bool cancelled = false;

    try {
        (void)pending.get();
    } catch (const std::future_error& error) {
        cancelled =
            error.code() ==
            std::make_error_code(
                std::future_errc::broken_promise
            );
    }

    LAMP_ASSERT(cancelled);
    LAMP_ASSERT(context.isStopped());
}

// ============================================================
// Test 23: Concurrent Task Submission
// ============================================================

void testConcurrentTaskSubmission() {
    RuntimeContext context({
        .workerCount = 4
    });

    context.start();

    constexpr int producerCount = 8;
    constexpr int tasksPerProducer = 40;

    std::atomic<int> executed{0};

    std::vector<std::thread> producers;

    for (int i = 0; i < producerCount; ++i) {
        producers.emplace_back([&]() {
            std::vector<std::future<void>> futures;

            for (int j = 0; j < tasksPerProducer; ++j) {
                futures.push_back(
                    context.submit([&]() {
                        executed.fetch_add(1);
                    })
                );
            }

            for (auto& future : futures) {
                future.get();
            }
        });
    }

    for (auto& producer : producers) {
        producer.join();
    }

    LAMP_ASSERT_EQ(
        executed.load(),
        producerCount * tasksPerProducer
    );

    context.shutdown();
}

// ============================================================
// Test 24: Concurrent Shutdown
// ============================================================

void testConcurrentShutdown() {
    RuntimeContext context({
        .workerCount = 4
    });

    context.start();

    constexpr int threadCount = 8;

    std::atomic<int> completed{0};

    std::vector<std::thread> threads;

    for (int i = 0; i < threadCount; ++i) {
        threads.emplace_back([&]() {
            context.shutdown();
            completed.fetch_add(1);
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    LAMP_ASSERT_EQ(completed.load(), threadCount);
    LAMP_ASSERT(context.isStopped());
}

// ============================================================
// Test 25: Shutdown Waits For Running Task
// ============================================================

void testShutdownWaitsForRunningTask() {
    RuntimeContext context({
        .workerCount = 1
    });

    context.start();

    Gate gate;

    std::atomic<bool> running{false};
    std::atomic<bool> shutdownCompleted{false};

    auto future = context.submit([&]() {
        running.store(true);
        gate.wait();
    });

    waitUntil([&]() {
        return running.load();
    });

    std::thread shutdownThread([&]() {
        context.shutdown();

        shutdownCompleted.store(true);
    });

    waitUntil([&]() {
        return context.isStopping();
    });

    LAMP_ASSERT(!shutdownCompleted.load());

    gate.open();

    shutdownThread.join();
    future.get();

    LAMP_ASSERT(shutdownCompleted.load());
    LAMP_ASSERT(context.isStopped());
}

// ============================================================
// Test 26: Destructor Shuts Down Services
// ============================================================

void testDestructorShutsDownServices() {
    std::atomic<int> executed{0};

    {
        RuntimeContext context({
            .workerCount = 2
        });

        context.start();

        for (int i = 0; i < 20; ++i) {
            (void)context.submit([&]() {
                executed.fetch_add(1);
            });
        }
    }

    LAMP_ASSERT_EQ(executed.load(), 20);
}

// ============================================================
// Test 27: Multiple Event Subscribers
// ============================================================

void testMultipleEventSubscribers() {
    RuntimeContext context({
        .workerCount = 2
    });

    std::atomic<int> first{0};
    std::atomic<int> second{0};

    LAMP_ASSERT(context.onEvent(
        [&](const RuntimeContextEvent&) {
            first.fetch_add(1);
        }
    ));

    LAMP_ASSERT(context.onEvent(
        [&](const RuntimeContextEvent&) {
            second.fetch_add(1);
        }
    ));

    context.start();
    context.shutdown();

    LAMP_ASSERT_EQ(first.load(), 3);
    LAMP_ASSERT_EQ(second.load(), 3);
}

// ============================================================
// Test 28: Event Subscriber Exception Isolation
// ============================================================

void testEventSubscriberExceptionIsolation() {
    RuntimeContext context({
        .workerCount = 2
    });

    std::atomic<int> received{0};

    LAMP_ASSERT(context.onEvent(
        [](const RuntimeContextEvent&) {
            throw std::runtime_error(
                "Subscriber failure"
            );
        }
    ));

    LAMP_ASSERT(context.onEvent(
        [&](const RuntimeContextEvent&) {
            received.fetch_add(1);
        }
    ));

    context.start();
    context.shutdown();

    LAMP_ASSERT_EQ(received.load(), 3);
    LAMP_ASSERT(context.isStopped());
}

// ============================================================
// Test Runner
// ============================================================

struct TestCase {
    std::string_view name;
    void (*execute)();
};

int runTests() {
    const std::vector<TestCase> tests{
        {"Initial state", testInitialState},
        {"Startup", testStartup},
        {"Default worker configuration",
         testDefaultWorkerConfiguration},
        {"Typed task submission",
         testTypedTaskSubmission},
        {"Void task submission",
         testVoidTaskSubmission},
        {"Move-only task submission",
         testMoveOnlyTaskSubmission},
        {"Task exception propagation",
         testTaskExceptionPropagation},
        {"Submission before startup rejected",
         testSubmissionBeforeStartupRejected},
        {"Worker access before startup rejected",
         testWorkerAccessBeforeStartupRejected},
        {"Scheduler access before startup rejected",
         testSchedulerAccessBeforeStartupRejected},
        {"Worker pool access", testWorkerPoolAccess},
        {"Scheduler access", testSchedulerAccess},
        {"Started event", testStartedEvent},
        {"Lifecycle event ordering",
         testLifecycleEventOrdering},
        {"Invalid event subscription",
         testInvalidEventSubscription},
        {"Duplicate startup rejected",
         testDuplicateStartupRejected},
        {"Shutdown before startup",
         testShutdownBeforeStartup},
        {"Repeated shutdown", testRepeatedShutdown},
        {"Submission after shutdown rejected",
         testSubmissionAfterShutdownRejected},
        {"Restart after shutdown rejected",
         testRestartAfterShutdownRejected},
        {"Graceful shutdown drains tasks",
         testGracefulShutdownDrainsTasks},
        {"Immediate shutdown cancels pending",
         testImmediateShutdownCancelsPending},
        {"Concurrent task submission",
         testConcurrentTaskSubmission},
        {"Concurrent shutdown",
         testConcurrentShutdown},
        {"Shutdown waits for running task",
         testShutdownWaitsForRunningTask},
        {"Destructor shuts down services",
         testDestructorShutsDownServices},
        {"Multiple event subscribers",
         testMultipleEventSubscribers},
        {"Event subscriber exception isolation",
         testEventSubscriberExceptionIsolation}
    };

    std::size_t passed = 0;
    std::size_t failed = 0;

    std::cout
        << "\n====================================\n"
        << " Lamp Runtime Context Tests\n"
        << "====================================\n\n";

    for (const auto& test : tests) {
        try {
            test.execute();

            ++passed;

            std::cout
                << "[PASS] " << test.name << '\n';

        } catch (const std::exception& error) {
            ++failed;

            std::cout
                << "[FAIL] " << test.name
                << "\n       "
                << error.what() << '\n';

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
