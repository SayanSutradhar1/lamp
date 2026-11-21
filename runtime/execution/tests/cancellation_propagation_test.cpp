
#include "bridge/request_envelope.hpp"
#include "execution/async_execution_contract.hpp"

#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

namespace bridge = lamp::bridge;
namespace execution = lamp::execution;

class TestFailure final : public std::runtime_error {
public:
    explicit TestFailure(std::string message)
        : std::runtime_error(std::move(message)) {}
};

void Require(
    bool condition,
    std::string_view message
) {
    if (!condition) {
        throw TestFailure(std::string(message));
    }
}

struct TestCase final {
    const char* name;
    std::function<void()> execute;
};

std::vector<TestCase>& Tests() {
    static std::vector<TestCase> tests;
    return tests;
}

struct RegisterTest final {
    RegisterTest(
        const char* name,
        std::function<void()> execute
    ) {
        Tests().push_back({
            name,
            std::move(execute)
        });
    }
};

#define LAMP_TEST(name)                          \
    void name();                                 \
    const RegisterTest register_##name(          \
        #name,                                   \
        [] { name(); }                           \
    );                                           \
    void name()

bridge::RequestEnvelope MakeRequest(
    bridge::RequestCancellationToken token = {}
) {
    bridge::RequestEnvelope request;

    request.request_id = 1001;
    request.operation_id = "tests.cancellation";
    request.transport = bridge::TransportKind::Internal;
    request.cancellation = std::move(token);

    return request;
}

bridge::ResultEnvelope CancelledResult(
    const execution::AsyncTaskCompletion& completion
) {
    return bridge::ResultEnvelope::Cancelled(
        completion.Request()
    );
}

// ============================================================
// Composite token behavior
// ============================================================

LAMP_TEST(DefaultTokenCannotBeCancelled) {
    bridge::RequestCancellationToken token;

    Require(
        !token.CanBeCancelled(),
        "Default token must have no cancellation source"
    );

    Require(
        !token.IsCancelled(),
        "Default token must not be cancelled"
    );
}

LAMP_TEST(CombinedTokenObservesFirstSource) {
    bridge::RequestCancellationSource first;
    bridge::RequestCancellationSource second;

    auto token =
        bridge::RequestCancellationToken::Combine(
            first.Token(),
            second.Token()
        );

    Require(token.CanBeCancelled(), "Token must be active");
    Require(!token.IsCancelled(), "Token starts active");

    first.Cancel();

    Require(
        token.IsCancelled(),
        "First source cancellation must propagate"
    );

    Require(
        !second.IsCancelled(),
        "Combining tokens must not cancel other sources"
    );
}

LAMP_TEST(CombinedTokenObservesSecondSource) {
    bridge::RequestCancellationSource first;
    bridge::RequestCancellationSource second;

    auto token =
        bridge::RequestCancellationToken::Combine(
            first.Token(),
            second.Token()
        );

    second.Cancel();

    Require(
        token.IsCancelled(),
        "Second source cancellation must propagate"
    );

    Require(
        !first.IsCancelled(),
        "Original source must remain independent"
    );
}

LAMP_TEST(CombiningWithEmptyTokenPreservesSource) {
    bridge::RequestCancellationSource source;
    bridge::RequestCancellationToken empty;

    auto first =
        bridge::RequestCancellationToken::Combine(
            empty,
            source.Token()
        );

    auto second =
        bridge::RequestCancellationToken::Combine(
            source.Token(),
            empty
        );

    source.Cancel();

    Require(first.IsCancelled(), "Left empty token failed");
    Require(second.IsCancelled(), "Right empty token failed");
}

LAMP_TEST(NestedCombinedTokensPropagate) {
    bridge::RequestCancellationSource a;
    bridge::RequestCancellationSource b;
    bridge::RequestCancellationSource c;

    auto inner =
        bridge::RequestCancellationToken::Combine(
            a.Token(),
            b.Token()
        );

    auto outer =
        bridge::RequestCancellationToken::Combine(
            inner,
            c.Token()
        );

    c.Cancel();

    Require(
        outer.IsCancelled(),
        "Nested composite must observe third source"
    );
}

LAMP_TEST(CombinedTokenRetainsSourceLifetime) {
    bridge::RequestCancellationToken token;

    {
        bridge::RequestCancellationSource first;
        bridge::RequestCancellationSource second;

        token =
            bridge::RequestCancellationToken::Combine(
                first.Token(),
                second.Token()
            );

        second.Cancel();
    }

    Require(
        token.IsCancelled(),
        "Token must retain shared cancellation state"
    );
}

// ============================================================
// Async completion integration
// ============================================================

LAMP_TEST(CompletionPreservesCallerCancellation) {
    bridge::RequestCancellationSource caller;

    execution::AsyncTaskCompletion completion(
        MakeRequest(caller.Token())
    );

    Require(
        !completion.CancellationRequested(),
        "Task must initially be active"
    );

    caller.Cancel();

    Require(
        completion.CancellationRequested(),
        "Completion must observe caller cancellation"
    );

    Require(
        completion.Request().IsCancelled(),
        "Execution request must observe caller cancellation"
    );
}

LAMP_TEST(CompletionObservesTaskHandleCancellation) {
    auto completion =
        std::make_shared<execution::AsyncTaskCompletion>(
            MakeRequest()
        );

    execution::AsyncTaskHandle handle(completion);

    Require(handle.Cancel(), "Handle cancellation failed");

    Require(
        completion->Request().IsCancelled(),
        "Execution request must observe handle cancellation"
    );
}

LAMP_TEST(CancellationTokenExposesBothSources) {
    bridge::RequestCancellationSource caller;

    execution::AsyncTaskCompletion completion(
        MakeRequest(caller.Token())
    );

    auto token = completion.CancellationToken();

    caller.Cancel();

    Require(
        token.IsCancelled(),
        "Public completion token must include caller source"
    );
}

LAMP_TEST(CallerCancellationDuringRunningTask) {
    bridge::RequestCancellationSource caller;

    auto completion =
        std::make_shared<execution::AsyncTaskCompletion>(
            MakeRequest(caller.Token())
        );

    Require(completion->MarkQueued(), "Queue transition failed");
    Require(completion->MarkRunning(), "Run transition failed");

    // The request token is obtained before cancellation,
    // just as it would be inside a running native handler.
    const auto token = completion->Request().cancellation;

    std::promise<void> worker_ready;
    auto ready = worker_ready.get_future();

    std::promise<void> continue_worker;
    auto proceed = continue_worker.get_future().share();

    std::atomic<bool> observed{false};

    std::thread worker([
        &worker_ready,
        &proceed,
        &observed,
        token
    ] {
        worker_ready.set_value();
        proceed.wait();

        observed.store(
            token.IsCancelled(),
            std::memory_order_release
        );
    });

    ready.wait();

    caller.Cancel();
    continue_worker.set_value();

    worker.join();

    Require(
        observed.load(std::memory_order_acquire),
        "Running task did not observe caller cancellation"
    );

    Require(
        completion->Complete(CancelledResult(*completion)),
        "Cancelled result publication failed"
    );

    Require(
        completion->State() ==
            execution::AsyncTaskState::Cancelled,
        "Task must reach Cancelled state"
    );
}

LAMP_TEST(HandleCancellationDuringRunningTask) {
    auto completion =
        std::make_shared<execution::AsyncTaskCompletion>(
            MakeRequest()
        );

    execution::AsyncTaskHandle handle(completion);

    Require(completion->MarkQueued(), "Queue transition failed");
    Require(completion->MarkRunning(), "Run transition failed");

    const auto token = completion->Request().cancellation;

    std::promise<void> worker_ready;
    auto ready = worker_ready.get_future();

    std::promise<void> continue_worker;
    auto proceed = continue_worker.get_future().share();

    std::atomic<bool> observed{false};

    std::thread worker([
        &worker_ready,
        &proceed,
        &observed,
        token
    ] {
        worker_ready.set_value();
        proceed.wait();

        observed.store(
            token.IsCancelled(),
            std::memory_order_release
        );
    });

    ready.wait();

    Require(handle.Cancel(), "Running task cancellation failed");

    continue_worker.set_value();
    worker.join();

    Require(
        observed.load(std::memory_order_acquire),
        "Running task did not observe handle cancellation"
    );
}

LAMP_TEST(ConcurrentCancellationSources) {
    bridge::RequestCancellationSource caller;

    auto completion =
        std::make_shared<execution::AsyncTaskCompletion>(
            MakeRequest(caller.Token())
        );

    execution::AsyncTaskHandle handle(completion);

    std::atomic<bool> start{false};

    std::thread caller_thread([&] {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        caller.Cancel();
    });

    std::thread handle_thread([&] {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        (void)handle.Cancel();
    });

    start.store(true, std::memory_order_release);

    caller_thread.join();
    handle_thread.join();

    Require(
        completion->Request().IsCancelled(),
        "Concurrent cancellation must be visible"
    );

    Require(
        caller.IsCancelled(),
        "Caller cancellation must remain set"
    );

    Require(
        handle.CancellationRequested(),
        "Handle must report cancellation"
    );
}

LAMP_TEST(CancellationDoesNotAffectOtherTasks) {
    bridge::RequestCancellationSource first_caller;
    bridge::RequestCancellationSource second_caller;

    execution::AsyncTaskCompletion first(
        MakeRequest(first_caller.Token())
    );

    execution::AsyncTaskCompletion second(
        MakeRequest(second_caller.Token())
    );

    first_caller.Cancel();

    Require(
        first.Request().IsCancelled(),
        "First task must be cancelled"
    );

    Require(
        !second.Request().IsCancelled(),
        "Unrelated task must remain active"
    );
}

LAMP_TEST(CancellationAfterCompletionIsRejected) {
    auto completion =
        std::make_shared<execution::AsyncTaskCompletion>(
            MakeRequest()
        );

    execution::AsyncTaskHandle handle(completion);

    Require(completion->MarkQueued(), "Queue transition failed");
    Require(completion->MarkRunning(), "Run transition failed");

    auto result = bridge::ResultEnvelope::Success(
        completion->Request(),
        {},
        bridge::SerializationFormat::Json,
        bridge::ExecutionBackend::Native
    );

    Require(
        completion->Complete(std::move(result)),
        "Success publication failed"
    );

    Require(
        !handle.Cancel(),
        "Cancellation after completion must be rejected"
    );

    Require(
        completion->State() ==
            execution::AsyncTaskState::Succeeded,
        "Completed task state must remain unchanged"
    );
}

LAMP_TEST(CancelledResultPublishedExactlyOnce) {
    execution::AsyncTaskCompletion completion(
        MakeRequest()
    );

    Require(completion.MarkQueued(), "Queue transition failed");

    Require(
        completion.Complete(CancelledResult(completion)),
        "First completion must succeed"
    );

    Require(
        !completion.Complete(CancelledResult(completion)),
        "Second completion must be rejected"
    );

    Require(
        completion.Get().IsCancelled(),
        "Published result must indicate cancellation"
    );
}

// ============================================================
// Test runner
// ============================================================

} // namespace

int main() {
    std::size_t passed = 0;
    std::size_t failed = 0;

    std::cout
        << "\n========================================\n"
        << " Lamp Phase 5 - Cancellation Tests\n"
        << "========================================\n\n";

    for (const auto& test : Tests()) {
        try {
            test.execute();

            ++passed;

            std::cout
                << "[PASS] "
                << test.name
                << '\n';

        } catch (const std::exception& error) {
            ++failed;

            std::cerr
                << "[FAIL] "
                << test.name
                << ": "
                << error.what()
                << '\n';

        } catch (...) {
            ++failed;

            std::cerr
                << "[FAIL] "
                << test.name
                << ": unknown exception\n";
        }
    }

    std::cout
        << "\n----------------------------------------\n"
        << "Passed: " << passed << '\n'
        << "Failed: " << failed << '\n'
        << "Total:  " << Tests().size() << '\n'
        << "----------------------------------------\n";

    return failed == 0 ? 0 : 1;
}
