
#include "core/worker_pool.hpp"

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
// Test 1: Construction
// ============================================================

void testConstruction() {
    WorkerPool pool(2);

    LAMP_ASSERT(pool.isAccepting());
    LAMP_ASSERT(!pool.isStopping());

    waitUntil([&]() {
        return pool.workerCount() == 2;
    });

    pool.shutdown();

    LAMP_ASSERT_EQ(pool.workerCount(), 0U);
}

// ============================================================
// Test 2: Zero Worker Count
// ============================================================

void testZeroWorkerCount() {
    WorkerPool pool(0);

    auto result = pool.submit([]() {
        return 42;
    });

    LAMP_ASSERT_EQ(result.get(), 42);

    pool.shutdown();
}

// ============================================================
// Test 3: Integer Task
// ============================================================

void testIntegerTask() {
    WorkerPool pool(2);

    auto result = pool.submit(
        [](int a, int b) {
            return a + b;
        },
        10,
        20
    );

    LAMP_ASSERT_EQ(result.get(), 30);

    pool.shutdown();
}

// ============================================================
// Test 4: Void Task
// ============================================================

void testVoidTask() {
    WorkerPool pool(2);

    std::atomic<int> value{0};

    auto result = pool.submit([&]() {
        value.store(100);
    });

    result.get();

    LAMP_ASSERT_EQ(value.load(), 100);

    pool.shutdown();
}

// ============================================================
// Test 5: Move-Only Task
// ============================================================

void testMoveOnlyTask() {
    WorkerPool pool(2);

    auto data = std::make_unique<int>(55);

    auto result = pool.submit(
        [value = std::move(data)]() {
            return *value;
        }
    );

    LAMP_ASSERT_EQ(result.get(), 55);

    pool.shutdown();
}

// ============================================================
// Test 6: Task Exception Propagation
// ============================================================

void testTaskExceptionPropagation() {
    WorkerPool pool(2);

    auto result = pool.submit([]() -> int {
        throw std::runtime_error(
            "Expected task exception"
        );
    });

    bool caught = false;

    try {
        (void)result.get();
    } catch (const std::runtime_error& error) {
        caught =
            std::string_view(error.what()) ==
            "Expected task exception";
    }

    LAMP_ASSERT(caught);

    pool.shutdown();
}

// ============================================================
// Test 7: Multiple Tasks
// ============================================================

void testMultipleTasks() {
    WorkerPool pool(4);

    constexpr int taskCount = 100;

    std::vector<std::future<int>> results;
    results.reserve(taskCount);

    for (int i = 0; i < taskCount; ++i) {
        results.push_back(
            pool.submit([i]() {
                return i * 2;
            })
        );
    }

    for (int i = 0; i < taskCount; ++i) {
        LAMP_ASSERT_EQ(
            results[static_cast<std::size_t>(i)].get(),
            i * 2
        );
    }

    pool.shutdown();
}

// ============================================================
// Test 8: Concurrent Submissions
// ============================================================

void testConcurrentSubmissions() {
    WorkerPool pool(4);

    constexpr int producerCount = 8;
    constexpr int tasksPerProducer = 50;

    std::atomic<int> executed{0};

    std::vector<std::thread> producers;
    producers.reserve(producerCount);

    for (int i = 0; i < producerCount; ++i) {
        producers.emplace_back([&]() {
            std::vector<std::future<void>> futures;

            for (int j = 0; j < tasksPerProducer; ++j) {
                futures.push_back(
                    pool.submit([&]() {
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

    pool.shutdown();
}

// ============================================================
// Test 9: Worker Thread Identification
// ============================================================

void testWorkerThreadIdentification() {
    WorkerPool pool(2);

    LAMP_ASSERT(!pool.isWorkerThread());

    auto result = pool.submit([&]() {
        return pool.isWorkerThread();
    });

    LAMP_ASSERT(result.get());

    pool.shutdown();
}

// ============================================================
// Test 10: Graceful Shutdown Drains Queue
// ============================================================

void testGracefulShutdownDrainsQueue() {
    WorkerPool pool(2);

    constexpr int taskCount = 50;

    std::atomic<int> executed{0};

    std::vector<std::future<void>> futures;

    for (int i = 0; i < taskCount; ++i) {
        futures.push_back(
            pool.submit([&]() {
                executed.fetch_add(1);
            })
        );
    }

    pool.shutdown(true);

    LAMP_ASSERT_EQ(executed.load(), taskCount);

    for (auto& future : futures) {
        future.get();
    }

    LAMP_ASSERT(pool.isStopping());
    LAMP_ASSERT(!pool.isAccepting());
    LAMP_ASSERT_EQ(pool.workerCount(), 0U);
}

// ============================================================
// Test 11: Immediate Shutdown Cancels Pending
// ============================================================

void testImmediateShutdownCancelsPending() {
    WorkerPool pool(1);

    Gate gate;

    std::atomic<bool> running{false};

    auto active = pool.submit([&]() {
        running.store(true);
        gate.wait();
        return 1;
    });

    waitUntil([&]() {
        return running.load();
    });

    auto pending = pool.submit([]() {
        return 2;
    });

    std::thread shutdownThread([&]() {
        pool.shutdown(false);
    });

    waitUntil([&]() {
        return pool.isStopping();
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
// Test 12: Submission After Shutdown
// ============================================================

void testSubmissionAfterShutdown() {
    WorkerPool pool(2);

    pool.shutdown();

    bool rejected = false;

    try {
        auto future = pool.submit([]() {
            return 10;
        });

        (void)future;
    } catch (const WorkerPoolError&) {
        rejected = true;
    }

    LAMP_ASSERT(rejected);
}

// ============================================================
// Test 13: Repeated Shutdown
// ============================================================

void testRepeatedShutdown() {
    WorkerPool pool(2);

    pool.shutdown();
    pool.shutdown();
    pool.shutdown(false);

    LAMP_ASSERT_EQ(pool.workerCount(), 0U);
    LAMP_ASSERT(!pool.isAccepting());
}

// ============================================================
// Test 14: Worker-Initiated Shutdown Rejected
// ============================================================

void testWorkerInitiatedShutdownRejected() {
    WorkerPool pool(2);

    auto result = pool.submit([&]() {
        try {
            pool.shutdown();
        } catch (const WorkerPoolError&) {
            return true;
        }

        return false;
    });

    LAMP_ASSERT(result.get());

    // The rejected attempt must not stop the pool.
    LAMP_ASSERT(pool.isAccepting());

    pool.shutdown();
}

// ============================================================
// Test 15: Concurrent Shutdown
// ============================================================

void testConcurrentShutdown() {
    WorkerPool pool(4);

    constexpr int threadCount = 8;

    std::atomic<int> completed{0};

    std::vector<std::thread> threads;
    threads.reserve(threadCount);

    for (int i = 0; i < threadCount; ++i) {
        threads.emplace_back([&]() {
            pool.shutdown(true);
            completed.fetch_add(1);
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    LAMP_ASSERT_EQ(completed.load(), threadCount);
    LAMP_ASSERT_EQ(pool.workerCount(), 0U);
}

// ============================================================
// Test 16: Pending Task Count
// ============================================================

void testPendingTaskCount() {
    WorkerPool pool(1);

    Gate gate;
    std::atomic<bool> running{false};

    auto active = pool.submit([&]() {
        running.store(true);
        gate.wait();
    });

    waitUntil([&]() {
        return running.load();
    });

    auto first = pool.submit([]() {
        return 1;
    });

    auto second = pool.submit([]() {
        return 2;
    });

    LAMP_ASSERT_EQ(pool.pendingTasks(), 2U);

    gate.open();

    active.get();
    LAMP_ASSERT_EQ(first.get(), 1);
    LAMP_ASSERT_EQ(second.get(), 2);

    pool.shutdown();
}

// ============================================================
// Test 17: Task FIFO Ordering
// ============================================================

void testTaskFifoOrdering() {
    WorkerPool pool(1);

    constexpr int taskCount = 25;

    std::vector<int> order;
    std::vector<std::future<void>> futures;

    for (int i = 0; i < taskCount; ++i) {
        futures.push_back(
            pool.submit([&order, i]() {
                order.push_back(i);
            })
        );
    }

    for (auto& future : futures) {
        future.get();
    }

    LAMP_ASSERT_EQ(
        order.size(),
        static_cast<std::size_t>(taskCount)
    );

    for (int i = 0; i < taskCount; ++i) {
        LAMP_ASSERT_EQ(
            order[static_cast<std::size_t>(i)],
            i
        );
    }

    pool.shutdown();
}

// ============================================================
// Test 18: Destructor Drains Tasks
// ============================================================

void testDestructorDrainsTasks() {
    std::atomic<int> executed{0};

    {
        WorkerPool pool(2);

        for (int i = 0; i < 20; ++i) {
            (void)pool.submit([&]() {
                executed.fetch_add(1);
            });
        }
    }

    LAMP_ASSERT_EQ(executed.load(), 20);
}

// ============================================================
// Test 19: Pool Continues After Task Failure
// ============================================================

void testContinuesAfterTaskFailure() {
    WorkerPool pool(1);

    auto failed = pool.submit([]() -> int {
        throw std::runtime_error("Task failed");
    });

    auto successful = pool.submit([]() {
        return 99;
    });

    bool caught = false;

    try {
        (void)failed.get();
    } catch (const std::runtime_error&) {
        caught = true;
    }

    LAMP_ASSERT(caught);
    LAMP_ASSERT_EQ(successful.get(), 99);

    pool.shutdown();
}

// ============================================================
// Test 20: Reference Return
// ============================================================

void testReferenceReturn() {
    WorkerPool pool(1);

    int value = 42;

    auto result = pool.submit([&]() -> int& {
        return value;
    });

    int& reference = result.get();

    LAMP_ASSERT(&reference == &value);

    reference = 100;

    LAMP_ASSERT_EQ(value, 100);

    pool.shutdown();
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
        {"Construction", testConstruction},
        {"Zero worker count", testZeroWorkerCount},
        {"Integer task", testIntegerTask},
        {"Void task", testVoidTask},
        {"Move-only task", testMoveOnlyTask},
        {"Task exception propagation",
         testTaskExceptionPropagation},
        {"Multiple tasks", testMultipleTasks},
        {"Concurrent submissions",
         testConcurrentSubmissions},
        {"Worker identification",
         testWorkerThreadIdentification},
        {"Graceful shutdown drains queue",
         testGracefulShutdownDrainsQueue},
        {"Immediate shutdown cancels pending",
         testImmediateShutdownCancelsPending},
        {"Submission after shutdown",
         testSubmissionAfterShutdown},
        {"Repeated shutdown", testRepeatedShutdown},
        {"Worker-initiated shutdown rejected",
         testWorkerInitiatedShutdownRejected},
        {"Concurrent shutdown",
         testConcurrentShutdown},
        {"Pending task count", testPendingTaskCount},
        {"FIFO task ordering", testTaskFifoOrdering},
        {"Destructor drains tasks",
         testDestructorDrainsTasks},
        {"Continues after task failure",
         testContinuesAfterTaskFailure},
        {"Reference return", testReferenceReturn}
    };

    std::size_t passed = 0;
    std::size_t failed = 0;

    std::cout
        << "\n====================================\n"
        << " Lamp Worker Pool Tests\n"
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
