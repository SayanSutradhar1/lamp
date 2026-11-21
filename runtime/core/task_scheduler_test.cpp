#include "core/task_scheduler.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace lamp::core;
using namespace std::chrono_literals;

struct TestFailure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

#define CHECK(expr) do { if (!(expr)) throw TestFailure( \
    std::string("Check failed: ") + #expr + " (line " + \
    std::to_string(__LINE__) + ")"); } while (false)

template<class Predicate>
void await(Predicate predicate, std::chrono::milliseconds timeout = 3000ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline)
            throw TestFailure("Timed out waiting for condition");
        std::this_thread::sleep_for(1ms);
    }
}

class Gate {
public:
    ~Gate() { open(); }
    void wait() {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [this] { return open_; });
    }
    void open() {
        {
            std::lock_guard lock(mutex_);
            open_ = true;
        }
        cv_.notify_all();
    }
private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool open_ = false;
};

void testInitialState() {
    WorkerPool workers(2);
    TaskScheduler scheduler(workers);
    CHECK(scheduler.isAccepting());
    CHECK(scheduler.pendingTasks() == 0);
    scheduler.shutdown();
    CHECK(!scheduler.isAccepting());
    workers.shutdown();
}

void testDelayedExecution() {
    WorkerPool workers(2);
    TaskScheduler scheduler(workers);
    std::atomic<int> count{0};
    const auto start = TaskScheduler::Clock::now();
    auto handle = scheduler.scheduleAfter(50ms, [&] { ++count; });
    CHECK(static_cast<bool>(handle));
    CHECK(scheduler.contains(handle));
    await([&] { return count.load() == 1; });
    CHECK(TaskScheduler::Clock::now() - start >= 50ms);
    CHECK(!scheduler.contains(handle));
    scheduler.shutdown();
    workers.shutdown();
}

void testZeroDelay() {
    WorkerPool workers(1);
    TaskScheduler scheduler(workers);
    std::atomic<bool> executed{false};
    (void)scheduler.scheduleAfter(0ms, [&] { executed = true; });
    await([&] { return executed.load(); });
    scheduler.shutdown();
    workers.shutdown();
}

void testInvalidArguments() {
    WorkerPool workers(1);
    TaskScheduler scheduler(workers);
    int rejected = 0;
    try { (void)scheduler.scheduleAfter(-1ms, [] {}); }
    catch (const TaskSchedulerError&) { ++rejected; }
    try { (void)scheduler.scheduleEvery(0ms, [] {}); }
    catch (const TaskSchedulerError&) { ++rejected; }
    try { (void)scheduler.scheduleEvery(-1ms, [] {}); }
    catch (const TaskSchedulerError&) { ++rejected; }
    try { (void)scheduler.scheduleAfter(1ms, TaskScheduler::Task{}); }
    catch (const TaskSchedulerError&) { ++rejected; }
    CHECK(rejected == 4);
    scheduler.shutdown();
    workers.shutdown();
}

void testCancelOneShot() {
    WorkerPool workers(1);
    TaskScheduler scheduler(workers);
    std::atomic<int> count{0};
    auto handle = scheduler.scheduleAfter(500ms, [&] { ++count; });
    CHECK(scheduler.cancel(handle));
    CHECK(!scheduler.cancel(handle));
    CHECK(!scheduler.contains(handle));
    CHECK(scheduler.pendingTasks() == 0);
    scheduler.shutdown(true);
    CHECK(count == 0);
    workers.shutdown();
}

void testCancelUnknownHandle() {
    WorkerPool workers(1);
    TaskScheduler scheduler(workers);
    CHECK(!scheduler.cancel({}));
    CHECK(!scheduler.contains({}));
    CHECK(!scheduler.cancel({999999}));
    scheduler.shutdown();
    workers.shutdown();
}

void testRecurringExecutionAndCancellation() {
    WorkerPool workers(2);
    TaskScheduler scheduler(workers);
    std::atomic<int> count{0};
    auto handle = scheduler.scheduleEvery(10ms, [&] { ++count; });
    await([&] { return count.load() >= 3; });
    CHECK(scheduler.cancel(handle));
    scheduler.shutdown();
    CHECK(count.load() >= 3);
    workers.shutdown();
}

void testRecurringDoesNotOverlap() {
    WorkerPool workers(4);
    TaskScheduler scheduler(workers);
    std::atomic<int> active{0};
    std::atomic<int> maximum{0};
    std::atomic<int> completed{0};
    auto handle = scheduler.scheduleEvery(1ms, [&] {
        const int current = ++active;
        int previous = maximum.load();
        while (current > previous &&
               !maximum.compare_exchange_weak(previous, current)) {}
        std::this_thread::sleep_for(15ms);
        --active;
        ++completed;
    });
    await([&] { return completed.load() >= 3; });
    CHECK(scheduler.cancel(handle));
    scheduler.shutdown();
    CHECK(maximum.load() == 1);
    workers.shutdown();
}

void testCallbackExceptionIsolation() {
    WorkerPool workers(1);
    TaskScheduler scheduler(workers);
    std::atomic<bool> completed{false};
    (void)scheduler.scheduleAfter(0ms, [] {
        throw std::runtime_error("intentional failure");
    });
    (void)scheduler.scheduleAfter(0ms, [&] { completed = true; });
    await([&] { return completed.load(); });
    scheduler.shutdown();
    workers.shutdown();
}

void testRecurringExceptionDoesNotStopRecurrence() {
    WorkerPool workers(1);
    TaskScheduler scheduler(workers);
    std::atomic<int> attempts{0};
    auto handle = scheduler.scheduleEvery(5ms, [&] {
        ++attempts;
        throw std::runtime_error("expected");
    });
    await([&] { return attempts.load() >= 3; });
    CHECK(scheduler.cancel(handle));
    scheduler.shutdown();
    workers.shutdown();
}

void testConcurrentScheduling() {
    WorkerPool workers(4);
    TaskScheduler scheduler(workers);
    constexpr int producers = 4;
    constexpr int perProducer = 25;
    std::atomic<int> executed{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < producers; ++i) {
        threads.emplace_back([&] {
            for (int j = 0; j < perProducer; ++j)
                (void)scheduler.scheduleAfter(0ms, [&] { ++executed; });
        });
    }
    for (auto& thread : threads) thread.join();
    scheduler.shutdown(true);
    CHECK(executed.load() == producers * perProducer);
    workers.shutdown();
}

void testImmediateShutdownDiscardsPending() {
    WorkerPool workers(1);
    TaskScheduler scheduler(workers);
    std::atomic<int> executed{0};
    (void)scheduler.scheduleAfter(1h, [&] { ++executed; });
    (void)scheduler.scheduleEvery(1h, [&] { ++executed; });
    CHECK(scheduler.pendingTasks() == 2);
    scheduler.shutdown(false);
    CHECK(executed.load() == 0);
    CHECK(scheduler.pendingTasks() == 0);
    workers.shutdown();
}

void testGracefulShutdownDispatchesOneShots() {
    WorkerPool workers(2);
    TaskScheduler scheduler(workers);
    std::atomic<int> executed{0};
    (void)scheduler.scheduleAfter(1h, [&] { ++executed; });
    (void)scheduler.scheduleAfter(2h, [&] { ++executed; });
    (void)scheduler.scheduleEvery(1h, [&] { executed += 100; });
    scheduler.shutdown(true);
    CHECK(executed.load() == 2);
    CHECK(scheduler.pendingTasks() == 0);
    workers.shutdown();
}

void testShutdownWaitsForActiveCallback() {
    WorkerPool workers(1);
    TaskScheduler scheduler(workers);
    Gate gate;
    std::atomic<bool> started{false};
    std::atomic<bool> finished{false};
    (void)scheduler.scheduleAfter(0ms, [&] {
        started = true;
        gate.wait();
        finished = true;
    });
    await([&] { return started.load(); });
    std::thread closer([&] { scheduler.shutdown(false); });
    gate.open();
    closer.join();
    CHECK(finished.load());
    workers.shutdown();
}

void testSubmissionAfterShutdownRejected() {
    WorkerPool workers(1);
    TaskScheduler scheduler(workers);
    scheduler.shutdown();
    int rejected = 0;
    try { (void)scheduler.scheduleAfter(0ms, [] {}); }
    catch (const TaskSchedulerError&) { ++rejected; }
    try { (void)scheduler.scheduleEvery(1ms, [] {}); }
    catch (const TaskSchedulerError&) { ++rejected; }
    CHECK(rejected == 2);
    workers.shutdown();
}

void testRepeatedShutdown() {
    WorkerPool workers(1);
    TaskScheduler scheduler(workers);
    scheduler.shutdown();
    scheduler.shutdown();
    scheduler.shutdown(true);
    CHECK(!scheduler.isAccepting());
    workers.shutdown();
}

void testDestructorDiscardsFutureTasks() {
    WorkerPool workers(1);
    std::atomic<int> executed{0};
    {
        TaskScheduler scheduler(workers);
        (void)scheduler.scheduleAfter(1h, [&] { ++executed; });
    }
    CHECK(executed.load() == 0);
    workers.shutdown();
}

struct TestCase {
    const char* name;
    void (*run)();
};

} // namespace

int main() {
    const std::vector<TestCase> tests{
        {"Initial state", testInitialState},
        {"Delayed execution", testDelayedExecution},
        {"Zero delay", testZeroDelay},
        {"Invalid arguments", testInvalidArguments},
        {"Cancel one-shot", testCancelOneShot},
        {"Cancel unknown handle", testCancelUnknownHandle},
        {"Recurring execution and cancellation", testRecurringExecutionAndCancellation},
        {"Recurring tasks do not overlap", testRecurringDoesNotOverlap},
        {"Callback exception isolation", testCallbackExceptionIsolation},
        {"Recurring exception recovery", testRecurringExceptionDoesNotStopRecurrence},
        {"Concurrent scheduling", testConcurrentScheduling},
        {"Immediate shutdown discards pending", testImmediateShutdownDiscardsPending},
        {"Graceful shutdown dispatches one-shots", testGracefulShutdownDispatchesOneShots},
        {"Shutdown waits for active callback", testShutdownWaitsForActiveCallback},
        {"Submission after shutdown rejected", testSubmissionAfterShutdownRejected},
        {"Repeated shutdown", testRepeatedShutdown},
        {"Destructor discards future tasks", testDestructorDiscardsFutureTasks}
    };

    int passed = 0;
    for (const auto& test : tests) {
        try {
            test.run();
            ++passed;
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception& error) {
            std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n';
        } catch (...) {
            std::cerr << "[FAIL] " << test.name << ": unknown exception\n";
        }
    }
    std::cout << "Passed " << passed << "/" << tests.size() << " tests\n";
    return passed == static_cast<int>(tests.size()) ? EXIT_SUCCESS : EXIT_FAILURE;
}
