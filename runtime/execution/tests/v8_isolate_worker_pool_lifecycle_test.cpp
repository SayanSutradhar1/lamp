// File: runtime/execution/tests/v8_isolate_worker_pool_lifecycle_test.cpp
// Lamp Phase 5 — V8 worker lifecycle regression tests (C++20).
#include "execution/v8_isolate_worker_pool.hpp"
#include <libplatform/libplatform.h>
#include <v8.h>

#include <atomic>
#include <chrono>
#include <exception>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;
namespace bridge = lamp::bridge;
namespace execution = lamp::execution;

void Require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

class V8Environment final {
public:
    V8Environment() : platform_(::v8::platform::NewDefaultPlatform()) {
        ::v8::V8::InitializePlatform(platform_.get());
        if (!::v8::V8::Initialize())
            throw std::runtime_error("V8 initialization failed");
    }
    ~V8Environment() {
        ::v8::V8::Dispose();
        ::v8::V8::DisposePlatform();
    }
private:
    std::unique_ptr<::v8::Platform> platform_;
};

struct Gate final {
    Gate() : released(finished.get_future().share()) {}
    void Wait() const { released.wait(); }
    void Release() { std::call_once(once, [this] { finished.set_value(); }); }
    ~Gate() { Release(); }
private:
    std::promise<void> finished;
    std::shared_future<void> released;
    std::once_flag once;
};

bridge::RequestEnvelope Request(bridge::RequestId id) {
    bridge::RequestEnvelope request;
    request.request_id = id;
    request.operation_id = "tests.v8.lifecycle";
    request.transport = bridge::TransportKind::Internal;
    request.format = bridge::SerializationFormat::Json;
    return request;
}

bridge::ResultEnvelope Success(const bridge::RequestEnvelope& request,
                               const execution::V8WorkerExecutionContext&) {
    return bridge::ResultEnvelope::Success(
        request, {}, bridge::SerializationFormat::Json,
        bridge::ExecutionBackend::JavaScript);
}

execution::V8IsolateWorkerPoolConfig Config(bool drain = true) {
    execution::V8IsolateWorkerPoolConfig config;
    config.worker_count = 1;
    config.max_pending_tasks = 32;
    config.drain_on_shutdown = drain;
    return config;
}

execution::AsyncTaskHandle Submit(execution::V8IsolateWorkerPool& pool,
                                  bridge::RequestId id) {
    execution::AsyncTaskHandle handle;
    const auto status = pool.Submit(Request(id), {}, handle);
    Require(status.Accepted() && handle.Valid(), "Task submission failed");
    return handle;
}

void Ready(const execution::AsyncTaskHandle& handle) {
    Require(handle.WaitFor(5s), "Task completion timed out");
}

void NonDrainingShutdownCancelsPending() {
    Gate gate;
    std::promise<void> entered;
    auto started = entered.get_future();
    std::atomic<int> executed{0};
    execution::V8IsolateWorkerPool pool(
        [&](const auto& request, const auto& worker) {
            ++executed;
            if (request.request_id == 1) {
                entered.set_value();
                gate.Wait();
            }
            return Success(request, worker);
        }, Config(false));
    auto running = Submit(pool, 1);
    Require(started.wait_for(5s) == std::future_status::ready,
            "Running task never started");
    auto queued1 = Submit(pool, 2);
    auto queued2 = Submit(pool, 3);
    std::thread shutdown([&] { pool.Shutdown(); });
    // Pending cancellation must be published before the running task exits.
    const bool cancelled_in_time = queued1.WaitFor(5s) && queued2.WaitFor(5s);
    gate.Release();
    shutdown.join();
    Require(cancelled_in_time, "Pending tasks were not completed on shutdown");
    Ready(running);
    Require(running.Get().IsSuccess(), "Running task must finish successfully");
    Require(queued1.Get().IsCancelled() && queued2.Get().IsCancelled(),
            "Non-draining shutdown must cancel queued tasks");
    Require(executed.load() == 1, "Queued executors must not run");
    Require(pool.PendingTasks() == 0 && pool.ActiveTasks() == 0,
            "Shutdown must leave no outstanding work");
}

void DrainingShutdownExecutesPending() {
    std::atomic<int> executed{0};
    execution::V8IsolateWorkerPool pool(
        [&](const auto& request, const auto& worker) {
            ++executed;
            return Success(request, worker);
        }, Config(true));
    std::vector<execution::AsyncTaskHandle> handles;
    for (int i = 0; i < 20; ++i) handles.push_back(Submit(pool, 100 + i));
    pool.Shutdown();
    for (const auto& handle : handles) {
        Ready(handle);
        Require(handle.Get().IsSuccess(), "Draining shutdown lost a task");
    }
    Require(executed.load() == 20, "Every accepted task must execute");
}

void WorkerThreadShutdownIsRejected() {
    std::promise<void> attempted;
    auto result = attempted.get_future();
    execution::V8IsolateWorkerPool* pool_address = nullptr;
    execution::V8IsolateWorkerPool pool(
        [&](const auto& request, const auto& worker) {
            bool rejected = false;
            try { pool_address->Shutdown(); }
            catch (const std::logic_error&) { rejected = true; }
            attempted.set_value();
            if (!rejected)
                throw std::runtime_error("Worker-thread shutdown was not rejected");
            return Success(request, worker);
        }, Config());
    pool_address = &pool;
    auto handle = Submit(pool, 200);
    Require(result.wait_for(5s) == std::future_status::ready,
            "Worker shutdown attempt did not finish");
    Ready(handle);
    Require(handle.Get().IsSuccess(), "Worker-thread shutdown must be rejected");
    Require(pool.IsRunning(), "Rejected shutdown must leave scheduler running");
    pool.Shutdown();
}

void ConcurrentShutdownIsIdempotent() {
    execution::V8IsolateWorkerPool pool(Success, Config());
    std::vector<execution::AsyncTaskHandle> handles;
    for (int i = 0; i < 12; ++i) handles.push_back(Submit(pool, 300 + i));
    std::thread first([&] { pool.Shutdown(); });
    std::thread second([&] { pool.Shutdown(); });
    first.join();
    second.join();
    pool.Shutdown();
    Require(!pool.IsRunning(), "Scheduler must remain stopped");
    for (const auto& handle : handles) {
        Ready(handle);
        Require(handle.Get().IsSuccess(), "Concurrent shutdown lost a task");
    }
}

} // namespace

int main() {
    try {
        V8Environment environment;
        struct Test { const char* name; void (*run)(); };
        const Test tests[] = {
            {"NonDrainingShutdownCancelsPending", NonDrainingShutdownCancelsPending},
            {"DrainingShutdownExecutesPending", DrainingShutdownExecutesPending},
            {"WorkerThreadShutdownIsRejected", WorkerThreadShutdownIsRejected},
            {"ConcurrentShutdownIsIdempotent", ConcurrentShutdownIsIdempotent},
        };
        int failed = 0;
        for (const auto& test : tests) {
            try { test.run(); std::cout << "[PASS] " << test.name << '\n'; }
            catch (const std::exception& error) {
                ++failed;
                std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n';
            }
        }
        std::cout << "Lifecycle tests: " << (4 - failed) << "/4 passed\n";
        return failed == 0 ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "[FATAL] " << error.what() << '\n';
        return 1;
    }
}
