
#include "core/runtime_manager.hpp"

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
#include <type_traits>
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
        const auto lamp_actual = (actual);                   \
        const auto lamp_expected = (expected);               \
        LAMP_ASSERT(lamp_actual == lamp_expected);           \
    } while (false)

template <typename Function>
void expectManagerError(Function&& function) {
    bool caught = false;

    try {
        std::forward<Function>(function)();
    } catch (const RuntimeManagerError&) {
        caught = true;
    }

    LAMP_ASSERT(caught);
}

template <typename Function>
void expectRegistryError(
    Function&& function,
    ServiceRegistryErrorCode expected
) {
    bool caught = false;

    try {
        std::forward<Function>(function)();
    } catch (const ServiceRegistryError& error) {
        caught = true;
        LAMP_ASSERT_EQ(error.code(), expected);
    }

    LAMP_ASSERT(caught);
}

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
// Test Services
// ============================================================

struct ConfigService {
    explicit ConfigService(int initial)
        : value(initial) {}

    int value;
};

struct CounterService {
    std::atomic<int> value{0};

    void increment() noexcept {
        value.fetch_add(1, std::memory_order_relaxed);
    }
};

struct ILogger {
    virtual ~ILogger() = default;
    virtual std::string name() const = 0;
};

struct ConsoleLogger final : ILogger {
    std::string name() const override {
        return "console";
    }
};

struct FileLogger final : ILogger {
    std::string name() const override {
        return "file";
    }
};

struct DestructionTracker {
    explicit DestructionTracker(
        std::atomic<int>& destroyed
    )
        : destroyed_(destroyed) {}

    ~DestructionTracker() {
        destroyed_.fetch_add(1);
    }

private:
    std::atomic<int>& destroyed_;
};

struct ShutdownObserver {
    RuntimeManager* manager = nullptr;
    std::atomic<bool>* contextStopped = nullptr;

    ~ShutdownObserver() {
        if (manager && contextStopped) {
            contextStopped->store(
                manager->context().isStopped()
            );
        }
    }
};

// ============================================================
// Test 1: Initial State
// ============================================================

void testInitialState() {
    RuntimeManager manager;

    LAMP_ASSERT_EQ(
        manager.state(),
        RuntimeManagerState::Created
    );

    LAMP_ASSERT(!manager.isRunning());
    LAMP_ASSERT(!manager.isStopping());
    LAMP_ASSERT(!manager.isStopped());
    LAMP_ASSERT(!manager.hasFailed());
    LAMP_ASSERT(!manager.isAcceptingRequests());
}

// ============================================================
// Test 2: Configuration
// ============================================================

void testConfiguration() {
    RuntimeManager manager({
        .runtime = {
            .workerCount = 3
        }
    });

    LAMP_ASSERT_EQ(
        manager.config().runtime.workerCount,
        3U
    );

    LAMP_ASSERT_EQ(
        manager.context().config().workerCount,
        3U
    );
}

// ============================================================
// Test 3: Application Access
// ============================================================

void testApplicationAccess() {
    RuntimeManager manager;

    LAMP_ASSERT(
        &manager.application() ==
        &manager.application()
    );
}

// ============================================================
// Test 4: Runtime Context Access
// ============================================================

void testRuntimeContextAccess() {
    RuntimeManager manager;

    LAMP_ASSERT(
        &manager.context() ==
        &manager.context()
    );
}

// ============================================================
// Test 5: Successful Startup
// ============================================================

void testSuccessfulStartup() {
    RuntimeManager manager({
        .runtime = {
            .workerCount = 2
        }
    });

    manager.start();

    LAMP_ASSERT(manager.isRunning());
    LAMP_ASSERT(manager.context().isRunning());

    const auto configuredWorkers =
        manager.config().runtime.workerCount;

    const auto actualWorkers =
        manager.context().workerCount();

    std::cout
        << "[DEBUG] Configured workers: "
        << configuredWorkers
        << '\n'
        << "[DEBUG] Actual workers: "
        << actualWorkers
        << '\n';

    LAMP_ASSERT_EQ(actualWorkers, 2U);

    manager.shutdown();

    LAMP_ASSERT(manager.isStopped());
}

// ============================================================
// Test 6: Native Task Execution
// ============================================================

void testNativeTaskExecution() {
    RuntimeManager manager;

    manager.start();

    auto result = manager.context().submit(
        [](int a, int b) {
            return a + b;
        },
        20,
        30
    );

    LAMP_ASSERT_EQ(result.get(), 50);

    manager.shutdown();
}

// ============================================================
// Test 7: Multiple Native Tasks
// ============================================================

void testMultipleNativeTasks() {
    RuntimeManager manager({
        .runtime = {
            .workerCount = 4
        }
    });

    manager.start();

    std::vector<std::future<int>> futures;

    for (int i = 0; i < 100; ++i) {
        futures.push_back(
            manager.context().submit([i]() {
                return i * 2;
            })
        );
    }

    for (int i = 0; i < 100; ++i) {
        LAMP_ASSERT_EQ(
            futures[static_cast<std::size_t>(i)].get(),
            i * 2
        );
    }

    manager.shutdown();
}

// ============================================================
// Test 8: Native Task Exception Propagation
// ============================================================

void testNativeTaskExceptionPropagation() {
    RuntimeManager manager;

    manager.start();

    auto future =
        manager.context().submit([]() -> int {
            throw std::runtime_error(
                "Expected task failure"
            );
        });

    bool caught = false;

    try {
        (void)future.get();
    } catch (const std::runtime_error&) {
        caught = true;
    }

    LAMP_ASSERT(caught);
    LAMP_ASSERT(manager.isRunning());

    manager.shutdown();
}

// ============================================================
// Test 9: Duplicate Startup Rejected
// ============================================================

void testDuplicateStartupRejected() {
    RuntimeManager manager;

    manager.start();

    expectManagerError([&]() {
        manager.start();
    });

    LAMP_ASSERT(manager.isRunning());

    manager.shutdown();
}

// ============================================================
// Test 10: Shutdown Before Startup
// ============================================================

void testShutdownBeforeStartup() {
    RuntimeManager manager;

    manager.shutdown();

    LAMP_ASSERT(manager.isStopped());
    LAMP_ASSERT(manager.services().isClosed());
    LAMP_ASSERT(manager.services().empty());
}

// ============================================================
// Test 11: Repeated Shutdown
// ============================================================

void testRepeatedShutdown() {
    RuntimeManager manager;

    manager.start();

    manager.shutdown();
    manager.shutdown();
    manager.shutdown(false);

    LAMP_ASSERT(manager.isStopped());
}

// ============================================================
// Test 12: Restart After Shutdown Rejected
// ============================================================

void testRestartAfterShutdownRejected() {
    RuntimeManager manager;

    manager.start();
    manager.shutdown();

    expectManagerError([&]() {
        manager.start();
    });
}

// ============================================================
// Test 13: Native Services Stop
// ============================================================

void testNativeServicesStop() {
    RuntimeManager manager;

    manager.start();

    LAMP_ASSERT(
        manager.context().isAcceptingTasks()
    );

    manager.shutdown();

    LAMP_ASSERT(manager.context().isStopped());
    LAMP_ASSERT(
        !manager.context().isAcceptingTasks()
    );
}

// ============================================================
// Test 14: Submission After Shutdown Rejected
// ============================================================

void testSubmissionAfterShutdownRejected() {
    RuntimeManager manager;

    manager.start();
    manager.shutdown();

    bool rejected = false;

    try {
        auto future = manager.context().submit([] {
            return 1;
        });

        (void)future;
    } catch (const RuntimeContextError&) {
        rejected = true;
    }

    LAMP_ASSERT(rejected);
}

// ============================================================
// Test 15: Graceful Shutdown Drains Tasks
// ============================================================

void testGracefulShutdownDrainsTasks() {
    RuntimeManager manager({
        .runtime = {
            .workerCount = 2
        }
    });

    manager.start();

    std::atomic<int> executed{0};
    std::vector<std::future<void>> futures;

    for (int i = 0; i < 50; ++i) {
        futures.push_back(
            manager.context().submit([&]() {
                executed.fetch_add(1);
            })
        );
    }

    manager.shutdown(true);

    for (auto& future : futures) {
        future.get();
    }

    LAMP_ASSERT_EQ(executed.load(), 50);
}

// ============================================================
// Test 16: Immediate Shutdown Cancels Pending
// ============================================================

void testImmediateShutdownCancelsPending() {
    RuntimeManager manager({
        .runtime = {
            .workerCount = 1
        }
    });

    manager.start();

    Gate gate;
    std::atomic<bool> running{false};

    auto active = manager.context().submit([&]() {
        running.store(true);
        gate.wait();
        return 1;
    });

    waitUntil([&]() {
        return running.load();
    });

    auto pending = manager.context().submit([] {
        return 2;
    });

    std::thread shutdownThread([&]() {
        manager.shutdown(false);
    });

    waitUntil([&]() {
        return manager.isStopping();
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
}

// ============================================================
// Test 17: Concurrent Shutdown
// ============================================================

void testConcurrentShutdown() {
    RuntimeManager manager;

    manager.start();

    constexpr int threadCount = 8;

    std::atomic<int> completed{0};
    std::vector<std::thread> threads;

    for (int i = 0; i < threadCount; ++i) {
        threads.emplace_back([&]() {
            manager.shutdown();
            completed.fetch_add(1);
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    LAMP_ASSERT_EQ(completed.load(), threadCount);
    LAMP_ASSERT(manager.isStopped());
}

// ============================================================
// Test 18: Shutdown Waits For Running Task
// ============================================================

void testShutdownWaitsForRunningTask() {
    RuntimeManager manager({
        .runtime = {
            .workerCount = 1
        }
    });

    manager.start();

    Gate gate;
    std::atomic<bool> running{false};
    std::atomic<bool> completed{false};

    auto future = manager.context().submit([&]() {
        running.store(true);
        gate.wait();
    });

    waitUntil([&]() {
        return running.load();
    });

    std::thread shutdownThread([&]() {
        manager.shutdown();
        completed.store(true);
    });

    waitUntil([&]() {
        return manager.isStopping();
    });

    LAMP_ASSERT(!completed.load());

    gate.open();
    shutdownThread.join();
    future.get();

    LAMP_ASSERT(completed.load());
}

// ============================================================
// Test 19: Concurrent Native Submissions
// ============================================================

void testConcurrentNativeSubmissions() {
    RuntimeManager manager({
        .runtime = {
            .workerCount = 4
        }
    });

    manager.start();

    constexpr int producers = 8;
    constexpr int tasksPerProducer = 40;

    std::atomic<int> executed{0};
    std::vector<std::thread> threads;

    for (int i = 0; i < producers; ++i) {
        threads.emplace_back([&]() {
            std::vector<std::future<void>> futures;

            for (int j = 0; j < tasksPerProducer; ++j) {
                futures.push_back(
                    manager.context().submit([&]() {
                        executed.fetch_add(1);
                    })
                );
            }

            for (auto& future : futures) {
                future.get();
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    LAMP_ASSERT_EQ(
        executed.load(),
        producers * tasksPerProducer
    );

    manager.shutdown();
}

// ============================================================
// Test 20: Destructor Shuts Down Runtime
// ============================================================

void testDestructorShutsDownRuntime() {
    std::atomic<int> executed{0};

    {
        RuntimeManager manager({
            .runtime = {
                .workerCount = 2
            }
        });

        manager.start();

        for (int i = 0; i < 20; ++i) {
            (void)manager.context().submit([&]() {
                executed.fetch_add(1);
            });
        }
    }

    LAMP_ASSERT_EQ(executed.load(), 20);
}

// ============================================================
// Test 21: Built-In Application Service
// ============================================================

void testBuiltInApplicationService() {
    RuntimeManager manager;

    LAMP_ASSERT(manager.hasService<LampApplication>());

    auto application =
        manager.resolve<LampApplication>();

    LAMP_ASSERT(
        application.get() == &manager.application()
    );
}

// ============================================================
// Test 22: Built-In Runtime Context Service
// ============================================================

void testBuiltInRuntimeContextService() {
    RuntimeManager manager;

    LAMP_ASSERT(manager.hasService<RuntimeContext>());

    auto context =
        manager.resolve<RuntimeContext>();

    LAMP_ASSERT(
        context.get() == &manager.context()
    );
}

// ============================================================
// Test 23: Register Custom Service
// ============================================================

void testRegisterCustomService() {
    RuntimeManager manager;

    auto service =
        std::make_shared<ConfigService>(42);

    manager.registerService(service);

    LAMP_ASSERT(manager.hasService<ConfigService>());

    LAMP_ASSERT(
        manager.resolve<ConfigService>() == service
    );
}

// ============================================================
// Test 24: Emplace Custom Service
// ============================================================

void testEmplaceCustomService() {
    RuntimeManager manager;

    auto service =
        manager.emplaceService<ConfigService>(77);

    LAMP_ASSERT_EQ(service->value, 77);

    LAMP_ASSERT(
        manager.resolve<ConfigService>() == service
    );
}

// ============================================================
// Test 25: Interface Registration
// ============================================================

void testInterfaceRegistration() {
    RuntimeManager manager;

    manager.registerAs<ILogger>(
        std::make_shared<ConsoleLogger>()
    );

    auto logger = manager.resolve<ILogger>();

    LAMP_ASSERT_EQ(
        logger->name(),
        std::string("console")
    );
}

// ============================================================
// Test 26: Replace Service Before Startup
// ============================================================

void testReplaceServiceBeforeStartup() {
    RuntimeManager manager;

    (void)manager.emplaceService<ConfigService>(10);

    auto replacement =
        std::make_shared<ConfigService>(20);

    manager.replaceService(replacement);

    LAMP_ASSERT(
        manager.resolve<ConfigService>() == replacement
    );
}

// ============================================================
// Test 27: Remove Service Before Startup
// ============================================================

void testRemoveServiceBeforeStartup() {
    RuntimeManager manager;

    (void)manager.emplaceService<ConfigService>(10);

    LAMP_ASSERT(manager.removeService<ConfigService>());
    LAMP_ASSERT(!manager.hasService<ConfigService>());
}

// ============================================================
// Test 28: Service Configuration Rejected After Startup
// ============================================================

void testServiceConfigurationRejectedAfterStartup() {
    RuntimeManager manager;

    manager.start();

    expectManagerError([&]() {
        manager.registerService(
            std::make_shared<ConfigService>(1)
        );
    });

    expectManagerError([&]() {
        (void)manager.emplaceService<ConfigService>(2);
    });

    manager.shutdown();
}

// ============================================================
// Test 29: Service Resolution During Runtime
// ============================================================

void testServiceResolutionDuringRuntime() {
    RuntimeManager manager;

    (void)manager.emplaceService<ConfigService>(99);

    manager.start();

    auto service =
        manager.resolve<ConfigService>();

    LAMP_ASSERT_EQ(service->value, 99);

    manager.shutdown();
}

// ============================================================
// Test 30: Const Service Resolution
// ============================================================

void testConstServiceResolution() {
    RuntimeManager manager;

    std::shared_ptr<const ConfigService> service =
        std::make_shared<const ConfigService>(123);

    manager.registerService(service);

    auto resolved =
        manager.resolve<const ConfigService>();

    LAMP_ASSERT(resolved == service);

    expectRegistryError(
        [&]() {
            (void)manager.resolve<ConfigService>();
        },
        ServiceRegistryErrorCode::ConstViolation
    );
}

// ============================================================
// Test 31: Registry Closes On Shutdown
// ============================================================

void testRegistryClosesOnShutdown() {
    RuntimeManager manager;

    manager.start();
    manager.shutdown();

    LAMP_ASSERT(manager.services().isClosed());

    expectRegistryError(
        [&]() {
            manager.services().registerService(
                std::make_shared<ConfigService>(1)
            );
        },
        ServiceRegistryErrorCode::RegistryClosed
    );
}

// ============================================================
// Test 32: Registry Clears On Shutdown
// ============================================================

void testRegistryClearsOnShutdown() {
    RuntimeManager manager;

    (void)manager.emplaceService<ConfigService>(10);

    manager.start();
    manager.shutdown();

    LAMP_ASSERT(manager.services().empty());
    LAMP_ASSERT(!manager.hasService<ConfigService>());
    LAMP_ASSERT(!manager.hasService<LampApplication>());
    LAMP_ASSERT(!manager.hasService<RuntimeContext>());
}

// ============================================================
// Test 33: Service Destruction During Shutdown
// ============================================================

void testServiceDestructionDuringShutdown() {
    std::atomic<int> destroyed{0};

    RuntimeManager manager;

    manager.registerService(
        std::make_shared<DestructionTracker>(destroyed)
    );

    manager.start();

    LAMP_ASSERT_EQ(destroyed.load(), 0);

    manager.shutdown();

    LAMP_ASSERT_EQ(destroyed.load(), 1);
}

// ============================================================
// Test 34: Services Released After Native Runtime Stops
// ============================================================

void testServicesReleasedAfterRuntimeStops() {
    RuntimeManager manager;

    std::atomic<bool> contextStopped{false};

    auto observer = std::make_shared<ShutdownObserver>();

    observer->manager = &manager;
    observer->contextStopped = &contextStopped;

    manager.registerService(observer);

    observer.reset();

    manager.start();
    manager.shutdown();

    LAMP_ASSERT(contextStopped.load());
}

// ============================================================
// Test 35: Concurrent Service Resolution
// ============================================================

void testConcurrentServiceResolution() {
    RuntimeManager manager;

    auto counter =
        manager.emplaceService<CounterService>();

    manager.start();

    constexpr int threadCount = 8;
    constexpr int iterations = 1000;

    std::vector<std::thread> threads;

    for (int i = 0; i < threadCount; ++i) {
        threads.emplace_back([&]() {
            for (int j = 0; j < iterations; ++j) {
                manager.resolve<CounterService>()
                    ->increment();
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    LAMP_ASSERT_EQ(
        counter->value.load(),
        threadCount * iterations
    );

    manager.shutdown();
}

// ============================================================
// Test 36: Service Handle Survives Registry Cleanup
// ============================================================

void testServiceHandleSurvivesRegistryCleanup() {
    RuntimeManager manager;

    auto retained =
        manager.emplaceService<ConfigService>(555);

    manager.start();
    manager.shutdown();

    LAMP_ASSERT(manager.services().empty());

    // A retained shared_ptr owns the service independently.
    LAMP_ASSERT_EQ(retained->value, 555);
}

// ============================================================
// Test Registration
// ============================================================

struct TestCase {
    std::string_view name;
    void (*execute)();
};

// ============================================================
// Test Runner
// ============================================================

int runTests() {
    const std::vector<TestCase> tests{
        {"Initial state", testInitialState},
        {"Configuration", testConfiguration},
        {"Application access", testApplicationAccess},
        {"Runtime context access", testRuntimeContextAccess},
        {"Successful startup", testSuccessfulStartup},
        {"Native task execution", testNativeTaskExecution},
        {"Multiple native tasks", testMultipleNativeTasks},
        {"Native task exception propagation",
         testNativeTaskExceptionPropagation},
        {"Duplicate startup rejected",
         testDuplicateStartupRejected},
        {"Shutdown before startup",
         testShutdownBeforeStartup},
        {"Repeated shutdown", testRepeatedShutdown},
        {"Restart after shutdown rejected",
         testRestartAfterShutdownRejected},
        {"Native services stop", testNativeServicesStop},
        {"Submission after shutdown rejected",
         testSubmissionAfterShutdownRejected},
        {"Graceful shutdown drains tasks",
         testGracefulShutdownDrainsTasks},
        {"Immediate shutdown cancels pending",
         testImmediateShutdownCancelsPending},
        {"Concurrent shutdown", testConcurrentShutdown},
        {"Shutdown waits for running task",
         testShutdownWaitsForRunningTask},
        {"Concurrent native submissions",
         testConcurrentNativeSubmissions},
        {"Destructor shuts down runtime",
         testDestructorShutsDownRuntime},
        {"Built-in application service",
         testBuiltInApplicationService},
        {"Built-in runtime context service",
         testBuiltInRuntimeContextService},
        {"Register custom service",
         testRegisterCustomService},
        {"Emplace custom service",
         testEmplaceCustomService},
        {"Interface registration",
         testInterfaceRegistration},
        {"Replace service before startup",
         testReplaceServiceBeforeStartup},
        {"Remove service before startup",
         testRemoveServiceBeforeStartup},
        {"Service configuration rejected after startup",
         testServiceConfigurationRejectedAfterStartup},
        {"Service resolution during runtime",
         testServiceResolutionDuringRuntime},
        {"Const service resolution",
         testConstServiceResolution},
        {"Registry closes on shutdown",
         testRegistryClosesOnShutdown},
        {"Registry clears on shutdown",
         testRegistryClearsOnShutdown},
        {"Service destruction during shutdown",
         testServiceDestructionDuringShutdown},
        {"Services released after native runtime stops",
         testServicesReleasedAfterRuntimeStops},
        {"Concurrent service resolution",
         testConcurrentServiceResolution},
        {"Service handle survives registry cleanup",
         testServiceHandleSurvivesRegistryCleanup}
    };

    std::size_t passed = 0;
    std::size_t failed = 0;

    std::cout
        << "\n========================================\n"
        << " Lamp Runtime Manager Tests\n"
        << "========================================\n\n";

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
                << "\n       " << error.what() << '\n';

        } catch (...) {
            ++failed;

            std::cout
                << "[FAIL] " << test.name
                << "\n       Unknown exception\n";
        }
    }

    std::cout
        << "\n========================================\n"
        << " Passed: " << passed << '\n'
        << " Failed: " << failed << '\n'
        << " Total : " << tests.size() << '\n'
        << "========================================\n";

    return failed == 0
        ? EXIT_SUCCESS
        : EXIT_FAILURE;
}

} // anonymous namespace

int main() {
    return runTests();
}
