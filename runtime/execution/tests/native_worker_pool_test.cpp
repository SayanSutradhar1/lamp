
#include "execution/async_execution_contract.hpp"
#include "execution/native_worker_pool.hpp"

#include "bridge/execution_dispatcher.hpp"
#include "bridge/operation_registry.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
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

using namespace lamp::bridge;
using namespace lamp::execution;
using namespace std::chrono_literals;

// ============================================================
// Minimal test framework
// ============================================================

class TestFailure final : public std::runtime_error {
public:
    explicit TestFailure(std::string message)
        : std::runtime_error(std::move(message)) {}
};

#define LAMP_CHECK(expression)                             \
    do {                                                   \
        if (!(expression)) {                                \
            throw TestFailure(                             \
                std::string("Assertion failed: ") +        \
                #expression + " at line " +                 \
                std::to_string(__LINE__)                   \
            );                                             \
        }                                                  \
    } while (false)

#define LAMP_CHECK_EQ(actual, expected)                    \
    LAMP_CHECK((actual) == (expected))

using TestFunction = void (*)();

struct TestCase {
    const char* name;
    TestFunction function;
    bool expected_failure = false;
};

std::vector<TestCase>& TestRegistry() {
    static std::vector<TestCase> tests;
    return tests;
}

struct TestRegistrar {
    TestRegistrar(
        const char* name,
        TestFunction function,
        bool expected_failure = false
    ) {
        TestRegistry().push_back({
            name,
            function,
            expected_failure
        });
    }
};

#define LAMP_TEST(name)                                    \
    void name();                                           \
    TestRegistrar registrar_##name(#name, &name);         \
    void name()

#define LAMP_XFAIL_TEST(name)                              \
    void name();                                           \
    TestRegistrar registrar_##name(                        \
        #name, &name, true                                 \
    );                                                     \
    void name()

// ============================================================
// Test utilities
// ============================================================

ResultEnvelope::Payload Bytes(std::string_view value) {
    return {value.begin(), value.end()};
}

RequestEnvelope MakeRequest(
    std::string operation,
    std::uint64_t request_id = 1
) {
    RequestEnvelope request;

    request.request_id = request_id;
    request.operation_id = std::move(operation);
    request.transport = TransportKind::Internal;
    request.format = SerializationFormat::Json;
    request.payload = Bytes("{}");

    return request;
}

OperationDescriptor MakeDescriptor(
    std::string id
) {
    OperationDescriptor descriptor;

    descriptor.id = std::move(id);
    descriptor.execution.mode = ExecutionMode::Native;
    descriptor.input_format = SerializationFormat::Json;
    descriptor.output_format = SerializationFormat::Json;

    return descriptor;
}

class LambdaHandler final
    : public INativeOperationHandler {
public:
    using Callback = std::function<
        ResultEnvelope(const RequestEnvelope&)
    >;

    explicit LambdaHandler(Callback callback)
        : callback_(std::move(callback)) {}

    ResultEnvelope Execute(
        const RequestEnvelope& request
    ) override {
        return callback_(request);
    }

private:
    Callback callback_;
};

struct TestEnvironment {
    std::shared_ptr<OperationRegistry> registry =
        std::make_shared<OperationRegistry>();

    std::shared_ptr<ExecutionDispatcher> dispatcher =
        std::make_shared<ExecutionDispatcher>(registry);

    void Register(
        std::string id,
        LambdaHandler::Callback callback
    ) {
        auto descriptor = MakeDescriptor(id);

        OperationBackends backends;
        backends.native =
            std::make_shared<LambdaHandler>(
                std::move(callback)
            );

        auto result = registry->Register(
            std::move(descriptor),
            std::move(backends)
        );

        LAMP_CHECK(result.Ok());
    }
};

ResultEnvelope Success(
    const RequestEnvelope& request,
    std::string_view response = "{}"
) {
    return ResultEnvelope::Success(
        request,
        Bytes(response),
        SerializationFormat::Json,
        ExecutionBackend::Native
    );
}

NativeWorkerPoolConfig PoolConfig(
    std::size_t workers = 2,
    std::size_t pending = 16
) {
    NativeWorkerPoolConfig config;
    config.worker_count = workers;
    config.max_pending_tasks = pending;
    config.priority_burst_limit = 4;
    return config;
}

AsyncTaskHandle SubmitOrThrow(
    NativeWorkerPool& pool,
    RequestEnvelope request,
    AsyncScheduleOptions options = {}
) {
    AsyncTaskHandle handle;

    auto status = pool.Submit(
        std::move(request),
        options,
        handle
    );

    LAMP_CHECK(status.Accepted());
    LAMP_CHECK(handle.Valid());

    return handle;
}

// ============================================================
// Synchronization helper
// ============================================================
//
// Allows tests to hold worker threads without relying on
// arbitrary sleep durations.
// ============================================================

class Gate final {
public:
    void Open() noexcept {
        {
            std::lock_guard lock(mutex_);
            open_ = true;
        }

        cv_.notify_all();
    }

    void Wait() {
        std::unique_lock lock(mutex_);

        cv_.wait(lock, [&] {
            return open_;
        });
    }

    ~Gate() {
        Open();
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool open_ = false;
};

bool WaitUntil(
    const std::function<bool()>& predicate,
    std::chrono::milliseconds timeout = 2s
) {
    const auto deadline =
        std::chrono::steady_clock::now() + timeout;

    while (!predicate()) {
        if (std::chrono::steady_clock::now() >=
            deadline) {
            return false;
        }

        std::this_thread::yield();
    }

    return true;
}

// ============================================================
// Contract tests
// ============================================================

LAMP_TEST(DefaultHandleIsInvalid) {
    AsyncTaskHandle handle;

    LAMP_CHECK(!handle.Valid());
    LAMP_CHECK(!handle.IsCompleted());
    LAMP_CHECK_EQ(handle.RequestId(), 0U);
    LAMP_CHECK(!handle.Cancel());

    bool threw = false;

    try {
        (void)handle.Future();
    } catch (const std::logic_error&) {
        threw = true;
    }

    LAMP_CHECK(threw);
}

LAMP_TEST(CompletionTransitionsExactlyOnce) {
    auto request = MakeRequest("task.complete");

    auto completion =
        std::make_shared<AsyncTaskCompletion>(
            std::move(request)
        );

    LAMP_CHECK_EQ(
        completion->State(),
        AsyncTaskState::Created
    );

    LAMP_CHECK(completion->MarkQueued());
    LAMP_CHECK(!completion->MarkQueued());
    LAMP_CHECK(completion->MarkRunning());

    auto result = Success(completion->Request());

    LAMP_CHECK(completion->Complete(
        std::move(result)
    ));

    LAMP_CHECK_EQ(
        completion->State(),
        AsyncTaskState::Succeeded
    );

    LAMP_CHECK(completion->IsCompleted());

    LAMP_CHECK(!completion->Complete(
        Success(completion->Request())
    ));

    LAMP_CHECK(completion->Get().IsSuccess());
}

LAMP_TEST(CompletionFutureCanBeReadRepeatedly) {
    auto completion =
        std::make_shared<AsyncTaskCompletion>(
            MakeRequest("task.future")
        );

    LAMP_CHECK(completion->MarkQueued());

    LAMP_CHECK(completion->Complete(
        Success(completion->Request())
    ));

    auto first = completion->Get();
    auto second = completion->Get();

    LAMP_CHECK(first.IsSuccess());
    LAMP_CHECK(second.IsSuccess());
}

LAMP_TEST(CompletionRejectsPrematurePublication) {
    auto completion =
        std::make_shared<AsyncTaskCompletion>(
            MakeRequest("task.premature")
        );

    LAMP_CHECK(!completion->Complete(
        Success(completion->Request())
    ));

    LAMP_CHECK_EQ(
        completion->State(),
        AsyncTaskState::Created
    );
}

// ============================================================
// Worker pool lifecycle
// ============================================================

LAMP_TEST(PoolStartsWithConfiguredWorkers) {
    TestEnvironment environment;

    NativeWorkerPool pool(
        environment.dispatcher,
        PoolConfig(3)
    );

    LAMP_CHECK(pool.IsRunning());
    LAMP_CHECK_EQ(pool.WorkerCount(), 3U);
    LAMP_CHECK_EQ(pool.PendingTasks(), 0U);
    LAMP_CHECK_EQ(pool.ActiveTasks(), 0U);

    pool.Shutdown();

    LAMP_CHECK(!pool.IsRunning());
}

LAMP_TEST(RejectNullDispatcher) {
    bool threw = false;

    try {
        NativeWorkerPool pool(
            nullptr,
            PoolConfig()
        );
    } catch (const std::invalid_argument&) {
        threw = true;
    }

    LAMP_CHECK(threw);
}

LAMP_TEST(RejectZeroQueueCapacity) {
    TestEnvironment environment;

    auto config = PoolConfig();
    config.max_pending_tasks = 0;

    bool threw = false;

    try {
        NativeWorkerPool pool(
            environment.dispatcher,
            config
        );
    } catch (const std::invalid_argument&) {
        threw = true;
    }

    LAMP_CHECK(threw);
}

LAMP_TEST(ShutdownIsIdempotent) {
    TestEnvironment environment;

    NativeWorkerPool pool(
        environment.dispatcher,
        PoolConfig()
    );

    pool.Shutdown();
    pool.Shutdown();

    LAMP_CHECK(!pool.IsRunning());
}

LAMP_TEST(RejectSubmissionAfterShutdown) {
    TestEnvironment environment;

    NativeWorkerPool pool(
        environment.dispatcher,
        PoolConfig()
    );

    pool.Shutdown();

    AsyncTaskHandle handle;

    auto result = pool.Submit(
        MakeRequest("task.stopped"),
        {},
        handle
    );

    LAMP_CHECK_EQ(
        result.status,
        AsyncSubmitStatus::SchedulerStopped
    );

    LAMP_CHECK(!handle.Valid());
}

// ============================================================
// Asynchronous execution
// ============================================================

LAMP_TEST(ExecutesNativeTaskAsynchronously) {
    TestEnvironment environment;

    environment.Register(
        "task.echo",
        [](const RequestEnvelope& request) {
            return Success(
                request,
                R"({"ok":true})"
            );
        }
    );

    NativeWorkerPool pool(
        environment.dispatcher,
        PoolConfig()
    );

    auto handle = SubmitOrThrow(
        pool,
        MakeRequest("task.echo", 100)
    );

    LAMP_CHECK(handle.WaitFor(2s));

    auto result = handle.Get();

    LAMP_CHECK(result.IsSuccess());

    LAMP_CHECK_EQ(
        result.request_id,
        100U
    );

    LAMP_CHECK_EQ(
        handle.State(),
        AsyncTaskState::Succeeded
    );

    pool.Shutdown();
}

LAMP_TEST(ExecutesMultipleTasks) {
    TestEnvironment environment;

    std::atomic<int> calls{0};

    environment.Register(
        "task.multi",
        [&](const RequestEnvelope& request) {
            calls.fetch_add(
                1,
                std::memory_order_relaxed
            );

            return Success(request);
        }
    );

    NativeWorkerPool pool(
        environment.dispatcher,
        PoolConfig(4, 128)
    );

    constexpr std::size_t kTaskCount = 100;

    std::vector<AsyncTaskHandle> handles;
    handles.reserve(kTaskCount);

    for (std::size_t i = 0; i < kTaskCount; ++i) {
        handles.push_back(
            SubmitOrThrow(
                pool,
                MakeRequest(
                    "task.multi",
                    static_cast<std::uint64_t>(i + 1)
                )
            )
        );
    }

    for (auto& handle : handles) {
        LAMP_CHECK(handle.WaitFor(2s));
        LAMP_CHECK(handle.Get().IsSuccess());
    }

    LAMP_CHECK_EQ(calls.load(), 100);

    pool.Shutdown();
}

LAMP_TEST(MultipleWorkersExecuteConcurrently) {
    TestEnvironment environment;

    Gate gate;

    std::atomic<int> entered{0};

    environment.Register(
        "task.parallel",
        [&](const RequestEnvelope& request) {
            entered.fetch_add(
                1,
                std::memory_order_release
            );

            gate.Wait();

            return Success(request);
        }
    );

    NativeWorkerPool pool(
        environment.dispatcher,
        PoolConfig(3, 8)
    );

    auto first = SubmitOrThrow(
        pool,
        MakeRequest("task.parallel", 1)
    );

    auto second = SubmitOrThrow(
        pool,
        MakeRequest("task.parallel", 2)
    );

    auto third = SubmitOrThrow(
        pool,
        MakeRequest("task.parallel", 3)
    );

    const bool concurrent = WaitUntil(
        [&] {
            return entered.load(
                std::memory_order_acquire
            ) == 3;
        }
    );

    gate.Open();

    LAMP_CHECK(concurrent);

    LAMP_CHECK(first.WaitFor(2s));
    LAMP_CHECK(second.WaitFor(2s));
    LAMP_CHECK(third.WaitFor(2s));

    pool.Shutdown();
}

// ============================================================
// Queue admission
// ============================================================

LAMP_TEST(RejectsWhenPendingQueueIsFull) {
    TestEnvironment environment;

    Gate gate;
    std::atomic<bool> entered{false};

    environment.Register(
        "task.block",
        [&](const RequestEnvelope& request) {
            entered.store(
                true,
                std::memory_order_release
            );

            gate.Wait();
            return Success(request);
        }
    );

    NativeWorkerPool pool(
        environment.dispatcher,
        PoolConfig(1, 1)
    );

    auto running = SubmitOrThrow(
        pool,
        MakeRequest("task.block", 1)
    );

    LAMP_CHECK(WaitUntil([&] {
        return entered.load(
            std::memory_order_acquire
        );
    }));

    auto queued = SubmitOrThrow(
        pool,
        MakeRequest("task.block", 2)
    );

    AsyncTaskHandle rejected;

    auto status = pool.Submit(
        MakeRequest("task.block", 3),
        {},
        rejected
    );

    LAMP_CHECK_EQ(
        status.status,
        AsyncSubmitStatus::QueueFull
    );

    LAMP_CHECK(!rejected.Valid());

    gate.Open();

    LAMP_CHECK(running.WaitFor(2s));
    LAMP_CHECK(queued.WaitFor(2s));

    pool.Shutdown();
}

LAMP_TEST(RejectsImmediateTaskWhenWorkersBusy) {
    TestEnvironment environment;

    Gate gate;
    std::atomic<bool> entered{false};

    environment.Register(
        "task.immediate",
        [&](const RequestEnvelope& request) {
            entered.store(
                true,
                std::memory_order_release
            );

            gate.Wait();
            return Success(request);
        }
    );

    NativeWorkerPool pool(
        environment.dispatcher,
        PoolConfig(1, 4)
    );

    auto running = SubmitOrThrow(
        pool,
        MakeRequest("task.immediate", 1)
    );

    LAMP_CHECK(WaitUntil([&] {
        return entered.load(
            std::memory_order_acquire
        );
    }));

    AsyncScheduleOptions options;
    options.allow_queueing = false;

    AsyncTaskHandle rejected;

    auto status = pool.Submit(
        MakeRequest("task.immediate", 2),
        options,
        rejected
    );

    LAMP_CHECK_EQ(
        status.status,
        AsyncSubmitStatus::QueueFull
    );

    gate.Open();

    LAMP_CHECK(running.WaitFor(2s));

    pool.Shutdown();
}

// ============================================================
// Cancellation and deadlines
// ============================================================

LAMP_TEST(RejectsCancelledRequestAtSubmission) {
    TestEnvironment environment;

    NativeWorkerPool pool(
        environment.dispatcher,
        PoolConfig()
    );

    RequestCancellationSource source;

    auto request = MakeRequest("task.cancel");
    request.cancellation = source.Token();

    source.Cancel();

    AsyncTaskHandle handle;

    auto status = pool.Submit(
        std::move(request),
        {},
        handle
    );

    LAMP_CHECK_EQ(
        status.status,
        AsyncSubmitStatus::Cancelled
    );

    LAMP_CHECK(!handle.Valid());

    pool.Shutdown();
}

LAMP_TEST(RejectsExpiredRequestAtSubmission) {
    TestEnvironment environment;

    NativeWorkerPool pool(
        environment.dispatcher,
        PoolConfig()
    );

    auto request = MakeRequest("task.expired");

    request.deadline =
        RequestDeadline::After(-1ms);

    AsyncTaskHandle handle;

    auto status = pool.Submit(
        std::move(request),
        {},
        handle
    );

    LAMP_CHECK_EQ(
        status.status,
        AsyncSubmitStatus::DeadlineExceeded
    );

    LAMP_CHECK(!handle.Valid());

    pool.Shutdown();
}

LAMP_TEST(CancelsQueuedTaskBeforeExecution) {
    TestEnvironment environment;

    Gate gate;
    std::atomic<bool> entered{false};
    std::atomic<int> calls{0};

    environment.Register(
        "task.queued.cancel",
        [&](const RequestEnvelope& request) {
            calls.fetch_add(
                1,
                std::memory_order_relaxed
            );

            entered.store(
                true,
                std::memory_order_release
            );

            gate.Wait();
            return Success(request);
        }
    );

    NativeWorkerPool pool(
        environment.dispatcher,
        PoolConfig(1, 4)
    );

    auto running = SubmitOrThrow(
        pool,
        MakeRequest("task.queued.cancel", 1)
    );

    LAMP_CHECK(WaitUntil([&] {
        return entered.load(
            std::memory_order_acquire
        );
    }));

    auto queued = SubmitOrThrow(
        pool,
        MakeRequest("task.queued.cancel", 2)
    );

    LAMP_CHECK(queued.Cancel());
    LAMP_CHECK(queued.CancellationRequested());

    gate.Open();

    LAMP_CHECK(running.WaitFor(2s));
    LAMP_CHECK(queued.WaitFor(2s));

    LAMP_CHECK(queued.Get().IsCancelled());

    LAMP_CHECK_EQ(
        queued.State(),
        AsyncTaskState::Cancelled
    );

    LAMP_CHECK_EQ(calls.load(), 1);

    pool.Shutdown();
}

LAMP_TEST(QueuedTaskDeadlineExpires) {
    TestEnvironment environment;

    Gate gate;
    std::atomic<bool> entered{false};

    environment.Register(
        "task.deadline",
        [&](const RequestEnvelope& request) {
            entered.store(
                true,
                std::memory_order_release
            );

            gate.Wait();
            return Success(request);
        }
    );

    NativeWorkerPool pool(
        environment.dispatcher,
        PoolConfig(1, 4)
    );

    auto running = SubmitOrThrow(
        pool,
        MakeRequest("task.deadline", 1)
    );

    LAMP_CHECK(WaitUntil([&] {
        return entered.load(
            std::memory_order_acquire
        );
    }));

    auto request = MakeRequest(
        "task.deadline",
        2
    );

    request.deadline =
        RequestDeadline::After(20ms);

    auto queued = SubmitOrThrow(
        pool,
        std::move(request)
    );

    std::this_thread::sleep_for(40ms);

    gate.Open();

    LAMP_CHECK(running.WaitFor(2s));
    LAMP_CHECK(queued.WaitFor(2s));

    LAMP_CHECK(
        queued.Get().IsDeadlineExceeded()
    );

    pool.Shutdown();
}

LAMP_TEST(QueuedTaskWaitLimitExpires) {
    TestEnvironment environment;

    Gate gate;
    std::atomic<bool> entered{false};

    environment.Register(
        "task.queue.timeout",
        [&](const RequestEnvelope& request) {
            entered.store(
                true,
                std::memory_order_release
            );

            gate.Wait();
            return Success(request);
        }
    );

    NativeWorkerPool pool(
        environment.dispatcher,
        PoolConfig(1, 4)
    );

    auto running = SubmitOrThrow(
        pool,
        MakeRequest("task.queue.timeout", 1)
    );

    LAMP_CHECK(WaitUntil([&] {
        return entered.load(
            std::memory_order_acquire
        );
    }));

    AsyncScheduleOptions options;
    options.max_queue_wait = 20ms;

    auto queued = SubmitOrThrow(
        pool,
        MakeRequest("task.queue.timeout", 2),
        options
    );

    std::this_thread::sleep_for(40ms);

    gate.Open();

    LAMP_CHECK(running.WaitFor(2s));
    LAMP_CHECK(queued.WaitFor(2s));

    auto result = queued.Get();

    LAMP_CHECK(result.IsFailure());

    LAMP_CHECK_EQ(
        result.error.identifier,
        "QUEUE_WAIT_EXCEEDED"
    );

    pool.Shutdown();
}

// ============================================================
// Priority scheduling
// ============================================================

LAMP_TEST(HigherPriorityRunsFirstWhenQueued) {
    TestEnvironment environment;

    Gate gate;
    std::atomic<bool> entered{false};

    std::mutex order_mutex;
    std::vector<std::uint64_t> order;

    environment.Register(
        "task.priority",
        [&](const RequestEnvelope& request) {
            if (request.request_id == 1) {
                entered.store(
                    true,
                    std::memory_order_release
                );

                gate.Wait();
            }

            {
                std::lock_guard lock(order_mutex);
                order.push_back(request.request_id);
            }

            return Success(request);
        }
    );

    NativeWorkerPool pool(
        environment.dispatcher,
        PoolConfig(1, 8)
    );

    auto blocker = SubmitOrThrow(
        pool,
        MakeRequest("task.priority", 1)
    );

    LAMP_CHECK(WaitUntil([&] {
        return entered.load(
            std::memory_order_acquire
        );
    }));

    AsyncScheduleOptions low;
    low.priority = TaskPriority::Low;

    AsyncScheduleOptions high;
    high.priority = TaskPriority::High;

    auto low_task = SubmitOrThrow(
        pool,
        MakeRequest("task.priority", 2),
        low
    );

    auto high_task = SubmitOrThrow(
        pool,
        MakeRequest("task.priority", 3),
        high
    );

    gate.Open();

    LAMP_CHECK(blocker.WaitFor(2s));
    LAMP_CHECK(low_task.WaitFor(2s));
    LAMP_CHECK(high_task.WaitFor(2s));

    {
        std::lock_guard lock(order_mutex);

        LAMP_CHECK_EQ(order.size(), 3U);
        LAMP_CHECK_EQ(order[0], 1U);
        LAMP_CHECK_EQ(order[1], 3U);
        LAMP_CHECK_EQ(order[2], 2U);
    }

    pool.Shutdown();
}

// ============================================================
// Shutdown behavior
// ============================================================

LAMP_TEST(ShutdownDrainsAcceptedTasks) {
    TestEnvironment environment;

    std::atomic<int> calls{0};

    environment.Register(
        "task.drain",
        [&](const RequestEnvelope& request) {
            calls.fetch_add(
                1,
                std::memory_order_relaxed
            );

            return Success(request);
        }
    );

    NativeWorkerPool pool(
        environment.dispatcher,
        PoolConfig(2, 64)
    );

    std::vector<AsyncTaskHandle> handles;

    for (std::uint64_t i = 1; i <= 40; ++i) {
        handles.push_back(
            SubmitOrThrow(
                pool,
                MakeRequest("task.drain", i)
            )
        );
    }

    pool.Shutdown();

    LAMP_CHECK(!pool.IsRunning());

    for (const auto& handle : handles) {
        LAMP_CHECK(handle.IsCompleted());
        LAMP_CHECK(handle.Get().IsSuccess());
    }

    LAMP_CHECK_EQ(calls.load(), 40);
    LAMP_CHECK_EQ(pool.PendingTasks(), 0U);
    LAMP_CHECK_EQ(pool.ActiveTasks(), 0U);
}

// ============================================================
// Known limitation regression
// ============================================================
//
// The current completion contract owns a separate
// cancellation source from the request.
//
// Calling handle.Cancel() does not automatically cancel
// request.cancellation observed by the native handler.
//
// This test documents the missing propagation behavior.
// ============================================================

LAMP_TEST(
    RunningTaskObservesHandleCancellation
) {
    TestEnvironment environment;

    std::atomic<bool> entered{false};
    std::atomic<bool> observed{false};

    Gate gate;

    environment.Register(
        "task.running.cancel",
        [&](const RequestEnvelope& request) {
            entered.store(
                true,
                std::memory_order_release
            );

            gate.Wait();

            observed.store(
                request.IsCancelled(),
                std::memory_order_release
            );

            return Success(request);
        }
    );

    NativeWorkerPool pool(
        environment.dispatcher,
        PoolConfig(1, 4)
    );

    auto handle = SubmitOrThrow(
        pool,
        MakeRequest("task.running.cancel")
    );

    LAMP_CHECK(WaitUntil([&] {
        return entered.load(
            std::memory_order_acquire
        );
    }));

    LAMP_CHECK(handle.Cancel());

    gate.Open();

    LAMP_CHECK(handle.WaitFor(2s));

    // Expected to fail until the worker pool propagates
    // the handle's cancellation into the request token.
    LAMP_CHECK(
        observed.load(std::memory_order_acquire)
    );

    pool.Shutdown();
}

// ============================================================
// Test runner
// ============================================================

} // namespace

int main() {
    std::size_t passed = 0;
    std::size_t failed = 0;
    std::size_t expected_failures = 0;
    std::size_t unexpected_passes = 0;

    std::cout
        << "\n"
        << "========================================\n"
        << " Lamp Phase 5 - Native Worker Pool Tests\n"
        << "========================================\n\n";

    for (const auto& test : TestRegistry()) {
        try {
            test.function();

            if (test.expected_failure) {
                ++unexpected_passes;

                std::cout
                    << "[XPASS] "
                    << test.name
                    << '\n';
            } else {
                ++passed;

                std::cout
                    << "[PASS] "
                    << test.name
                    << '\n';
            }

        } catch (const std::exception& error) {
            if (test.expected_failure) {
                ++expected_failures;

                std::cout
                    << "[XFAIL] "
                    << test.name
                    << ": "
                    << error.what()
                    << '\n';
            } else {
                ++failed;

                std::cerr
                    << "[FAIL] "
                    << test.name
                    << ": "
                    << error.what()
                    << '\n';
            }

        } catch (...) {
            if (test.expected_failure) {
                ++expected_failures;

                std::cout
                    << "[XFAIL] "
                    << test.name
                    << ": unknown exception\n";
            } else {
                ++failed;

                std::cerr
                    << "[FAIL] "
                    << test.name
                    << ": unknown exception\n";
            }
        }
    }

    std::cout
        << "\n----------------------------------------\n"
        << "Passed:            " << passed << '\n'
        << "Failed:            " << failed << '\n'
        << "Expected failures: " << expected_failures
        << '\n'
        << "Unexpected passes: " << unexpected_passes
        << '\n'
        << "Total:             "
        << TestRegistry().size()
        << '\n'
        << "----------------------------------------\n";

    return (failed == 0 && unexpected_passes == 0)
        ? 0
        : 1;
}
