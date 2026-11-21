
#include "execution/v8_isolate_worker_pool.hpp"

#include <libplatform/libplatform.h>
#include <v8.h>

#include <atomic>
#include <chrono>
#include <cstddef>
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
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

namespace bridge = lamp::bridge;
namespace execution = lamp::execution;

// ============================================================
// Test infrastructure
// ============================================================

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

using TestFunction = void (*)();

struct TestCase final {
    const char* name;
    TestFunction function;
};

std::vector<TestCase>& Tests() {
    static std::vector<TestCase> tests;
    return tests;
}

struct RegisterTest final {
    RegisterTest(
        const char* name,
        TestFunction function
    ) {
        Tests().push_back({name, function});
    }
};

#define LAMP_TEST(name)                         \
    void name();                                \
    const RegisterTest register_##name(         \
        #name, &name                            \
    );                                          \
    void name()

// ============================================================
// V8 process lifecycle
// ============================================================

class V8Environment final {
public:
    V8Environment() {
        platform_ =
            ::v8::platform::NewDefaultPlatform();

        ::v8::V8::InitializePlatform(platform_.get());

        if (!::v8::V8::Initialize()) {
            throw std::runtime_error(
                "V8 initialization failed"
            );
        }
    }

    ~V8Environment() {
        ::v8::V8::Dispose();
        ::v8::V8::DisposePlatform();
    }

    V8Environment(const V8Environment&) = delete;

    V8Environment& operator=(
        const V8Environment&
    ) = delete;

private:
    std::unique_ptr<::v8::Platform> platform_;
};

// ============================================================
// Test helpers
// ============================================================

bridge::RequestEnvelope MakeRequest(
    bridge::RequestId id,
    bridge::RequestCancellationToken cancellation = {}
) {
    bridge::RequestEnvelope request;

    request.request_id = id;
    request.operation_id = "tests.v8.execute";
    request.transport = bridge::TransportKind::Internal;
    request.format = bridge::SerializationFormat::Json;
    request.cancellation = std::move(cancellation);

    return request;
}

struct SuccessExecutor final {
    bridge::ResultEnvelope operator()(
        const bridge::RequestEnvelope& request
    ) const {
        return bridge::ResultEnvelope::Success(
            request,
            {},
            bridge::SerializationFormat::Json,
            bridge::ExecutionBackend::JavaScript
        );
    }

    bridge::ResultEnvelope operator()(
        const bridge::RequestEnvelope& request,
        const execution::V8WorkerExecutionContext&
    ) const {
        return (*this)(request);
    }
};

inline constexpr SuccessExecutor Success{};

execution::V8IsolateWorkerPoolConfig Config(
    std::size_t workers = 2,
    std::size_t capacity = 32
) {
    execution::V8IsolateWorkerPoolConfig config;

    config.worker_count = workers;
    config.max_pending_tasks = capacity;
    config.max_heap_size_mb = 128;

    return config;
}

execution::AsyncTaskHandle Submit(
    execution::V8IsolateWorkerPool& pool,
    bridge::RequestEnvelope request,
    execution::AsyncScheduleOptions options = {}
) {
    execution::AsyncTaskHandle handle;

    auto result = pool.Submit(
        std::move(request),
        options,
        handle
    );

    Require(
        result.Accepted(),
        "Expected task submission to succeed"
    );

    Require(
        handle.Valid(),
        "Accepted task must return a valid handle"
    );

    return handle;
}

void RequireReady(
    const execution::AsyncTaskHandle& handle
) {
    Require(
        handle.WaitFor(5s),
        "Task future did not become ready"
    );
}

// A reusable gate for deterministic worker tests.
//
// The gate is released automatically on destruction,
// preventing a failed assertion from leaving a worker
// blocked indefinitely.

class WorkerGate final {
public:
    WorkerGate()
        : future_(promise_.get_future().share()) {}

    ~WorkerGate() {
        Release();
    }

    void Wait() const {
        future_.wait();
    }

    void Release() noexcept {
        std::call_once(release_once_, [this] {
            promise_.set_value();
        });
    }

private:
    std::promise<void> promise_;
    std::shared_future<void> future_;
    std::once_flag release_once_;
};

// ============================================================
// Configuration
// ============================================================

LAMP_TEST(RejectsZeroWorkers) {
    auto config = Config();
    config.worker_count = 0;

    bool rejected = false;

    try {
        execution::V8IsolateWorkerPool pool(
            Success,
            config
        );
    } catch (const std::invalid_argument&) {
        rejected = true;
    }

    Require(
        rejected,
        "Zero worker count must be rejected"
    );
}

LAMP_TEST(RejectsZeroQueueCapacity) {
    auto config = Config();
    config.max_pending_tasks = 0;

    bool rejected = false;

    try {
        execution::V8IsolateWorkerPool pool(
            Success,
            config
        );
    } catch (const std::invalid_argument&) {
        rejected = true;
    }

    Require(
        rejected,
        "Zero queue capacity must be rejected"
    );
}

LAMP_TEST(RejectsMissingExecutor) {
    bool rejected = false;

    try {
        execution::V8IsolateWorkerPool pool(
            {},
            Config()
        );
    } catch (const std::invalid_argument&) {
        rejected = true;
    }

    Require(
        rejected,
        "Missing executor must be rejected"
    );
}

// ============================================================
// Basic execution
// ============================================================

LAMP_TEST(ExecutesJavaScriptTask) {
    std::atomic<unsigned> calls{0};

    execution::V8IsolateWorkerPool pool(
        [&](const auto& request, const auto& context) {
            Require(
                context.isolate != nullptr,
                "Executor requires a V8 isolate"
            );

            Require(
                !context.context.IsEmpty(),
                "Executor requires a V8 context"
            );

            ++calls;
            return Success(request);
        },
        Config(1)
    );

    auto handle = Submit(pool, MakeRequest(1));

    RequireReady(handle);

    Require(
        handle.Get().IsSuccess(),
        "JavaScript task must succeed"
    );

    Require(
        calls.load() == 1,
        "Executor must run exactly once"
    );

    Require(
        handle.State() ==
            execution::AsyncTaskState::Succeeded,
        "Task must reach Succeeded state"
    );

    pool.Shutdown();
}

LAMP_TEST(ExecutesJavaScriptInsideContext) {
    execution::V8IsolateWorkerPool pool(
        [](const auto& request, const auto& worker) {
            auto* isolate = worker.isolate;

            auto source = ::v8::String::NewFromUtf8Literal(
                isolate,
                "20 + 22"
            );

            auto script =
                ::v8::Script::Compile(
                    worker.context,
                    source
                ).ToLocalChecked();

            auto value =
                script->Run(
                    worker.context
                ).ToLocalChecked();

            Require(
                value->Int32Value(
                    worker.context
                ).FromMaybe(0) == 42,
                "JavaScript expression returned wrong value"
            );

            return Success(request);
        },
        Config(1)
    );

    auto handle = Submit(pool, MakeRequest(2));

    RequireReady(handle);

    Require(
        handle.Get().IsSuccess(),
        "JavaScript expression must execute successfully"
    );
}

LAMP_TEST(ProcessesMultipleRequests) {
    constexpr std::size_t kTaskCount = 64;

    std::atomic<std::size_t> executed{0};

    execution::V8IsolateWorkerPool pool(
        [&](const auto& request, const auto&) {
            ++executed;
            return Success(request);
        },
        Config(4, kTaskCount)
    );

    std::vector<execution::AsyncTaskHandle> handles;
    handles.reserve(kTaskCount);

    for (std::size_t i = 0; i < kTaskCount; ++i) {
        handles.push_back(
            Submit(pool, MakeRequest(i + 100))
        );
    }

    for (const auto& handle : handles) {
        RequireReady(handle);

        Require(
            handle.Get().IsSuccess(),
            "Batch task failed"
        );
    }

    Require(
        executed.load() == kTaskCount,
        "Every accepted task must execute exactly once"
    );
}

// ============================================================
// Worker ownership
// ============================================================

LAMP_TEST(WorkersOwnDistinctIsolates) {
    constexpr std::size_t kWorkers = 3;

    WorkerGate gate;

    std::mutex mutex;
    std::unordered_set<::v8::Isolate*> isolates;
    std::unordered_set<std::thread::id> threads;

    std::atomic<std::size_t> entered{0};

    execution::V8IsolateWorkerPool pool(
        [&](const auto& request, const auto& worker) {
            {
                std::lock_guard lock(mutex);

                isolates.insert(worker.isolate);
                threads.insert(std::this_thread::get_id());
            }

            ++entered;
            gate.Wait();

            return Success(request);
        },
        Config(kWorkers)
    );

    std::vector<execution::AsyncTaskHandle> handles;

    for (std::size_t i = 0; i < kWorkers; ++i) {
        handles.push_back(
            Submit(pool, MakeRequest(i + 200))
        );
    }

    const auto deadline =
        std::chrono::steady_clock::now() + 5s;

    while (entered.load() < kWorkers &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }

    gate.Release();

    for (const auto& handle : handles) {
        RequireReady(handle);
    }

    Require(
        entered.load() == kWorkers,
        "Expected concurrent worker execution"
    );

    Require(
        isolates.size() == kWorkers,
        "Each worker must own a distinct V8 isolate"
    );

    Require(
        threads.size() == kWorkers,
        "Each worker must execute on a distinct thread"
    );
}

// ============================================================
// Queue capacity
// ============================================================

LAMP_TEST(RejectsWhenPendingQueueIsFull) {
    WorkerGate gate;

    std::promise<void> entered_promise;
    auto entered = entered_promise.get_future();

    execution::V8IsolateWorkerPool pool(
        [&](const auto& request, const auto&) {
            if (request.request_id == 300) {
                entered_promise.set_value();
                gate.Wait();
            }

            return Success(request);
        },
        Config(1, 1)
    );

    auto active = Submit(pool, MakeRequest(300));

    Require(
        entered.wait_for(5s) ==
            std::future_status::ready,
        "First task did not start"
    );

    auto pending = Submit(pool, MakeRequest(301));

    execution::AsyncTaskHandle rejected_handle;

    const auto result = pool.Submit(
        MakeRequest(302),
        {},
        rejected_handle
    );

    gate.Release();

    RequireReady(active);
    RequireReady(pending);

    Require(
        result.status ==
            execution::AsyncSubmitStatus::QueueFull,
        "Full queue must reject additional requests"
    );

    Require(
        !rejected_handle.Valid(),
        "Rejected submission must not return a task handle"
    );
}

// ============================================================
// Cancellation
// ============================================================

LAMP_TEST(RejectsPreCancelledRequest) {
    bridge::RequestCancellationSource cancellation;
    cancellation.Cancel();

    execution::V8IsolateWorkerPool pool(
        Success,
        Config(1)
    );

    execution::AsyncTaskHandle handle;

    auto result = pool.Submit(
        MakeRequest(400, cancellation.Token()),
        {},
        handle
    );

    Require(
        result.status ==
            execution::AsyncSubmitStatus::Cancelled,
        "Pre-cancelled request must be rejected"
    );

    Require(
        !handle.Valid(),
        "Rejected request must not create a handle"
    );
}

LAMP_TEST(QueuedTaskObservesCallerCancellation) {
    WorkerGate gate;

    std::promise<void> entered_promise;
    auto entered = entered_promise.get_future();

    bridge::RequestCancellationSource cancellation;

    execution::V8IsolateWorkerPool pool(
        [&](const auto& request, const auto&) {
            if (request.request_id == 500) {
                entered_promise.set_value();
                gate.Wait();
            }

            return Success(request);
        },
        Config(1)
    );

    auto active = Submit(pool, MakeRequest(500));

    Require(
        entered.wait_for(5s) ==
            std::future_status::ready,
        "Blocking task did not start"
    );

    auto queued = Submit(
        pool,
        MakeRequest(501, cancellation.Token())
    );

    cancellation.Cancel();
    gate.Release();

    RequireReady(active);
    RequireReady(queued);

    Require(
        queued.Get().IsCancelled(),
        "Queued task must observe caller cancellation"
    );
}

LAMP_TEST(RunningTaskObservesHandleCancellation) {
    std::promise<void> entered_promise;
    auto entered = entered_promise.get_future();

    WorkerGate gate;

    std::atomic<bool> observed{false};

    execution::V8IsolateWorkerPool pool(
        [&](const auto& request, const auto&) {
            entered_promise.set_value();

            gate.Wait();

            observed.store(
                request.IsCancelled(),
                std::memory_order_release
            );

            if (request.IsCancelled()) {
                return bridge::ResultEnvelope::Cancelled(
                    request
                );
            }

            return Success(request);
        },
        Config(1)
    );

    auto handle = Submit(pool, MakeRequest(600));

    Require(
        entered.wait_for(5s) ==
            std::future_status::ready,
        "Task did not enter executor"
    );

    const bool accepted = handle.Cancel();

    gate.Release();

    RequireReady(handle);

    Require(
        accepted,
        "Running task cancellation must be accepted"
    );

    Require(
        observed.load(std::memory_order_acquire),
        "Running executor must observe handle cancellation"
    );

    Require(
        handle.Get().IsCancelled(),
        "Executor must publish cancelled result"
    );
}

LAMP_TEST(RunningTaskObservesCallerCancellation) {
    std::promise<void> entered_promise;
    auto entered = entered_promise.get_future();

    WorkerGate gate;

    bridge::RequestCancellationSource cancellation;

    execution::V8IsolateWorkerPool pool(
        [&](const auto& request, const auto&) {
            entered_promise.set_value();

            gate.Wait();

            if (request.IsCancelled()) {
                return bridge::ResultEnvelope::Cancelled(
                    request
                );
            }

            return Success(request);
        },
        Config(1)
    );

    auto handle = Submit(
        pool,
        MakeRequest(601, cancellation.Token())
    );

    Require(
        entered.wait_for(5s) ==
            std::future_status::ready,
        "Task did not enter executor"
    );

    cancellation.Cancel();
    gate.Release();

    RequireReady(handle);

    Require(
        handle.Get().IsCancelled(),
        "Running executor must observe caller cancellation"
    );
}

// ============================================================
// Deadlines
// ============================================================

LAMP_TEST(RejectsExpiredRequest) {
    execution::V8IsolateWorkerPool pool(
        Success,
        Config(1)
    );

    auto request = MakeRequest(700);

    request.deadline =
        bridge::RequestDeadline::After(-1ms);

    execution::AsyncTaskHandle handle;

    const auto result = pool.Submit(
        std::move(request),
        {},
        handle
    );

    Require(
        result.status ==
            execution::AsyncSubmitStatus::DeadlineExceeded,
        "Expired request must be rejected"
    );
}

LAMP_TEST(QueuedTaskDeadlineExpires) {
    WorkerGate gate;

    std::promise<void> entered_promise;
    auto entered = entered_promise.get_future();

    execution::V8IsolateWorkerPool pool(
        [&](const auto& request, const auto&) {
            if (request.request_id == 800) {
                entered_promise.set_value();
                gate.Wait();
            }

            return Success(request);
        },
        Config(1)
    );

    auto active = Submit(pool, MakeRequest(800));

    Require(
        entered.wait_for(5s) ==
            std::future_status::ready,
        "Blocking task did not start"
    );

    auto request = MakeRequest(801);

    request.deadline =
        bridge::RequestDeadline::After(25ms);

    auto queued = Submit(pool, std::move(request));

    std::this_thread::sleep_for(50ms);

    gate.Release();

    RequireReady(active);
    RequireReady(queued);

    Require(
        queued.Get().IsDeadlineExceeded(),
        "Expired queued task must report deadline exceeded"
    );
}

// ============================================================
// Shutdown
// ============================================================

LAMP_TEST(ShutdownDrainsAcceptedTasks) {
    std::atomic<std::size_t> executed{0};

    execution::V8IsolateWorkerPool pool(
        [&](const auto& request, const auto&) {
            ++executed;
            return Success(request);
        },
        Config(2, 32)
    );

    std::vector<execution::AsyncTaskHandle> handles;

    for (std::size_t i = 0; i < 16; ++i) {
        handles.push_back(
            Submit(pool, MakeRequest(i + 900))
        );
    }

    pool.Shutdown();

    Require(
        !pool.IsRunning(),
        "Pool must stop after shutdown"
    );

    for (const auto& handle : handles) {
        RequireReady(handle);

        Require(
            handle.Get().IsSuccess(),
            "Accepted task must finish during draining"
        );
    }

    Require(
        executed.load() == handles.size(),
        "Shutdown must drain every accepted task"
    );

    Require(
        pool.PendingTasks() == 0,
        "No pending tasks may remain after shutdown"
    );

    Require(
        pool.ActiveTasks() == 0,
        "No active tasks may remain after shutdown"
    );
}

LAMP_TEST(RejectsSubmissionAfterShutdown) {
    execution::V8IsolateWorkerPool pool(
        Success,
        Config(1)
    );

    pool.Shutdown();

    execution::AsyncTaskHandle handle;

    const auto result = pool.Submit(
        MakeRequest(1000),
        {},
        handle
    );

    Require(
        result.status ==
            execution::AsyncSubmitStatus::SchedulerStopped,
        "Stopped scheduler must reject new tasks"
    );

    Require(
        !handle.Valid(),
        "Rejected submission must not return a handle"
    );
}

LAMP_TEST(ShutdownIsIdempotent) {
    execution::V8IsolateWorkerPool pool(
        Success,
        Config(1)
    );

    pool.Shutdown();
    pool.Shutdown();

    Require(
        !pool.IsRunning(),
        "Repeated shutdown must leave pool stopped"
    );
}

// ============================================================
// Test runner
// ============================================================

} // namespace

int main() {
    std::size_t passed = 0;
    std::size_t failed = 0;

    try {
        V8Environment environment;

        std::cout
            << "\n========================================\n"
            << " Lamp Phase 5 - V8 Worker Pool Tests\n"
            << "========================================\n\n";

        for (const auto& test : Tests()) {
            try {
                test.function();

                ++passed;

                std::cout
                    << "[PASS] " << test.name << '\n';

            } catch (const std::exception& error) {
                ++failed;

                std::cerr
                    << "[FAIL] " << test.name
                    << ": " << error.what() << '\n';

            } catch (...) {
                ++failed;

                std::cerr
                    << "[FAIL] " << test.name
                    << ": unknown exception\n";
            }
        }

    } catch (const std::exception& error) {
        std::cerr
            << "[FATAL] V8 test initialization: "
            << error.what() << '\n';

        return 1;
    }

    std::cout
        << "\n----------------------------------------\n"
        << "Passed: " << passed << '\n'
        << "Failed: " << failed << '\n'
        << "Total:  " << Tests().size() << '\n'
        << "----------------------------------------\n";

    return failed == 0 ? 0 : 1;
}
