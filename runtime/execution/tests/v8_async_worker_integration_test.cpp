// Lamp Framework - Phase 5
// File: runtime/execution/tests/v8_async_worker_integration_test.cpp
// C++20 regression tests for the worker-local Promise -> task completion adapter.

#include "execution/v8_async_worker_integration.hpp"

#include <libplatform/libplatform.h>
#include <v8.h>

#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
namespace bridge = lamp::bridge;
namespace execution = lamp::execution;
using namespace std::chrono_literals;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
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

class IsolateFixture final {
public:
    IsolateFixture()
        : allocator_(::v8::ArrayBuffer::Allocator::NewDefaultAllocator()) {
        ::v8::Isolate::CreateParams params;
        params.array_buffer_allocator = allocator_.get();
        isolate_ = ::v8::Isolate::New(params);
        Require(isolate_ != nullptr, "V8 isolate creation failed");
    }
    ~IsolateFixture() { isolate_->Dispose(); }
    ::v8::Isolate* Get() const noexcept { return isolate_; }
private:
    std::unique_ptr<::v8::ArrayBuffer::Allocator> allocator_;
    ::v8::Isolate* isolate_ = nullptr;
};

template <class F>
void WithContext(F&& f) {
    IsolateFixture fixture;
    ::v8::Isolate::Scope isolate_scope(fixture.Get());
    ::v8::HandleScope handle_scope(fixture.Get());
    auto context = ::v8::Context::New(fixture.Get());
    Require(!context.IsEmpty(), "V8 context creation failed");
    ::v8::Context::Scope context_scope(context);
    f(fixture.Get(), context);
}

using Completion = std::shared_ptr<execution::AsyncTaskCompletion>;
Completion MakeRunningCompletion(bridge::RequestId id) {
    bridge::RequestEnvelope request;
    request.request_id = id;
    request.operation_id = "tests.v8.async";
    request.transport = bridge::TransportKind::Internal;
    request.format = bridge::SerializationFormat::Json;
    auto completion = std::make_shared<execution::AsyncTaskCompletion>(
        std::move(request));
    Require(completion->MarkQueued(), "Could not mark completion queued");
    Require(completion->MarkRunning(), "Could not mark completion running");
    return completion;
}

bridge::ResultEnvelope Success(const bridge::RequestEnvelope& request) {
    return bridge::ResultEnvelope::Success(
        request, {}, bridge::SerializationFormat::Json,
        bridge::ExecutionBackend::JavaScript);
}

using Kind = execution::V8PromiseSettlementKind;
using Mapper = execution::V8AsyncResultMapper;

Mapper MappingToSuccess(std::vector<Kind>& kinds) {
    return [&kinds](const bridge::RequestEnvelope& request,
                    Kind kind, ::v8::Local<::v8::Value>) {
        kinds.push_back(kind);
        return Success(request);
    };
}

using Test = void (*)();
struct TestCase { const char* name; Test run; };
std::vector<TestCase>& Tests() {
    static std::vector<TestCase> tests;
    return tests;
}
struct RegisterTest {
    RegisterTest(const char* name, Test test) { Tests().push_back({name, test}); }
};
#define LAMP_TEST(name) \
    void name(); \
    const RegisterTest register_##name(#name, &name); \
    void name()

LAMP_TEST(RejectsMissingMapper) {
    WithContext([](::v8::Isolate* isolate, auto) {
        bool rejected = false;
        try { execution::V8AsyncWorkerIntegration adapter(isolate, {}); }
        catch (const std::invalid_argument&) { rejected = true; }
        Require(rejected, "Empty mapper must be rejected");
    });
}

LAMP_TEST(EmptyPumpAndMissingCancellation) {
    WithContext([](::v8::Isolate* isolate, auto) {
        std::vector<Kind> kinds;
        execution::V8AsyncWorkerIntegration adapter(
            isolate, MappingToSuccess(kinds));
        Require(adapter.Empty(), "New adapter must be empty");
        Require(adapter.PendingCount() == 0, "New adapter must have zero pending");
        const auto result = adapter.Pump();
        Require(result.pending == 0 && result.settled == 0,
                "Empty pump must not settle tasks");
        Require(!adapter.Cancel(999), "Missing continuation cancellation succeeded");
        Require(adapter.AbandonAll() == 0, "Empty abandon count incorrect");
    });
}

LAMP_TEST(RejectsInvalidRegistration) {
    WithContext([](::v8::Isolate* isolate, ::v8::Local<::v8::Context> context) {
        std::vector<Kind> kinds;
        execution::V8AsyncWorkerIntegration adapter(
            isolate, MappingToSuccess(kinds));
        auto resolver = ::v8::Promise::Resolver::New(context).ToLocalChecked();
        auto completion = MakeRunningCompletion(100);
        Require(!adapter.Register(1, resolver->GetPromise(), {}),
                "Null completion must be rejected");
        Require(!adapter.Register(1, ::v8::Local<::v8::Promise>(), completion),
                "Empty promise must be rejected");
        Require(adapter.Empty(), "Rejected registration retained a Promise");
    });
}

LAMP_TEST(FulfillmentPublishesOnce) {
    WithContext([](::v8::Isolate* isolate, ::v8::Local<::v8::Context> context) {
        std::vector<Kind> kinds;
        execution::V8AsyncWorkerIntegration adapter(
            isolate, MappingToSuccess(kinds));
        auto resolver = ::v8::Promise::Resolver::New(context).ToLocalChecked();
        auto completion = MakeRunningCompletion(101);
        Require(adapter.Register(101, resolver->GetPromise(), completion),
                "Registration failed");
        Require(adapter.PendingCount() == 1, "Promise not tracked");
        Require(adapter.Pump().pending == 1, "Pending Promise settled early");
        Require(resolver->Resolve(context, ::v8::Integer::New(isolate, 42)).FromMaybe(false),
                "Promise resolution failed");
        const auto result = adapter.Pump();
        Require(result.settled == 1 && result.pending == 0,
                "Fulfilled Promise not settled");
        Require(kinds.size() == 1 && kinds.front() == Kind::Fulfilled,
                "Fulfilled mapper invocation incorrect");
        Require(adapter.Empty(), "Fulfilled continuation retained");
        (void)completion->Get();
        (void)adapter.Pump();
        Require(kinds.size() == 1, "Fulfillment published twice");
    });
}

LAMP_TEST(RejectionReachesMapper) {
    WithContext([](::v8::Isolate* isolate, ::v8::Local<::v8::Context> context) {
        std::vector<Kind> kinds;
        execution::V8AsyncWorkerIntegration adapter(
            isolate, MappingToSuccess(kinds));
        auto resolver = ::v8::Promise::Resolver::New(context).ToLocalChecked();
        auto completion = MakeRunningCompletion(102);
        Require(adapter.Register(102, resolver->GetPromise(), completion),
                "Registration failed");
        Require(resolver->Reject(context, ::v8::String::NewFromUtf8Literal(
                    isolate, "expected rejection")).FromMaybe(false),
                "Promise rejection failed");
        Require(adapter.Pump().settled == 1, "Rejected Promise not observed");
        Require(kinds.size() == 1 && kinds.front() == Kind::Rejected,
                "Rejection did not reach mapper");
        (void)completion->Get();
    });
}

LAMP_TEST(DuplicateAndCapacity) {
    WithContext([](::v8::Isolate* isolate, ::v8::Local<::v8::Context> context) {
        std::vector<Kind> kinds;
        execution::V8AsyncWorkerIntegration adapter(
            isolate, MappingToSuccess(kinds),
            {.max_pending_continuations = 1, .checkpoints_per_pump = 1});
        auto first = ::v8::Promise::Resolver::New(context).ToLocalChecked();
        auto second = ::v8::Promise::Resolver::New(context).ToLocalChecked();
        Require(adapter.Register(103, first->GetPromise(), MakeRunningCompletion(103)),
                "First registration failed");
        Require(!adapter.Register(103, second->GetPromise(), MakeRunningCompletion(104)),
                "Duplicate ID accepted");
        Require(!adapter.Register(104, second->GetPromise(), MakeRunningCompletion(105)),
                "Capacity exceeded");
        Require(adapter.PendingCount() == 1, "Rejected registration changed count");
        Require(adapter.AbandonAll() == 1, "Abandon count incorrect");
        Require(adapter.Empty(), "Abandon did not clear tracked continuations");
    });
}

LAMP_TEST(CancelPublishesTerminalResult) {
    WithContext([](::v8::Isolate* isolate, ::v8::Local<::v8::Context> context) {
        std::vector<Kind> kinds;
        execution::V8AsyncWorkerIntegration adapter(
            isolate, MappingToSuccess(kinds));
        auto resolver = ::v8::Promise::Resolver::New(context).ToLocalChecked();
        auto completion = MakeRunningCompletion(106);
        Require(adapter.Register(106, resolver->GetPromise(), completion),
                "Registration failed");
        Require(adapter.Cancel(106), "Cancellation failed");
        Require(adapter.Empty(), "Cancellation retained continuation");
        Require(kinds.empty(), "Cancellation incorrectly called value mapper");
        (void)completion->Get();
        Require(!adapter.Cancel(106), "Cancellation succeeded twice");
    });
}

LAMP_TEST(AbandonPublishesTerminalResult) {
    WithContext([](::v8::Isolate* isolate, ::v8::Local<::v8::Context> context) {
        std::vector<Kind> kinds;
        execution::V8AsyncWorkerIntegration adapter(
            isolate, MappingToSuccess(kinds));
        auto first = ::v8::Promise::Resolver::New(context).ToLocalChecked();
        auto second = ::v8::Promise::Resolver::New(context).ToLocalChecked();
        auto c1 = MakeRunningCompletion(107);
        auto c2 = MakeRunningCompletion(108);
        Require(adapter.Register(107, first->GetPromise(), c1), "First registration failed");
        Require(adapter.Register(108, second->GetPromise(), c2), "Second registration failed");
        Require(adapter.AbandonAll() == 2, "AbandonAll must report two tasks");
        Require(adapter.Empty(), "AbandonAll retained tasks");
        Require(kinds.empty(), "Abandon incorrectly called value mapper");
        (void)c1->Get();
        (void)c2->Get();
    });
}

LAMP_TEST(MapperExceptionBecomesFailure) {
    WithContext([](::v8::Isolate* isolate, ::v8::Local<::v8::Context> context) {
        int mapper_calls = 0;
        execution::V8AsyncWorkerIntegration adapter(
            isolate,
            [&mapper_calls](const bridge::RequestEnvelope&, Kind,
                            ::v8::Local<::v8::Value>) -> bridge::ResultEnvelope {
                ++mapper_calls;
                throw std::runtime_error("expected mapper failure");
            });
        auto resolver = ::v8::Promise::Resolver::New(context).ToLocalChecked();
        auto completion = MakeRunningCompletion(109);
        Require(adapter.Register(109, resolver->GetPromise(), completion),
                "Registration failed");
        Require(resolver->Resolve(context, ::v8::Undefined(isolate)).FromMaybe(false),
                "Resolution failed");
        Require(adapter.Pump().settled == 1, "Mapper exception blocked settlement");
        Require(mapper_calls == 1, "Mapper not invoked exactly once");
        Require(adapter.Empty(), "Failed mapping retained continuation");
        (void)completion->Get();
    });
}

LAMP_TEST(PromiseCheckpointLimit) {
    WithContext([](::v8::Isolate* isolate, ::v8::Local<::v8::Context> context) {
        std::vector<Kind> kinds;
        execution::V8AsyncWorkerIntegration adapter(
            isolate, MappingToSuccess(kinds));
        auto resolver = ::v8::Promise::Resolver::New(context).ToLocalChecked();
        auto completion = MakeRunningCompletion(110);
        execution::V8PromiseExecutionOptions options;
        options.max_microtask_checkpoints = 2;
        Require(adapter.Register(110, resolver->GetPromise(), completion, options),
                "Registration failed");
        Require(adapter.Pump().pending == 1, "First checkpoint must remain pending");
        Require(adapter.Pump().settled == 1, "Second checkpoint must expire Promise");
        Require(adapter.Empty(), "Expired continuation retained");
        Require(kinds.empty(), "Deadline incorrectly called value mapper");
        (void)completion->Get();
    });
}

LAMP_TEST(ForeignThreadAccessRejected) {
    WithContext([](::v8::Isolate* isolate, auto) {
        std::vector<Kind> kinds;
        execution::V8AsyncWorkerIntegration adapter(
            isolate, MappingToSuccess(kinds));
        bool rejected = false;
        std::thread other([&] {
            try { (void)adapter.PendingCount(); }
            catch (const std::logic_error&) { rejected = true; }
        });
        other.join();
        Require(rejected, "Foreign thread access was not rejected");
    });
}

LAMP_TEST(ReentrantPumpRejected) {
    WithContext([](::v8::Isolate* isolate, ::v8::Local<::v8::Context> context) {
        bool reentry_rejected = false;
        execution::V8AsyncWorkerIntegration* adapter_ptr = nullptr;
        execution::V8AsyncWorkerIntegration adapter(
            isolate,
            [&](const bridge::RequestEnvelope& request, Kind,
                ::v8::Local<::v8::Value>) {
                try { (void)adapter_ptr->Pump(); }
                catch (const std::logic_error&) { reentry_rejected = true; }
                return Success(request);
            });
        adapter_ptr = &adapter;
        auto resolver = ::v8::Promise::Resolver::New(context).ToLocalChecked();
        auto completion = MakeRunningCompletion(111);
        Require(adapter.Register(111, resolver->GetPromise(), completion),
                "Registration failed");
        Require(resolver->Resolve(context, ::v8::Undefined(isolate)).FromMaybe(false),
                "Resolution failed");
        Require(adapter.Pump().settled == 1, "Settlement failed");
        Require(reentry_rejected, "Reentrant Pump was not rejected");
        (void)completion->Get();
    });
}

} // namespace

int main() {
    try {
        V8Environment environment;
        std::size_t passed = 0;
        for (const auto& test : Tests()) {
            try {
                test.run();
                ++passed;
                std::cout << "[PASS] " << test.name << '\n';
            } catch (const std::exception& e) {
                std::cerr << "[FAIL] " << test.name << ": " << e.what() << '\n';
                return 1;
            }
        }
        std::cout << "Lamp V8 async integration: " << passed
                  << "/" << Tests().size() << " tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[FATAL] " << e.what() << '\n';
        return 1;
    }
}
