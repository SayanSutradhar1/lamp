
#include "http/http_completion_queue.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace lamp::http;

void Check(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

template <typename Exception, typename Function>
void CheckThrows(Function&& function, std::string_view message) {
    try {
        std::forward<Function>(function)();
    } catch (const Exception&) {
        return;
    }

    throw std::runtime_error(std::string(message));
}

HttpCompletionToken Token(
    std::uint64_t requestId = 1,
    std::uint64_t connectionId = 1,
    std::uint64_t generation = 1
) {
    return {connectionId, generation, requestId};
}

HttpCompletion Completion(
    std::uint64_t requestId,
    HttpCompletionCallback callback
) {
    return {Token(requestId), std::move(callback)};
}

void TestDefaultConfiguration() {
    HttpCompletionQueue queue;

    Check(queue.capacity() == 4096, "Incorrect default capacity");
    Check(queue.empty(), "New queue must be empty");
    Check(queue.size() == 0, "New queue size must be zero");
    Check(!queue.closed(), "New queue must be open");
    Check(queue.drain() == 0, "Empty drain must return zero");
    Check(!queue.tryPop().has_value(), "Empty pop must fail");
}

void TestInvalidCapacity() {
    CheckThrows<HttpCompletionQueueError>(
        [] {
            HttpCompletionQueue queue({0});
            (void)queue;
        },
        "Zero capacity must be rejected"
    );
}

void TestTokenValidation() {
    Check(Token().valid(), "Valid token rejected");
    Check(!Token(0).valid(), "Zero request ID accepted");
    Check(!Token(1, 0).valid(), "Zero connection ID accepted");
    Check(!Token(1, 1, 0).valid(), "Zero generation accepted");

    Check(
        Token(1) == Token(1),
        "Equal tokens must compare equal"
    );

    Check(
        !(Token(1) == Token(2)),
        "Different request IDs must not compare equal"
    );

    Check(
        !(Token(1, 2) == Token(1, 1)),
        "Different connection IDs must not compare equal"
    );

    Check(
        !(Token(1, 1, 2) == Token(1, 1, 1)),
        "Different generations must not compare equal"
    );
}

void TestInvalidCompletions() {
    HttpCompletionQueue queue({2});

    HttpCompletion invalidToken{
        Token(0),
        [](const HttpCompletionToken&) {}
    };

    Check(
        queue.post(invalidToken) ==
            HttpCompletionPostResult::Invalid,
        "Invalid token was accepted"
    );

    HttpCompletion invalidCallback{
        Token(),
        HttpCompletionCallback{}
    };

    Check(
        queue.post(std::move(invalidCallback)) ==
            HttpCompletionPostResult::Invalid,
        "Empty callback was accepted"
    );

    Check(queue.empty(), "Invalid post changed queue");
}

void TestPublishDoesNotExecute() {
    HttpCompletionQueue queue;
    int calls = 0;

    Check(
        queue.post(Completion(
            1,
            [&](const HttpCompletionToken&) {
                ++calls;
            }
        )) == HttpCompletionPostResult::Accepted,
        "Post failed"
    );

    Check(calls == 0, "Post executed callback");
    Check(queue.size() == 1, "Completion was not queued");

    Check(queue.drain() == 1, "Drain count incorrect");
    Check(calls == 1, "Callback did not execute");
    Check(queue.empty(), "Queue not empty after drain");
}

void TestFIFOOrdering() {
    HttpCompletionQueue queue;
    std::vector<std::uint64_t> observed;

    for (std::uint64_t id = 1; id <= 10; ++id) {
        const auto result = queue.post(Completion(
            id,
            [&](const HttpCompletionToken& token) {
                observed.push_back(token.requestId);
            }
        ));

        Check(
            result == HttpCompletionPostResult::Accepted,
            "FIFO test post failed"
        );
    }

    Check(queue.drain() == 10, "Incorrect FIFO drain count");

    for (std::uint64_t id = 1; id <= 10; ++id) {
        Check(
            observed[static_cast<std::size_t>(id - 1)] == id,
            "FIFO order violated"
        );
    }
}

void TestBoundedCapacity() {
    HttpCompletionQueue queue({2});
    int calls = 0;

    auto callback = [&](const HttpCompletionToken&) {
        ++calls;
    };

    Check(
        queue.post(Completion(1, callback)) ==
            HttpCompletionPostResult::Accepted,
        "First post failed"
    );

    Check(
        queue.post(Completion(2, callback)) ==
            HttpCompletionPostResult::Accepted,
        "Second post failed"
    );

    Check(
        queue.post(Completion(3, callback)) ==
            HttpCompletionPostResult::Full,
        "Full queue accepted another completion"
    );

    Check(queue.size() == 2, "Capacity was exceeded");
    Check(calls == 0, "Callback ran during posting");

    Check(queue.drain(1) == 1, "Partial drain failed");

    Check(
        queue.post(Completion(3, callback)) ==
            HttpCompletionPostResult::Accepted,
        "Capacity was not restored after draining"
    );

    Check(queue.drain() == 2, "Remaining drain failed");
    Check(calls == 3, "Incorrect callback count");
}

void TestRejectedLvaluePreserved() {
    HttpCompletionQueue queue({1});

    Check(
        queue.post(Completion(
            1,
            [](const HttpCompletionToken&) {}
        )) == HttpCompletionPostResult::Accepted,
        "Initial post failed"
    );

    HttpCompletion pending = Completion(
        2,
        [](const HttpCompletionToken&) {}
    );

    Check(
        queue.post(pending) == HttpCompletionPostResult::Full,
        "Expected Full result"
    );

    Check(
        pending.token.requestId == 2 && pending.callback,
        "Rejected lvalue completion was modified"
    );
}

void TestTryPopTransfersOwnership() {
    HttpCompletionQueue queue;
    int calls = 0;

    Check(
        queue.post(Completion(
            42,
            [&](const HttpCompletionToken&) {
                ++calls;
            }
        )) == HttpCompletionPostResult::Accepted,
        "Post failed"
    );

    auto completion = queue.tryPop();

    Check(completion.has_value(), "Expected completion");
    Check(
        completion->token.requestId == 42,
        "Incorrect popped token"
    );

    Check(queue.empty(), "Pop did not remove completion");
    Check(calls == 0, "Pop executed callback");

    completion->callback(completion->token);

    Check(calls == 1, "Transferred callback failed");
}

void TestPartialDrain() {
    HttpCompletionQueue queue;
    int calls = 0;

    for (std::uint64_t id = 1; id <= 5; ++id) {
        Check(
            queue.post(Completion(
                id,
                [&](const HttpCompletionToken&) {
                    ++calls;
                }
            )) == HttpCompletionPostResult::Accepted,
            "Post failed"
        );
    }

    Check(queue.drain(0) == 0, "Zero-limit drain executed work");
    Check(queue.size() == 5, "Zero-limit drain removed work");

    Check(queue.drain(2) == 2, "Partial drain count incorrect");
    Check(calls == 2, "Partial drain executed wrong count");
    Check(queue.size() == 3, "Partial drain left wrong size");

    Check(queue.drain() == 3, "Final drain count incorrect");
    Check(calls == 5, "Some callbacks were not executed");
}

void TestCloseAndDrain() {
    HttpCompletionQueue queue;
    int calls = 0;

    Check(
        queue.post(Completion(
            1,
            [&](const HttpCompletionToken&) {
                ++calls;
            }
        )) == HttpCompletionPostResult::Accepted,
        "Initial post failed"
    );

    queue.close();
    queue.close();

    Check(queue.closed(), "Queue did not close");

    Check(
        queue.post(Completion(
            2,
            [&](const HttpCompletionToken&) {
                ++calls;
            }
        )) == HttpCompletionPostResult::Closed,
        "Closed queue accepted new work"
    );

    Check(queue.drain() == 1, "Pending work lost on close");
    Check(calls == 1, "Pending callback did not execute");
    Check(queue.empty(), "Closed queue not empty after drain");
}

void TestDiscardPending() {
    HttpCompletionQueue queue;
    int calls = 0;

    for (std::uint64_t id = 1; id <= 3; ++id) {
        Check(
            queue.post(Completion(
                id,
                [&](const HttpCompletionToken&) {
                    ++calls;
                }
            )) == HttpCompletionPostResult::Accepted,
            "Post failed"
        );
    }

    Check(queue.discardPending() == 3, "Discard count incorrect");
    Check(queue.empty(), "Discard did not clear queue");
    Check(queue.discardPending() == 0, "Second discard not empty");
    Check(queue.drain() == 0, "Discarded work was drained");
    Check(calls == 0, "Discarded callback executed");
}

void TestCallbackException() {
    HttpCompletionQueue queue;
    int secondCalls = 0;

    Check(
        queue.post(Completion(
            1,
            [](const HttpCompletionToken&) {
                throw std::runtime_error("callback failure");
            }
        )) == HttpCompletionPostResult::Accepted,
        "First post failed"
    );

    Check(
        queue.post(Completion(
            2,
            [&](const HttpCompletionToken&) {
                ++secondCalls;
            }
        )) == HttpCompletionPostResult::Accepted,
        "Second post failed"
    );

    CheckThrows<std::runtime_error>(
        [&] {
            (void)queue.drain();
        },
        "Callback exception did not propagate"
    );

    Check(
        queue.size() == 1,
        "Exception removed subsequent completion"
    );

    Check(secondCalls == 0, "Later callback ran prematurely");
    Check(queue.drain() == 1, "Remaining callback not drained");
    Check(secondCalls == 1, "Remaining callback not executed");
}

void TestCallbackCanPost() {
    HttpCompletionQueue queue;
    int calls = 0;

    Check(
        queue.post(Completion(
            1,
            [&](const HttpCompletionToken&) {
                ++calls;

                const auto result = queue.post(Completion(
                    2,
                    [&](const HttpCompletionToken&) {
                        ++calls;
                    }
                ));

                Check(
                    result == HttpCompletionPostResult::Accepted,
                    "Callback could not post another completion"
                );
            }
        )) == HttpCompletionPostResult::Accepted,
        "Initial post failed"
    );

    Check(
        queue.drain(1) == 1,
        "First callback did not execute"
    );

    Check(queue.size() == 1, "Reentrant post missing");
    Check(queue.drain() == 1, "Posted callback not drained");
    Check(calls == 2, "Incorrect callback execution count");
}

void TestMultiProducerConcurrency() {
    constexpr int producerCount = 8;
    constexpr int completionsPerProducer = 500;
    constexpr int total = producerCount * completionsPerProducer;

    HttpCompletionQueue queue({total});

    std::atomic<int> accepted{0};
    std::atomic<int> failures{0};
    std::atomic<int> executed{0};

    std::vector<std::thread> producers;
    producers.reserve(producerCount);

    for (int producer = 0; producer < producerCount; ++producer) {
        producers.emplace_back([&, producer] {
            try {
                for (int i = 0; i < completionsPerProducer; ++i) {
                    const std::uint64_t requestId =
                        static_cast<std::uint64_t>(
                            producer * completionsPerProducer + i + 1
                        );

                    const auto result = queue.post(Completion(
                        requestId,
                        [&](const HttpCompletionToken&) {
                            executed.fetch_add(
                                1,
                                std::memory_order_relaxed
                            );
                        }
                    ));

                    if (result == HttpCompletionPostResult::Accepted) {
                        accepted.fetch_add(
                            1,
                            std::memory_order_relaxed
                        );
                    } else {
                        failures.fetch_add(
                            1,
                            std::memory_order_relaxed
                        );
                    }
                }
            } catch (...) {
                failures.fetch_add(
                    1,
                    std::memory_order_relaxed
                );
            }
        });
    }

    for (auto& producer : producers) {
        producer.join();
    }

    Check(failures.load() == 0, "Concurrent post failed");
    Check(accepted.load() == total, "Lost producer completion");
    Check(queue.size() == total, "Incorrect concurrent queue size");

    Check(
        queue.drain() == static_cast<std::size_t>(total),
        "Concurrent completion drain count incorrect"
    );

    Check(executed.load() == total, "Lost callback execution");
    Check(queue.empty(), "Queue not empty after concurrent drain");
}

void TestConcurrentProducerAndConsumer() {
    constexpr int total = 2000;

    HttpCompletionQueue queue({128});

    std::atomic<int> executed{0};
    std::atomic<int> accepted{0};
    std::atomic<int> rejected{0};
    std::atomic<bool> producerDone{false};

    std::thread producer([&] {
        for (int i = 1; i <= total; ++i) {
            // Backpressure is handled by retrying from the
            // producer side. The queue itself never waits.
            for (;;) {
                const auto result = queue.post(Completion(
                    static_cast<std::uint64_t>(i),
                    [&](const HttpCompletionToken&) {
                        executed.fetch_add(
                            1,
                            std::memory_order_relaxed
                        );
                    }
                ));

                if (result == HttpCompletionPostResult::Accepted) {
                    accepted.fetch_add(
                        1,
                        std::memory_order_relaxed
                    );
                    break;
                }

                if (result != HttpCompletionPostResult::Full) {
                    rejected.fetch_add(
                        1,
                        std::memory_order_relaxed
                    );
                    break;
                }

                std::this_thread::yield();
            }
        }

        producerDone.store(true, std::memory_order_release);
    });

    // The main thread is the sole consumer.
    while (!producerDone.load(std::memory_order_acquire) ||
           !queue.empty()) {
        (void)queue.drain(32);
        std::this_thread::yield();
    }

    producer.join();

    Check(rejected.load() == 0, "Unexpected post rejection");
    Check(accepted.load() == total, "Lost completion");
    Check(executed.load() == total, "Lost callback");
    Check(queue.empty(), "Queue not fully drained");
}

void TestTokenPreservation() {
    HttpCompletionQueue queue;

    const HttpCompletionToken expected{
        987654321,
        42,
        123456789
    };

    HttpCompletionToken observed{};

    Check(
        queue.post(HttpCompletion{
            expected,
            [&](const HttpCompletionToken& token) {
                observed = token;
            }
        }) == HttpCompletionPostResult::Accepted,
        "Post failed"
    );

    Check(queue.drain() == 1, "Drain failed");
    Check(observed == expected, "Completion token changed");
}

struct TestCase {
    const char* name;
    void (*run)();
};

} // namespace

int main() {
    const std::vector<TestCase> tests = {
        {"DefaultConfiguration", TestDefaultConfiguration},
        {"InvalidCapacity", TestInvalidCapacity},
        {"TokenValidation", TestTokenValidation},
        {"InvalidCompletions", TestInvalidCompletions},
        {"PublishDoesNotExecute", TestPublishDoesNotExecute},
        {"FIFOOrdering", TestFIFOOrdering},
        {"BoundedCapacity", TestBoundedCapacity},
        {"RejectedLvaluePreserved", TestRejectedLvaluePreserved},
        {"TryPopTransfersOwnership", TestTryPopTransfersOwnership},
        {"PartialDrain", TestPartialDrain},
        {"CloseAndDrain", TestCloseAndDrain},
        {"DiscardPending", TestDiscardPending},
        {"CallbackException", TestCallbackException},
        {"CallbackCanPost", TestCallbackCanPost},
        {"MultiProducerConcurrency", TestMultiProducerConcurrency},
        {"ConcurrentProducerAndConsumer",
         TestConcurrentProducerAndConsumer},
        {"TokenPreservation", TestTokenPreservation}
    };

    std::size_t passed = 0;

    for (const auto& test : tests) {
        try {
            test.run();
            ++passed;
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception& exception) {
            std::cerr
                << "[FAIL] " << test.name
                << ": " << exception.what() << '\n';
            return 1;
        } catch (...) {
            std::cerr
                << "[FAIL] " << test.name
                << ": unknown exception\n";
            return 1;
        }
    }

    std::cout
        << "Lamp HTTP completion queue: "
        << passed << "/" << tests.size()
        << " tests passed\n";

    return passed == tests.size() ? 0 : 1;
}
