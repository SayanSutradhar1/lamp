// Lamp Phase 5: end-to-end V8 worker pool / Promise integration tests.
// Standalone C++20 test executable. Link lamp::execution and lamp::v8,
// and register with CTest as lamp_v8_isolate_worker_pool_promise_integration.
#include "execution/v8_isolate_worker_pool.hpp"

#include <libplatform/libplatform.h>
#include <v8.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
using namespace lamp;
using namespace lamp::execution;
using namespace std::chrono_literals;

void Check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

struct V8Runtime final {
    std::unique_ptr<v8::Platform> platform;
    V8Runtime() {
        platform = v8::platform::NewDefaultPlatform();
        v8::V8::InitializePlatform(platform.get());
        Check(
            v8::V8::Initialize(),
            "V8 initialization failed"
        );
    }
    ~V8Runtime() {
        v8::V8::Dispose();
        v8::V8::DisposePlatform();
    }
};

bridge::RequestEnvelope Request(std::uint64_t id) {
    bridge::RequestEnvelope request;
    request.request_id = id;
    request.operation_id = "test.promise";
    request.transport = bridge::TransportKind::Internal;
    request.format = bridge::SerializationFormat::Json;
    return request;
}

bridge::ResultEnvelope Success(const bridge::RequestEnvelope& request) {
    return bridge::ResultEnvelope::Success(request, {},
        bridge::SerializationFormat::Json, bridge::ExecutionBackend::JavaScript);
}

V8WorkerPromiseResultMapper Mapper() {
    return [](const bridge::RequestEnvelope& request,
              V8PromiseSettlementKind kind, v8::Local<v8::Value>) {
        if (kind == V8PromiseSettlementKind::Fulfilled) return Success(request);
        return bridge::ResultEnvelope::Failure(request,
            {bridge::OperationErrorCode::ExecutionFailed,
             "Promise rejected", "TEST_PROMISE_REJECTED", false},
            bridge::ExecutionBackend::JavaScript);
    };
}

// Modes: 0 resolved, 1 rejected, 2 unresolved, 3 immediate,
// 4 chained microtask, 5 executor exception.
V8WorkerAsyncOperationExecutor Executor(int mode,
                                        std::atomic<int>* invocations = nullptr) {
    return [mode, invocations](const bridge::RequestEnvelope& request,
                               const V8WorkerExecutionContext& worker)
        -> V8WorkerAsyncResult {
        if (invocations) ++*invocations;
        Check(worker.isolate != nullptr, "worker isolate missing");
        Check(!worker.context.IsEmpty(), "worker context missing");
        if (mode == 5) throw std::runtime_error("deliberate executor error");
        if (mode == 3) return V8WorkerAsyncResult::Completed(Success(request));
        auto resolver = v8::Promise::Resolver::New(worker.context).ToLocalChecked();
        if (mode == 0) {
            Check(resolver->Resolve(worker.context,
                v8::Integer::New(worker.isolate, 42)).FromMaybe(false),
                "Promise resolve failed");
        } else if (mode == 1) {
            Check(resolver->Reject(worker.context,
                v8::String::NewFromUtf8Literal(worker.isolate, "rejected"))
                .FromMaybe(false), "Promise reject failed");
        } else if (mode == 4) {
            // A then-chain creates a Promise whose completion depends on a
            // microtask checkpoint rather than on immediate resolver state.
            auto source = resolver->GetPromise();
            auto identity = v8::Function::New(worker.context,
                [](const v8::FunctionCallbackInfo<v8::Value>& args) {
                    args.GetReturnValue().Set(args[0]);
                }).ToLocalChecked();
            auto chained = source->Then(worker.context, identity).ToLocalChecked();
            Check(resolver->Resolve(worker.context,
                v8::Integer::New(worker.isolate, 7)).FromMaybe(false),
                "source Promise resolve failed");
            return V8WorkerAsyncResult::Pending(chained);
        }
        return V8WorkerAsyncResult::Pending(resolver->GetPromise());
    };
}

V8IsolateWorkerPoolConfig PoolConfig() {
    V8IsolateWorkerPoolConfig config;
    config.max_heap_size_mb = 128;
    config.worker_count = 1;
    config.max_pending_tasks = 16;
    config.drain_on_shutdown = true;
    return config;
}

V8WorkerAsyncExecutionConfig AsyncConfig(std::size_t capacity = 16) {
    V8WorkerAsyncExecutionConfig config;
    config.max_pending_continuations_per_worker = capacity;
    config.microtask_checkpoints_per_pump = 1;
    config.idle_pump_interval = 2ms;
    return config;
}

AsyncTaskHandle Submit(V8IsolateWorkerPool& pool, std::uint64_t id,
                       bridge::RequestEnvelope request = {}) {
    if (request.request_id == 0) request = Request(id);
    AsyncTaskHandle handle;
    const auto submitted = pool.Submit(std::move(request), {}, handle);
    Check(submitted.Accepted(), "task submission rejected");
    Check(handle.Valid(), "accepted task has invalid handle");
    return handle;
}

bridge::ResultEnvelope Await(const AsyncTaskHandle& handle) {
    Check(handle.WaitFor(3000ms), "task completion timed out");
    return handle.Get();
}

void TestFulfillment() {
    std::cerr << "[FULFILLMENT] Creating worker pool" << std::endl;

    V8IsolateWorkerPool pool(
        Executor(0),
        Mapper(),
        PoolConfig(),
        AsyncConfig()
    );

    std::cerr << "[FULFILLMENT] Worker pool created" << std::endl;

    auto handle = Submit(pool, 1);

    std::cerr << "[FULFILLMENT] Task submitted" << std::endl;

    auto result = Await(handle);

    std::cerr << "[FULFILLMENT] Promise completed" << std::endl;

    Check(
        result.outcome == bridge::ResultOutcome::Success,
        "fulfilled Promise did not succeed"
    );

    Check(
        pool.ActiveTasks() == 0,
        "active count leaked after fulfillment"
    );

    std::cerr << "[FULFILLMENT] All assertions passed" << std::endl;
}

void TestRejection() {
    V8IsolateWorkerPool pool(Executor(1), Mapper(), PoolConfig(), AsyncConfig());
    auto handle = Submit(pool, 2);
    const auto result = Await(handle);
    Check(result.outcome == bridge::ResultOutcome::Failure,
          "rejected Promise did not fail");
    Check(result.error.identifier == "TEST_PROMISE_REJECTED",
          "rejection mapper was not invoked");
}

void TestMicrotaskChain() {
    V8IsolateWorkerPool pool(Executor(4), Mapper(), PoolConfig(), AsyncConfig());
    auto handle = Submit(pool, 3);
    Check(Await(handle).IsSuccess(), "microtask Promise chain did not complete");
}

void TestImmediate() {
    V8IsolateWorkerPool pool(Executor(3), Mapper(), PoolConfig(), AsyncConfig());
    auto handle = Submit(pool, 4);
    Check(Await(handle).IsSuccess(), "immediate async executor result failed");
}

void TestExecutorFailure() {
    V8IsolateWorkerPool pool(Executor(5), Mapper(), PoolConfig(), AsyncConfig());
    auto handle = Submit(pool, 5);
    Check(Await(handle).outcome == bridge::ResultOutcome::Failure,
          "executor exception was not converted to failure");
}

void TestCancellation() {
    V8IsolateWorkerPool pool(Executor(2), Mapper(), PoolConfig(), AsyncConfig());
    auto handle = Submit(pool, 6);
    Check(handle.Cancel(), "cancellation request failed");
    Check(Await(handle).outcome == bridge::ResultOutcome::Cancelled,
          "pending Promise cancellation did not complete");
    Check(pool.ActiveTasks() == 0, "active count leaked after cancellation");
}

void TestDeadline() {
    V8IsolateWorkerPool pool(Executor(2), Mapper(), PoolConfig(), AsyncConfig());
    auto request = Request(7);
    request.deadline = bridge::RequestDeadline::After(100ms);
    auto handle = Submit(pool, 7, std::move(request));
    Check(Await(handle).outcome == bridge::ResultOutcome::DeadlineExceeded,
          "pending Promise did not respect request deadline");
}

void TestCapacityAndForwardProgress() {
    std::atomic<int> invoked{0};
    V8IsolateWorkerPool pool(Executor(2, &invoked), Mapper(),
                             PoolConfig(), AsyncConfig(1));
    auto first = Submit(pool, 8);
    auto second = Submit(pool, 9);
    const auto rejected = Await(second);
    Check(rejected.outcome == bridge::ResultOutcome::Failure,
          "continuation capacity was not enforced");
    Check(rejected.error.code == bridge::OperationErrorCode::ResourceExhausted,
          "continuation capacity returned wrong error");
    Check(first.Cancel(), "first continuation cancellation failed");
    Check(Await(first).IsCancelled(), "first continuation did not cancel");
    Check(invoked.load() == 2, "worker did not execute both requests");
}

void TestShutdownAbandonsUnresolved() {
    V8IsolateWorkerPool pool(Executor(2), Mapper(), PoolConfig(), AsyncConfig());
    auto handle = Submit(pool, 10);
    // Wait until the Promise is registered (active count includes pending).
    const auto until = std::chrono::steady_clock::now() + 2s;
    while (pool.ActiveTasks() == 0 && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(1ms);
    Check(pool.ActiveTasks() == 1, "pending Promise was not counted active");
    pool.Shutdown();
    const auto result = Await(handle);
    Check(result.outcome == bridge::ResultOutcome::Failure,
          "unresolved Promise was not abandoned at shutdown");
    Check(pool.ActiveTasks() == 0, "shutdown leaked active tasks");
    Check(!pool.IsRunning(), "pool remained running after shutdown");
}

void TestSynchronousCompatibility() {
    V8WorkerOperationExecutor sync = [](const bridge::RequestEnvelope& request,
                                        const V8WorkerExecutionContext&) {
        return Success(request);
    };
    V8IsolateWorkerPool pool(std::move(sync), PoolConfig());
    auto handle = Submit(pool, 11);
    Check(Await(handle).IsSuccess(), "legacy synchronous path regressed");
}

} // namespace

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    try {
        // Initialize V8 before creating any worker pool.
        // Keep the runtime alive until all tests and isolates are destroyed.
        V8Runtime runtime;
        #define RUN_TEST(fn)                                      \
            do {                                                  \
                std::cerr << "[START] " #fn << std::endl;         \
                fn();                                             \
                std::cerr << "[PASS]  " #fn << std::endl;         \
            } while (false)

        RUN_TEST(TestFulfillment);
        RUN_TEST(TestRejection);
        RUN_TEST(TestMicrotaskChain);
        RUN_TEST(TestImmediate);
        RUN_TEST(TestExecutorFailure);
        RUN_TEST(TestCancellation);
        RUN_TEST(TestDeadline);
        RUN_TEST(TestCapacityAndForwardProgress);
        RUN_TEST(TestShutdownAbandonsUnresolved);
        RUN_TEST(TestSynchronousCompatibility);

        #undef RUN_TEST
        std::cout << "Lamp V8 Promise worker integration: all tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Lamp V8 Promise worker integration FAILED: "
                  << error.what() << '\n';
        return 1;
    }
}
