// Lamp Framework - Phase 5: AsyncExecutionDispatcher regression tests (C++20)
// File: runtime/execution/tests/async_execution_dispatcher_test.cpp
#include "execution/async_execution_dispatcher.hpp"

#include <atomic>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace bridge = lamp::bridge;
namespace execution = lamp::execution;

namespace {

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

bridge::RequestEnvelope MakeRequest(std::uint64_t id) {
    bridge::RequestEnvelope request;
    request.request_id = id;
    request.operation_id = "tests.dispatch";
    request.transport = bridge::TransportKind::Internal;
    request.format = bridge::SerializationFormat::Json;
    return request;
}

class MockScheduler final : public execution::IAsyncExecutionScheduler {
public:
    explicit MockScheduler(execution::AsyncSubmitStatus status =
        execution::AsyncSubmitStatus::Accepted) : status_(status) {}

    execution::AsyncSubmitResult Submit(
        bridge::RequestEnvelope request,
        execution::AsyncScheduleOptions options,
        execution::AsyncTaskHandle& output) override {
        output = execution::AsyncTaskHandle{};
        std::lock_guard lock(mutex_);
        ++calls_;
        last_target_ = options.target;
        last_request_id_ = request.request_id;
        if (stopped_) {
            return execution::AsyncSubmitResult::Failure(
                execution::AsyncSubmitStatus::SchedulerStopped, "stopped");
        }
        if (status_ == execution::AsyncSubmitStatus::Accepted) {
            return execution::AsyncSubmitResult::Success();
        }
        return execution::AsyncSubmitResult::Failure(status_, "mock rejection");
    }

    bool IsRunning() const noexcept override {
        std::lock_guard lock(mutex_);
        return !stopped_;
    }
    std::size_t PendingTasks() const noexcept override { return pending_; }
    std::size_t ActiveTasks() const noexcept override { return active_; }
    void Shutdown() override {
        std::lock_guard lock(mutex_);
        ++shutdown_calls_;
        stopped_ = true;
    }
    void SetStatus(execution::AsyncSubmitStatus status) {
        std::lock_guard lock(mutex_);
        status_ = status;
    }
    std::size_t Calls() const {
        std::lock_guard lock(mutex_);
        return calls_;
    }
    std::size_t ShutdownCalls() const {
        std::lock_guard lock(mutex_);
        return shutdown_calls_;
    }
    execution::TaskExecutionTarget LastTarget() const {
        std::lock_guard lock(mutex_);
        return last_target_;
    }
    bridge::RequestId LastRequestId() const {
        std::lock_guard lock(mutex_);
        return last_request_id_;
    }
    std::size_t pending_ = 0;
    std::size_t active_ = 0;
private:
    mutable std::mutex mutex_;
    execution::AsyncSubmitStatus status_;
    execution::TaskExecutionTarget last_target_ =
        execution::TaskExecutionTarget::Automatic;
    bridge::RequestId last_request_id_{};
    std::size_t calls_ = 0;
    std::size_t shutdown_calls_ = 0;
    bool stopped_ = false;
};

using Dispatcher = execution::AsyncExecutionDispatcher;
using Target = execution::TaskExecutionTarget;
using Status = execution::AsyncSubmitStatus;

execution::AsyncSubmitResult Send(Dispatcher& dispatcher, std::uint64_t id,
                                  Target target = Target::Automatic) {
    execution::AsyncScheduleOptions options;
    options.target = target;
    execution::AsyncTaskHandle handle;
    return dispatcher.Submit(MakeRequest(id), options, handle);
}

void TestConstructorValidation() {
    bool threw = false;
    try { Dispatcher dispatcher(nullptr, nullptr); }
    catch (const std::invalid_argument&) { threw = true; }
    Require(threw, "two missing backends must be rejected");

    auto native = std::make_shared<MockScheduler>();
    threw = false;
    try { Dispatcher dispatcher(native, native); }
    catch (const std::invalid_argument&) { threw = true; }
    Require(threw, "identical backends must be rejected");

    execution::AsyncExecutionDispatcherConfig config;
    config.automatic_target = Target::Automatic;
    threw = false;
    try { Dispatcher dispatcher(native, nullptr, config); }
    catch (const std::invalid_argument&) { threw = true; }
    Require(threw, "automatic default must be rejected");
}

void TestExplicitRouting() {
    auto native = std::make_shared<MockScheduler>();
    auto js = std::make_shared<MockScheduler>();
    Dispatcher dispatcher(native, js);
    Require(Send(dispatcher, 101, Target::NativeWorker).Accepted(), "native dispatch");
    Require(Send(dispatcher, 102, Target::JavaScriptWorker).Accepted(), "JS dispatch");
    Require(native->Calls() == 1 && js->Calls() == 1, "explicit routing counts");
    Require(native->LastTarget() == Target::NativeWorker, "native target propagation");
    Require(js->LastTarget() == Target::JavaScriptWorker, "JS target propagation");
}

void TestAutomaticDefault() {
    auto native = std::make_shared<MockScheduler>();
    auto js = std::make_shared<MockScheduler>();
    execution::AsyncExecutionDispatcherConfig config;
    config.automatic_target = Target::JavaScriptWorker;
    Dispatcher dispatcher(native, js, config);
    Require(Send(dispatcher, 103).Accepted(), "automatic dispatch");
    Require(native->Calls() == 0 && js->Calls() == 1, "automatic preference");
}

void TestMissingBackend() {
    auto native = std::make_shared<MockScheduler>();
    Dispatcher dispatcher(native, nullptr);
    Require(Send(dispatcher, 104, Target::JavaScriptWorker).status ==
        Status::BackendUnavailable, "missing explicit backend");
    Require(native->Calls() == 0, "must not reroute explicit target");
}

void TestFallbackOnUnavailable() {
    auto native = std::make_shared<MockScheduler>(Status::BackendUnavailable);
    auto js = std::make_shared<MockScheduler>();
    execution::AsyncExecutionDispatcherConfig config;
    config.fallback_on_backend_unavailable = true;
    Dispatcher dispatcher(native, js, config);
    Require(Send(dispatcher, 105).Accepted(), "fallback should succeed");
    Require(native->Calls() == 1 && js->Calls() == 1, "fallback calls");
    Require(js->LastTarget() == Target::JavaScriptWorker, "fallback target");
    Require(js->LastRequestId() == bridge::RequestId{105}, "request preserved");
}

void TestNoFallbackForQueueFull() {
    auto native = std::make_shared<MockScheduler>(Status::QueueFull);
    auto js = std::make_shared<MockScheduler>();
    execution::AsyncExecutionDispatcherConfig config;
    config.fallback_on_backend_unavailable = true;
    Dispatcher dispatcher(native, js, config);
    Require(Send(dispatcher, 106).status == Status::QueueFull, "queue-full status");
    Require(js->Calls() == 0, "queue-full must not retry");
}

void TestNoFallbackForExplicitTarget() {
    auto native = std::make_shared<MockScheduler>(Status::BackendUnavailable);
    auto js = std::make_shared<MockScheduler>();
    execution::AsyncExecutionDispatcherConfig config;
    config.fallback_on_backend_unavailable = true;
    Dispatcher dispatcher(native, js, config);
    Require(Send(dispatcher, 107, Target::NativeWorker).status ==
        Status::BackendUnavailable, "explicit rejection preserved");
    Require(js->Calls() == 0, "explicit request must not fallback");
}

void TestFallbackWithoutSecondary() {
    auto native = std::make_shared<MockScheduler>(Status::BackendUnavailable);
    execution::AsyncExecutionDispatcherConfig config;
    config.fallback_on_backend_unavailable = true;
    Dispatcher dispatcher(native, nullptr, config);
    Require(Send(dispatcher, 108).status == Status::BackendUnavailable,
        "original rejection preserved without backup");
}

void TestAggregatedCounters() {
    auto native = std::make_shared<MockScheduler>();
    auto js = std::make_shared<MockScheduler>();
    native->pending_ = 3;
    native->active_ = 2;
    js->pending_ = 5;
    js->active_ = 4;
    Dispatcher dispatcher(native, js);
    Require(dispatcher.PendingTasks() == 8, "pending sum");
    Require(dispatcher.ActiveTasks() == 6, "active sum");
    Require(dispatcher.IsRunning(), "running state");
}

void TestShutdownBackends() {
    auto native = std::make_shared<MockScheduler>();
    auto js = std::make_shared<MockScheduler>();
    Dispatcher dispatcher(native, js);
    dispatcher.Shutdown();
    dispatcher.Shutdown();
    Require(native->ShutdownCalls() == 1 && js->ShutdownCalls() == 1,
        "backend shutdown exactly once");
    Require(!dispatcher.IsRunning(), "dispatcher stopped");
    Require(Send(dispatcher, 109).status == Status::SchedulerStopped,
        "post-shutdown submissions rejected");
}

void TestKeepSharedBackendsRunning() {
    auto native = std::make_shared<MockScheduler>();
    auto js = std::make_shared<MockScheduler>();
    execution::AsyncExecutionDispatcherConfig config;
    config.shutdown_backends = false;
    Dispatcher dispatcher(native, js, config);
    dispatcher.Shutdown();
    Require(native->IsRunning() && js->IsRunning(), "shared backends preserved");
    Require(native->ShutdownCalls() == 0 && js->ShutdownCalls() == 0,
        "shared backends not shut down");
}

void TestConcurrentSubmissions() {
    auto native = std::make_shared<MockScheduler>();
    auto js = std::make_shared<MockScheduler>();
    Dispatcher dispatcher(native, js);
    constexpr int threads = 8;
    constexpr int per_thread = 100;
    std::atomic<int> failures{0};
    std::vector<std::thread> workers;
    for (int i = 0; i < threads; ++i) {
        workers.emplace_back([&, i] {
            for (int j = 0; j < per_thread; ++j) {
                const auto id = static_cast<std::uint64_t>(i * per_thread + j + 1000);
                const auto target = j % 2 == 0 ? Target::NativeWorker :
                    Target::JavaScriptWorker;
                if (!Send(dispatcher, id, target).Accepted()) ++failures;
            }
        });
    }
    for (auto& worker : workers) worker.join();
    Require(failures == 0, "concurrent submissions must succeed");
    Require(native->Calls() == 400 && js->Calls() == 400,
        "concurrent routing counts");
}

void TestConcurrentShutdown() {
    auto native = std::make_shared<MockScheduler>();
    auto js = std::make_shared<MockScheduler>();
    Dispatcher dispatcher(native, js);
    std::vector<std::thread> workers;
    for (int i = 0; i < 8; ++i) {
        workers.emplace_back([&] { dispatcher.Shutdown(); });
    }
    for (auto& worker : workers) worker.join();
    Require(native->ShutdownCalls() == 1 && js->ShutdownCalls() == 1,
        "concurrent shutdown must be idempotent");
}

} // namespace

int main() {
    struct Case { const char* name; void (*run)(); };
    const Case tests[] = {
        {"constructor validation", TestConstructorValidation},
        {"explicit routing", TestExplicitRouting},
        {"automatic default", TestAutomaticDefault},
        {"missing backend", TestMissingBackend},
        {"fallback on unavailable", TestFallbackOnUnavailable},
        {"no fallback on queue full", TestNoFallbackForQueueFull},
        {"no fallback for explicit target", TestNoFallbackForExplicitTarget},
        {"fallback without secondary", TestFallbackWithoutSecondary},
        {"aggregated counters", TestAggregatedCounters},
        {"shutdown backends", TestShutdownBackends},
        {"shared backend lifetime", TestKeepSharedBackendsRunning},
        {"concurrent submissions", TestConcurrentSubmissions},
        {"concurrent shutdown", TestConcurrentShutdown},
    };
    int failed = 0;
    for (const auto& test : tests) {
        try {
            test.run();
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failed;
            std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n';
        } catch (...) {
            ++failed;
            std::cerr << "[FAIL] " << test.name << ": unknown exception\n";
        }
    }
    std::cout << (sizeof(tests) / sizeof(tests[0]) - failed)
              << "/" << (sizeof(tests) / sizeof(tests[0]))
              << " tests passed\n";
    return failed == 0 ? 0 : 1;
}
