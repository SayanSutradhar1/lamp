// Lamp Framework - Phase 5: Promise execution contract regression tests.
// File: runtime/execution/tests/v8_promise_execution_contract_test.cpp
#include "execution/v8_promise_execution_contract.hpp"

#include <libplatform/libplatform.h>
#include <v8.h>

#include <chrono>
#include <cstddef>
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
namespace execution = lamp::execution;
using namespace std::chrono_literals;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class V8Environment final {
public:
    V8Environment() : platform_(::v8::platform::NewDefaultPlatform()) {
        ::v8::V8::InitializePlatform(platform_.get());
        if (!::v8::V8::Initialize()) {
            throw std::runtime_error("V8 initialization failed");
        }
    }
    ~V8Environment() {
        ::v8::V8::Dispose();
        ::v8::V8::DisposePlatform();
    }
    V8Environment(const V8Environment&) = delete;
    V8Environment& operator=(const V8Environment&) = delete;
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
        Require(isolate_ != nullptr, "Failed to create V8 isolate");
    }
    ~IsolateFixture() { isolate_->Dispose(); }
    IsolateFixture(const IsolateFixture&) = delete;
    IsolateFixture& operator=(const IsolateFixture&) = delete;
    ::v8::Isolate* Get() const noexcept { return isolate_; }
private:
    std::unique_ptr<::v8::ArrayBuffer::Allocator> allocator_;
    ::v8::Isolate* isolate_ = nullptr;
};

using Test = void (*)();
struct TestCase { const char* name; Test function; };
std::vector<TestCase>& Registry() {
    static std::vector<TestCase> tests;
    return tests;
}
struct Register {
    Register(const char* name, Test function) { Registry().push_back({name, function}); }
};
#define LAMP_TEST(name) \
    void name(); \
    const Register registered_##name(#name, &name); \
    void name()

// Each test creates its own isolate, and all continuations are destroyed before
// the isolate is disposed. All V8 operations run under an isolate/context scope.
template<class Function>
void WithContext(Function&& function) {
    IsolateFixture fixture;
    ::v8::Isolate::Scope isolate_scope(fixture.Get());
    ::v8::HandleScope handle_scope(fixture.Get());
    auto context = ::v8::Context::New(fixture.Get());
    Require(!context.IsEmpty(), "Failed to create V8 context");
    ::v8::Context::Scope context_scope(context);
    function(fixture.Get(), context);
}

LAMP_TEST(FulfilledPromisePublishesExactlyOnce) {
    WithContext([](::v8::Isolate* isolate, ::v8::Local<::v8::Context> context) {
        auto resolver = ::v8::Promise::Resolver::New(context).ToLocalChecked();
        int calls = 0;
        execution::V8PromiseSettlementKind kind{};
        execution::V8PromiseContinuation continuation(
            101, isolate, resolver->GetPromise(),
            [&](const execution::V8PromiseSettlement& settlement, ::v8::Local<::v8::Value> value) {
                ++calls;
                kind = settlement.kind;
                Require(settlement.continuation_id == 101, "Wrong continuation id");
                Require(value->Int32Value(context).FromMaybe(-1) == 42, "Wrong fulfillment value");
            });
        Require(!continuation.Poll(), "Unsettled promise must stay pending");
        Require(resolver->Resolve(context, ::v8::Integer::New(isolate, 42)).FromMaybe(false),
                "Resolve failed");
        isolate->PerformMicrotaskCheckpoint();
        Require(continuation.Poll(), "Fulfilled promise must publish");
        Require(!continuation.Poll(), "Terminal promise must not republish");
        Require(calls == 1, "Callback must run exactly once");
        Require(kind == execution::V8PromiseSettlementKind::Fulfilled, "Wrong settlement kind");
        Require(continuation.State() == execution::V8PromiseState::Fulfilled, "Wrong state");
    });
}

LAMP_TEST(RejectedPromisePublishesReason) {
    WithContext([](::v8::Isolate* isolate, ::v8::Local<::v8::Context> context) {
        auto resolver = ::v8::Promise::Resolver::New(context).ToLocalChecked();
        bool called = false;
        execution::V8PromiseContinuation continuation(
            102, isolate, resolver->GetPromise(),
            [&](const execution::V8PromiseSettlement& settlement, ::v8::Local<::v8::Value> value) {
                called = true;
                Require(settlement.kind == execution::V8PromiseSettlementKind::Rejected,
                        "Expected rejection");
                Require(value->IsString(), "Rejection reason should be a string");
            });
        auto reason = ::v8::String::NewFromUtf8Literal(isolate, "rejected");
        Require(resolver->Reject(context, reason).FromMaybe(false), "Reject failed");
        isolate->PerformMicrotaskCheckpoint();
        Require(continuation.Poll(), "Rejected promise must publish");
        Require(called, "Rejection callback not invoked");
        Require(continuation.State() == execution::V8PromiseState::Rejected, "Wrong state");
    });
}

LAMP_TEST(PendingPromiseDoesNotPublish) {
    WithContext([](::v8::Isolate* isolate, ::v8::Local<::v8::Context> context) {
        auto resolver = ::v8::Promise::Resolver::New(context).ToLocalChecked();
        int calls = 0;
        execution::V8PromiseContinuation continuation(
            103, isolate, resolver->GetPromise(),
            [&](const auto&, auto) { ++calls; });
        for (int i = 0; i < 3; ++i) {
            isolate->PerformMicrotaskCheckpoint();
            Require(!continuation.Poll(), "Pending promise must not publish");
        }
        Require(continuation.Pending(), "Promise should remain pending");
        Require(continuation.Checkpoints() == 3, "Incorrect checkpoint count");
        Require(calls == 0, "Pending callback must not run");
    });
}

LAMP_TEST(CancelIsTerminalAndIdempotent) {
    WithContext([](::v8::Isolate* isolate, ::v8::Local<::v8::Context> context) {
        auto resolver = ::v8::Promise::Resolver::New(context).ToLocalChecked();
        int calls = 0;
        execution::V8PromiseContinuation continuation(
            104, isolate, resolver->GetPromise(),
            [&](const execution::V8PromiseSettlement& settlement, ::v8::Local<::v8::Value>) {
                ++calls;
                Require(settlement.kind == execution::V8PromiseSettlementKind::Cancelled,
                        "Expected cancellation");
            });
        Require(continuation.Cancel(), "Initial cancellation must publish");
        Require(!continuation.Cancel(), "Repeated cancellation must be a no-op");
        Require(!continuation.Poll(), "Cancelled promise must not republish");
        Require(!continuation.Abandon(), "Cancelled promise cannot be abandoned");
        Require(calls == 1, "Cancellation callback must run once");
        Require(continuation.State() == execution::V8PromiseState::Cancelled, "Wrong state");
    });
}

LAMP_TEST(AbandonIsTerminal) {
    WithContext([](::v8::Isolate* isolate, ::v8::Local<::v8::Context> context) {
        auto resolver = ::v8::Promise::Resolver::New(context).ToLocalChecked();
        int calls = 0;
        execution::V8PromiseContinuation continuation(
            105, isolate, resolver->GetPromise(),
            [&](const execution::V8PromiseSettlement& settlement, ::v8::Local<::v8::Value>) {
                ++calls;
                Require(settlement.kind == execution::V8PromiseSettlementKind::Abandoned,
                        "Expected abandonment");
            });
        Require(continuation.Abandon(), "Abandon must publish");
        Require(!continuation.Abandon(), "Repeated abandon must be a no-op");
        Require(calls == 1, "Abandon callback must run once");
        Require(continuation.State() == execution::V8PromiseState::Abandoned, "Wrong state");
    });
}

LAMP_TEST(CheckpointBudgetExpiresPendingPromise) {
    WithContext([](::v8::Isolate* isolate, ::v8::Local<::v8::Context> context) {
        auto resolver = ::v8::Promise::Resolver::New(context).ToLocalChecked();
        execution::V8PromiseExecutionOptions options;
        options.max_microtask_checkpoints = 2;
        int calls = 0;
        execution::V8PromiseContinuation continuation(
            106, isolate, resolver->GetPromise(),
            [&](const execution::V8PromiseSettlement& settlement, ::v8::Local<::v8::Value>) {
                ++calls;
                Require(settlement.kind == execution::V8PromiseSettlementKind::DeadlineExceeded,
                        "Expected checkpoint deadline");
                Require(settlement.checkpoints == 2, "Wrong checkpoint count");
            }, options);
        Require(!continuation.Poll(), "First checkpoint should not expire");
        Require(continuation.Poll(), "Second checkpoint should expire");
        Require(calls == 1, "Deadline callback must run once");
        Require(continuation.State() == execution::V8PromiseState::DeadlineExceeded,
                "Wrong state");
    });
}

LAMP_TEST(WallClockBudgetExpiresPendingPromise) {
    WithContext([](::v8::Isolate* isolate, ::v8::Local<::v8::Context> context) {
        auto resolver = ::v8::Promise::Resolver::New(context).ToLocalChecked();
        execution::V8PromiseExecutionOptions options;
        options.max_pending_time = 1ms;
        bool called = false;
        execution::V8PromiseContinuation continuation(
            107, isolate, resolver->GetPromise(),
            [&](const execution::V8PromiseSettlement& settlement, ::v8::Local<::v8::Value>) {
                called = true;
                Require(settlement.kind == execution::V8PromiseSettlementKind::DeadlineExceeded,
                        "Expected wall-clock deadline");
            }, options);
        std::this_thread::sleep_for(5ms);
        Require(continuation.Poll(), "Elapsed timeout must expire");
        Require(called, "Timeout callback not invoked");
    });
}

LAMP_TEST(ForeignThreadAccessIsRejected) {
    WithContext([](::v8::Isolate* isolate, ::v8::Local<::v8::Context> context) {
        auto resolver = ::v8::Promise::Resolver::New(context).ToLocalChecked();
        execution::V8PromiseContinuation continuation(
            108, isolate, resolver->GetPromise(),
            [](const auto&, auto) {});
        bool rejected = false;
        std::thread foreign([&] {
            try { (void)continuation.Poll(); }
            catch (const std::logic_error&) { rejected = true; }
        });
        foreign.join();
        Require(rejected, "Foreign thread must be rejected before V8 access");
        Require(continuation.Pending(), "Foreign access must not change state");
        Require(continuation.OwnerThread() == std::this_thread::get_id(),
                "Owner thread mismatch");
    });
}

LAMP_TEST(InvalidConstructorArgumentsAreRejected) {
    WithContext([](::v8::Isolate* isolate, ::v8::Local<::v8::Context> context) {
        auto resolver = ::v8::Promise::Resolver::New(context).ToLocalChecked();
        bool rejected = false;
        try {
            execution::V8PromiseContinuation invalid(
                109, isolate, resolver->GetPromise(), {});
        } catch (const std::invalid_argument&) { rejected = true; }
        Require(rejected, "Missing callback must be rejected");

        execution::V8PromiseExecutionOptions options;
        options.max_pending_time = -1ms;
        rejected = false;
        try {
            execution::V8PromiseContinuation invalid(
                110, isolate, resolver->GetPromise(),
                [](const auto&, auto) {}, options);
        } catch (const std::invalid_argument&) { rejected = true; }
        Require(rejected, "Negative timeout must be rejected");
    });
}

LAMP_TEST(CallbackExceptionDoesNotPermitDuplicateSettlement) {
    WithContext([](::v8::Isolate* isolate, ::v8::Local<::v8::Context> context) {
        auto resolver = ::v8::Promise::Resolver::New(context).ToLocalChecked();
        int calls = 0;
        execution::V8PromiseContinuation continuation(
            111, isolate, resolver->GetPromise(),
            [&](const auto&, auto) {
                ++calls;
                throw std::runtime_error("callback failure");
            });
        bool threw = false;
        try { (void)continuation.Cancel(); }
        catch (const std::runtime_error&) { threw = true; }
        Require(threw, "Callback exception should propagate");
        Require(!continuation.Pending(), "Continuation must be terminal despite callback failure");
        Require(!continuation.Poll(), "Terminal continuation cannot publish again");
        Require(calls == 1, "Callback must not be invoked twice");
    });
}
} // namespace

int main() {
    std::size_t passed = 0;
    std::size_t failed = 0;
    try {
        V8Environment environment;
        std::cout << "\nLamp Phase 5 - V8 Promise Contract Tests\n\n";
        for (const auto& test : Registry()) {
            try {
                test.function();
                ++passed;
                std::cout << "[PASS] " << test.name << '\n';
            } catch (const std::exception& error) {
                ++failed;
                std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n';
            } catch (...) {
                ++failed;
                std::cerr << "[FAIL] " << test.name << ": unknown exception\n";
            }
        }
    } catch (const std::exception& error) {
        std::cerr << "[FATAL] " << error.what() << '\n';
        return 1;
    }
    std::cout << "\nPassed: " << passed << "  Failed: " << failed
              << "  Total: " << Registry().size() << '\n';
    return failed == 0 ? 0 : 1;
}
