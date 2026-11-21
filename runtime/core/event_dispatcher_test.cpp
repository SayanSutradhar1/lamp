#include "core/event_dispatcher.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
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

class TestFailure final : public std::runtime_error {
public:
    explicit TestFailure(std::string message)
        : std::runtime_error(std::move(message)) {}
};

#define LAMP_ASSERT(condition)                                            \
    do {                                                                  \
        if (!(condition)) {                                               \
            throw TestFailure(std::string("Assertion failed: ") +         \
                              #condition + " at line " +                 \
                              std::to_string(__LINE__));                  \
        }                                                                 \
    } while (false)

#define LAMP_ASSERT_EQ(actual, expected)                                  \
    do {                                                                  \
        const auto& lamp_actual = (actual);                               \
        const auto& lamp_expected = (expected);                           \
        LAMP_ASSERT(lamp_actual == lamp_expected);                        \
    } while (false)

struct TestEvent {
    int id = 0;
    std::string message;
};

void testEmptyDispatcher() {
    EventDispatcher<TestEvent> dispatcher;
    LAMP_ASSERT_EQ(dispatcher.subscriberCount(), 0U);
    LAMP_ASSERT_EQ(dispatcher.pendingCount(), 0U);
    LAMP_ASSERT(!dispatcher.isDispatching());
    dispatcher.publish({1, "ignored"});
    LAMP_ASSERT_EQ(dispatcher.pendingCount(), 0U);
    LAMP_ASSERT(!dispatcher.isDispatching());
}

void testRejectEmptyCallback() {
    EventDispatcher<TestEvent> dispatcher;
    EventDispatcher<TestEvent>::Callback callback;
    LAMP_ASSERT(!dispatcher.subscribe(std::move(callback)));
    LAMP_ASSERT_EQ(dispatcher.subscriberCount(), 0U);
}

void testSingleSubscriber() {
    EventDispatcher<TestEvent> dispatcher;
    int calls = 0;
    TestEvent received;
    LAMP_ASSERT(dispatcher.subscribe([&](const TestEvent& event) {
        ++calls;
        received = event;
    }));
    dispatcher.publish({7, "hello"});
    LAMP_ASSERT_EQ(calls, 1);
    LAMP_ASSERT_EQ(received.id, 7);
    LAMP_ASSERT_EQ(received.message, "hello");
}

void testMultipleSubscribersInRegistrationOrder() {
    EventDispatcher<TestEvent> dispatcher;
    std::vector<int> calls;
    for (int i = 1; i <= 3; ++i) {
        LAMP_ASSERT(dispatcher.subscribe([&, i](const TestEvent&) {
            calls.push_back(i);
        }));
    }
    dispatcher.publish({1, "order"});
    LAMP_ASSERT_EQ(calls, (std::vector<int>{1, 2, 3}));
    LAMP_ASSERT_EQ(dispatcher.subscriberCount(), 3U);
}

void testSequentialFifo() {
    EventDispatcher<TestEvent> dispatcher;
    std::vector<int> received;
    LAMP_ASSERT(dispatcher.subscribe([&](const TestEvent& event) {
        received.push_back(event.id);
    }));
    for (int i = 0; i < 100; ++i) {
        dispatcher.publish({i, "fifo"});
    }
    LAMP_ASSERT_EQ(received.size(), 100U);
    for (int i = 0; i < 100; ++i) {
        LAMP_ASSERT_EQ(received.at(static_cast<std::size_t>(i)), i);
    }
}

void testCallbackExceptionIsolation() {
    EventDispatcher<TestEvent> dispatcher;
    int calls = 0;
    LAMP_ASSERT(dispatcher.subscribe([](const TestEvent&) {
        throw std::runtime_error("expected callback failure");
    }));
    LAMP_ASSERT(dispatcher.subscribe([&](const TestEvent&) {
        ++calls;
    }));
    dispatcher.publish({1, "first"});
    dispatcher.publish({2, "second"});
    LAMP_ASSERT_EQ(calls, 2);
    LAMP_ASSERT(!dispatcher.isDispatching());
}

void testReentrantPublicationIsQueued() {
    EventDispatcher<TestEvent> dispatcher;
    std::vector<std::string> order;
    LAMP_ASSERT(dispatcher.subscribe([&](const TestEvent& event) {
        order.push_back("A" + std::to_string(event.id));
        if (event.id == 1) {
            dispatcher.publish({2, "nested"});
            LAMP_ASSERT(dispatcher.isDispatching());
            LAMP_ASSERT_EQ(dispatcher.pendingCount(), 1U);
        }
    }));
    LAMP_ASSERT(dispatcher.subscribe([&](const TestEvent& event) {
        order.push_back("B" + std::to_string(event.id));
    }));
    dispatcher.publish({1, "root"});
    LAMP_ASSERT_EQ(order,
                   (std::vector<std::string>{"A1", "B1", "A2", "B2"}));
    LAMP_ASSERT_EQ(dispatcher.pendingCount(), 0U);
}

void testDeepReentrancyDoesNotRecurse() {
    EventDispatcher<TestEvent> dispatcher;
    constexpr int eventCount = 10000;
    int observed = 0;
    LAMP_ASSERT(dispatcher.subscribe([&](const TestEvent& event) {
        ++observed;
        if (event.id < eventCount) {
            dispatcher.publish({event.id + 1, "chain"});
        }
    }));
    dispatcher.publish({1, "chain"});
    LAMP_ASSERT_EQ(observed, eventCount);
    LAMP_ASSERT(!dispatcher.isDispatching());
}

void testSubscriptionDuringDispatch() {
    EventDispatcher<TestEvent> dispatcher;
    std::vector<std::string> order;
    LAMP_ASSERT(dispatcher.subscribe([&](const TestEvent& event) {
        order.push_back("first:" + std::to_string(event.id));
        if (event.id == 1) {
            LAMP_ASSERT(dispatcher.subscribe([&](const TestEvent& next) {
                order.push_back("late:" + std::to_string(next.id));
            }));
            dispatcher.publish({2, "next"});
        }
    }));
    dispatcher.publish({1, "initial"});
    LAMP_ASSERT_EQ(order,
                   (std::vector<std::string>{"first:1", "first:2", "late:2"}));
}

void testSnapshotIsolation() {
    EventDispatcher<TestEvent> dispatcher;
    std::vector<std::string> order;
    LAMP_ASSERT(dispatcher.subscribe([&](const TestEvent& event) {
        order.push_back("A" + std::to_string(event.id));
        if (event.id == 1) {
            LAMP_ASSERT(dispatcher.subscribe([&](const TestEvent& next) {
                order.push_back("C" + std::to_string(next.id));
            }));
        }
    }));
    LAMP_ASSERT(dispatcher.subscribe([&](const TestEvent& event) {
        order.push_back("B" + std::to_string(event.id));
    }));
    dispatcher.publish({1, "one"});
    dispatcher.publish({2, "two"});
    LAMP_ASSERT_EQ(order,
                   (std::vector<std::string>{"A1", "B1", "A2", "B2", "C2"}));
}

void testDispatcherCanDrainAgain() {
    EventDispatcher<TestEvent> dispatcher;
    int observed = 0;
    LAMP_ASSERT(dispatcher.subscribe([&](const TestEvent&) { ++observed; }));
    for (int i = 0; i < 10; ++i) {
        dispatcher.publish({i, "repeat"});
        LAMP_ASSERT(!dispatcher.isDispatching());
        LAMP_ASSERT_EQ(dispatcher.pendingCount(), 0U);
    }
    LAMP_ASSERT_EQ(observed, 10);
}

void testConcurrentPublishers() {
    EventDispatcher<TestEvent> dispatcher;
    constexpr int threadCount = 8;
    constexpr int eventsPerThread = 250;
    constexpr int total = threadCount * eventsPerThread;
    std::vector<std::atomic<int>> seen(static_cast<std::size_t>(total));
    for (auto& counter : seen) {
        counter.store(0);
    }
    std::atomic<int> delivered{0};
    LAMP_ASSERT(dispatcher.subscribe([&](const TestEvent& event) {
        seen.at(static_cast<std::size_t>(event.id)).fetch_add(1);
        delivered.fetch_add(1);
    }));
    std::vector<std::thread> threads;
    threads.reserve(threadCount);
    for (int t = 0; t < threadCount; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < eventsPerThread; ++i) {
                dispatcher.publish({t * eventsPerThread + i, "concurrent"});
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    LAMP_ASSERT_EQ(delivered.load(), total);
    for (const auto& counter : seen) {
        LAMP_ASSERT_EQ(counter.load(), 1);
    }
    LAMP_ASSERT_EQ(dispatcher.pendingCount(), 0U);
    LAMP_ASSERT(!dispatcher.isDispatching());
}

void testConcurrentCallbacksNeverOverlap() {
    EventDispatcher<TestEvent> dispatcher;
    std::atomic<int> active{0};
    std::atomic<int> maximum{0};
    std::atomic<int> calls{0};
    LAMP_ASSERT(dispatcher.subscribe([&](const TestEvent&) {
        const int now = active.fetch_add(1) + 1;
        int previous = maximum.load();
        while (previous < now &&
               !maximum.compare_exchange_weak(previous, now)) {}
        std::this_thread::sleep_for(50us);
        calls.fetch_add(1);
        active.fetch_sub(1);
    }));
    constexpr int threadCount = 6;
    constexpr int eventsPerThread = 40;
    std::vector<std::thread> threads;
    for (int t = 0; t < threadCount; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < eventsPerThread; ++i) {
                dispatcher.publish({t * eventsPerThread + i, "serial"});
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    LAMP_ASSERT_EQ(calls.load(), threadCount * eventsPerThread);
    LAMP_ASSERT_EQ(maximum.load(), 1);
}

void testPendingEventsWhileCallbackBlocked() {
    EventDispatcher<TestEvent> dispatcher;
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    bool release = false;
    std::vector<int> observed;
    LAMP_ASSERT(dispatcher.subscribe([&](const TestEvent& event) {
        if (event.id == 1) {
            std::unique_lock<std::mutex> lock(mutex);
            entered = true;
            cv.notify_all();
            cv.wait(lock, [&]() { return release; });
        }
        observed.push_back(event.id);
    }));

    std::thread owner([&]() { dispatcher.publish({1, "blocked"}); });
    {
        std::unique_lock<std::mutex> lock(mutex);
        if (!cv.wait_for(lock, 5s, [&]() { return entered; })) {
            release = true;
            lock.unlock();
            cv.notify_all();
            owner.join();
            throw TestFailure("Timed out waiting for callback to start");
        }
    }
    // Another publisher enqueues while the first callback is blocked.
    dispatcher.publish({2, "queued"});
    dispatcher.publish({3, "queued"});
    const bool dispatching = dispatcher.isDispatching();
    const auto pending = dispatcher.pendingCount();
    {
        std::lock_guard<std::mutex> lock(mutex);
        release = true;
    }
    cv.notify_all();
    owner.join();
    LAMP_ASSERT(dispatching);
    LAMP_ASSERT_EQ(pending, 2U);
    LAMP_ASSERT_EQ(observed, (std::vector<int>{1, 2, 3}));
}

void testIndependentDispatchers() {
    EventDispatcher<TestEvent> first;
    EventDispatcher<TestEvent> second;
    int firstCalls = 0;
    int secondCalls = 0;
    LAMP_ASSERT(first.subscribe([&](const TestEvent&) { ++firstCalls; }));
    LAMP_ASSERT(second.subscribe([&](const TestEvent&) { ++secondCalls; }));
    first.publish({1, "first"});
    second.publish({2, "second"});
    second.publish({3, "second"});
    LAMP_ASSERT_EQ(firstCalls, 1);
    LAMP_ASSERT_EQ(secondCalls, 2);
}

struct TestCase {
    std::string_view name;
    void (*execute)();
};

int runTests() {
    const std::vector<TestCase> tests{
        {"Empty dispatcher", testEmptyDispatcher},
        {"Reject empty callback", testRejectEmptyCallback},
        {"Single subscriber", testSingleSubscriber},
        {"Subscriber registration order", testMultipleSubscribersInRegistrationOrder},
        {"Sequential FIFO", testSequentialFifo},
        {"Callback exception isolation", testCallbackExceptionIsolation},
        {"Reentrant publication is queued", testReentrantPublicationIsQueued},
        {"Deep reentrancy", testDeepReentrancyDoesNotRecurse},
        {"Subscription during dispatch", testSubscriptionDuringDispatch},
        {"Subscriber snapshot isolation", testSnapshotIsolation},
        {"Repeated queue draining", testDispatcherCanDrainAgain},
        {"Concurrent publishers", testConcurrentPublishers},
        {"Concurrent callback serialization", testConcurrentCallbacksNeverOverlap},
        {"Pending events during blocked callback", testPendingEventsWhileCallbackBlocked},
        {"Independent dispatchers", testIndependentDispatchers}
    };

    std::size_t passed = 0;
    std::size_t failed = 0;
    std::cout << "\nLamp EventDispatcher Tests\n\n";
    for (const auto& test : tests) {
        try {
            test.execute();
            ++passed;
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception& exception) {
            ++failed;
            std::cout << "[FAIL] " << test.name << ": " << exception.what() << '\n';
        } catch (...) {
            ++failed;
            std::cout << "[FAIL] " << test.name << ": unknown exception\n";
        }
        std::cout.flush();
    }
    std::cout << "\nPassed: " << passed << " | Failed: " << failed
              << " | Total: " << tests.size() << '\n';
    return failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

} // namespace

int main() {
    return runTests();
}
