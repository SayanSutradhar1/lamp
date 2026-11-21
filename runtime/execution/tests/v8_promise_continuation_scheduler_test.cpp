// Lamp Framework - Phase 5: V8 Promise continuation scheduler regression tests.
// File: runtime/execution/tests/v8_promise_continuation_scheduler_test.cpp
// C++20; standalone test executable using the project's V8 SDK.

#include "execution/v8_promise_continuation_scheduler.hpp"

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

using Test = void (*)();
struct TestCase { const char* name; Test function; };
std::vector<TestCase>& Registry() {
    static std::vector<TestCase> tests;
    return tests;
}
struct Register {
    Register(const char* name, Test function) {
        Registry().push_back({name, function});
    }
};
#define LAMP_TEST(name) \
    void name(); \
    const Register registered_##name(#name, &name); \
    void name()

using Kind = execution::V8PromiseSettlementKind;
using Settlement = execution::V8PromiseSettlement;
using Value = ::v8::Local<::v8::Value>;

LAMP_TEST(RejectsInvalidConfiguration) {
    bool null_rejected = false;
    try { execution::V8PromiseContinuationScheduler scheduler(nullptr); }
    catch (const std::invalid_argument&) { null_rejected = true; }
    Require(null_rejected, "Null isolate must be rejected");

    WithContext([](::v8::Isolate* isolate, auto) {
        bool zero_capacity = false;
        try {
            execution::V8PromiseContinuationScheduler scheduler(
                isolate, {.max_pending_continuations = 0, .checkpoints_per_pump = 1});
        } catch (const std::invalid_argument&) { zero_capacity = true; }
        Require(zero_capacity, "Zero capacity must be rejected");

        bool zero_checkpoints = false;
        try {
            execution::V8PromiseContinuationScheduler scheduler(
                isolate, {.max_pending_continuations = 1, .checkpoints_per_pump = 0});
        } catch (const std::invalid_argument&) { zero_checkpoints = true; }
        Require(zero_checkpoints, "Zero checkpoints must be rejected");
    });
}

LAMP_TEST(EmptyPumpDoesNotRunCheckpoints) {
    WithContext([](::v8::Isolate* isolate, auto) {
        execution::V8PromiseContinuationScheduler scheduler(isolate);
        Require(scheduler.Empty(), "New scheduler must be empty");
        Require(scheduler.PendingCount() == 0, "New scheduler count must be zero");
        auto result = scheduler.Pump();
        Require(result.checkpoints == 0, "Empty pump must not run checkpoints");
        Require(result.settled == 0 && result.pending == 0, "Empty pump counts incorrect");
        Require(scheduler.AbandonAll() == 0, "Empty abandon must return zero");
        Require(!scheduler.Cancel(123), "Cancelling missing ID must fail");
    });
}

LAMP_TEST(RegisterDuplicateAndCapacity) {
    WithContext([](::v8::Isolate* isolate, ::v8::Local<::v8::Context> context) {
        execution::V8PromiseContinuationScheduler scheduler(
            isolate, {.max_pending_continuations = 1, .checkpoints_per_pump = 1});
        auto first = ::v8::Promise::Resolver::New(context).ToLocalChecked();
        auto second = ::v8::Promise::Resolver::New(context).ToLocalChecked();
        auto callback = [](const Settlement&, Value) {};
        Require(scheduler.Register(11, first->GetPromise(), callback), "First register failed");
        Require(scheduler.Contains(11), "Registered ID missing");
        Require(!scheduler.Register(11, first->GetPromise(), callback), "Duplicate ID accepted");
        Require(!scheduler.Register(12, second->GetPromise(), callback), "Capacity exceeded");
        Require(scheduler.PendingCount() == 1, "Capacity rejection changed count");
        Require(scheduler.AbandonAll() == 1, "Cleanup count incorrect");
        Require(scheduler.Empty(), "Cleanup did not empty scheduler");
    });
}

LAMP_TEST(PendingPromiseSurvivesRepeatedPumps) {
    WithContext([](::v8::Isolate* isolate, ::v8::Local<::v8::Context> context) {
        execution::V8PromiseContinuationScheduler scheduler(
            isolate, {.max_pending_continuations = 4, .checkpoints_per_pump = 2});
        auto resolver = ::v8::Promise::Resolver::New(context).ToLocalChecked();
        int calls = 0;
        Require(scheduler.Register(21, resolver->GetPromise(),
            [&](const Settlement&, Value) { ++calls; }), "Registration failed");
        for (int i = 0; i < 3; ++i) {
            auto result = scheduler.Pump();
            Require(result.checkpoints == 2, "Configured checkpoint count ignored");
            Require(result.pending == 1 && result.settled == 0, "Pending promise lost");
        }
        Require(calls == 0, "Pending promise invoked callback");
        Require(scheduler.Cancel(21), "Cleanup cancellation failed");
        Require(calls == 1, "Cancellation callback missing");
    });
}

LAMP_TEST(FulfilledPromisePublishesExactlyOnce) {
    WithContext([](::v8::Isolate* isolate, ::v8::Local<::v8::Context> context) {
        execution::V8PromiseContinuationScheduler scheduler(isolate);
        auto resolver = ::v8::Promise::Resolver::New(context).ToLocalChecked();
        int calls = 0;
        Kind kind{};
        std::uint64_t id = 0;
        Require(scheduler.Register(31, resolver->GetPromise(),
            [&](const Settlement& settlement, Value value) {
                ++calls;
                kind = settlement.kind;
                id = settlement.continuation_id;
                Require(value->IsInt32(), "Fulfillment value is not an integer");
                Require(value.As<::v8::Int32>()->Value() == 42, "Fulfillment value incorrect");
            }), "Registration failed");
        Require(resolver->Resolve(context, ::v8::Integer::New(isolate, 42)).FromMaybe(false),
                "Resolve failed");
        auto result = scheduler.Pump();
        Require(result.settled == 1 && result.pending == 0, "Fulfilled promise not removed");
        Require(calls == 1 && kind == Kind::Fulfilled && id == 31, "Fulfillment incorrect");
        (void)scheduler.Pump();
        Require(calls == 1 && !scheduler.Contains(31), "Duplicate settlement observed");
    });
}

LAMP_TEST(RejectedPromisePublishesReason) {
    WithContext([](::v8::Isolate* isolate, ::v8::Local<::v8::Context> context) {
        execution::V8PromiseContinuationScheduler scheduler(isolate);
        auto resolver = ::v8::Promise::Resolver::New(context).ToLocalChecked();
        int calls = 0;
        Require(scheduler.Register(41, resolver->GetPromise(),
            [&](const Settlement& settlement, Value value) {
                ++calls;
                Require(settlement.kind == Kind::Rejected, "Expected rejection");
                Require(value->IsString(), "Rejection reason missing");
            }), "Registration failed");
        auto reason = ::v8::String::NewFromUtf8Literal(isolate, "rejected");
        Require(resolver->Reject(context, reason).FromMaybe(false), "Reject failed");
        auto result = scheduler.Pump();
        Require(result.settled == 1 && result.pending == 0, "Rejection not removed");
        Require(calls == 1, "Rejection callback not invoked once");
    });
}

LAMP_TEST(MultiplePromisesSettleIndependently) {
    WithContext([](::v8::Isolate* isolate, ::v8::Local<::v8::Context> context) {
        execution::V8PromiseContinuationScheduler scheduler(isolate);
        auto first = ::v8::Promise::Resolver::New(context).ToLocalChecked();
        auto second = ::v8::Promise::Resolver::New(context).ToLocalChecked();
        auto third = ::v8::Promise::Resolver::New(context).ToLocalChecked();
        int calls = 0;
        auto callback = [&](const Settlement&, Value) { ++calls; };
        Require(scheduler.Register(51, first->GetPromise(), callback), "Register 51 failed");
        Require(scheduler.Register(52, second->GetPromise(), callback), "Register 52 failed");
        Require(scheduler.Register(53, third->GetPromise(), callback), "Register 53 failed");
        Require(first->Resolve(context, ::v8::Undefined(isolate)).FromMaybe(false), "Resolve 51 failed");
        Require(third->Resolve(context, ::v8::Undefined(isolate)).FromMaybe(false), "Resolve 53 failed");
        auto result = scheduler.Pump();
        Require(result.settled == 2 && result.pending == 1, "Partial settlement counts incorrect");
        Require(scheduler.Contains(52), "Pending continuation removed");
        Require(!scheduler.Contains(51) && !scheduler.Contains(53), "Settled continuation retained");
        Require(second->Resolve(context, ::v8::Undefined(isolate)).FromMaybe(false), "Resolve 52 failed");
        result = scheduler.Pump();
        Require(result.settled == 1 && result.pending == 0, "Final settlement incorrect");
        Require(calls == 3, "Callbacks not invoked exactly once");
    });
}

LAMP_TEST(CancelAndAbandonPublishTerminalStates) {
    WithContext([](::v8::Isolate* isolate, ::v8::Local<::v8::Context> context) {
        execution::V8PromiseContinuationScheduler scheduler(isolate);
        auto a = ::v8::Promise::Resolver::New(context).ToLocalChecked();
        auto b = ::v8::Promise::Resolver::New(context).ToLocalChecked();
        auto c = ::v8::Promise::Resolver::New(context).ToLocalChecked();
        int cancelled = 0;
        int abandoned = 0;
        auto callback = [&](const Settlement& s, Value) {
            if (s.kind == Kind::Cancelled) ++cancelled;
            if (s.kind == Kind::Abandoned) ++abandoned;
        };
        Require(scheduler.Register(61, a->GetPromise(), callback), "Register 61 failed");
        Require(scheduler.Register(62, b->GetPromise(), callback), "Register 62 failed");
        Require(scheduler.Register(63, c->GetPromise(), callback), "Register 63 failed");
        Require(scheduler.Cancel(61), "Cancel failed");
        Require(!scheduler.Cancel(61), "Second cancel should fail");
        Require(scheduler.AbandonAll() == 2, "Abandon count incorrect");
        Require(cancelled == 1 && abandoned == 2, "Terminal notifications incorrect");
        Require(scheduler.Empty(), "Scheduler not empty after cleanup");
    });
}

LAMP_TEST(CheckpointBudgetExpiresPendingPromise) {
    WithContext([](::v8::Isolate* isolate, ::v8::Local<::v8::Context> context) {
        execution::V8PromiseContinuationScheduler scheduler(isolate);
        auto resolver = ::v8::Promise::Resolver::New(context).ToLocalChecked();
        int calls = 0;
        Require(scheduler.Register(71, resolver->GetPromise(),
            [&](const Settlement& s, Value) {
                ++calls;
                Require(s.kind == Kind::DeadlineExceeded, "Expected checkpoint deadline");
                Require(s.checkpoints == 2, "Checkpoint count incorrect");
            }, {.max_pending_time = {}, .max_microtask_checkpoints = 2}),
            "Registration failed");
        auto first = scheduler.Pump();
        Require(first.pending == 1, "Promise expired too early");
        auto second = scheduler.Pump();
        Require(second.settled == 1 && second.pending == 0, "Checkpoint limit not enforced");
        Require(calls == 1, "Deadline callback not invoked once");
    });
}

LAMP_TEST(ExpiredWallClockBudget) {
    WithContext([](::v8::Isolate* isolate, ::v8::Local<::v8::Context> context) {
        execution::V8PromiseContinuationScheduler scheduler(isolate);
        auto resolver = ::v8::Promise::Resolver::New(context).ToLocalChecked();
        int calls = 0;
        Require(scheduler.Register(81, resolver->GetPromise(),
            [&](const Settlement& s, Value) {
                ++calls;
                Require(s.kind == Kind::DeadlineExceeded, "Expected time deadline");
            }, {.max_pending_time = 1ns, .max_microtask_checkpoints = 0}),
            "Registration failed");
        auto result = scheduler.Pump();
        Require(result.settled == 1 && result.pending == 0, "Expired promise retained");
        Require(calls == 1, "Timeout callback not invoked");
    });
}

LAMP_TEST(ForeignThreadOperationsRejected) {
    WithContext([](::v8::Isolate* isolate, auto) {
        execution::V8PromiseContinuationScheduler scheduler(isolate);
        bool contains_rejected = false;
        bool pump_rejected = false;
        bool cancel_rejected = false;
        bool abandon_rejected = false;
        std::thread foreign([&] {
            try { (void)scheduler.Contains(1); }
            catch (const std::logic_error&) { contains_rejected = true; }
            try { (void)scheduler.Pump(); }
            catch (const std::logic_error&) { pump_rejected = true; }
            try { (void)scheduler.Cancel(1); }
            catch (const std::logic_error&) { cancel_rejected = true; }
            try { (void)scheduler.AbandonAll(); }
            catch (const std::logic_error&) { abandon_rejected = true; }
        });
        foreign.join();
        Require(contains_rejected && pump_rejected && cancel_rejected && abandon_rejected,
                "Foreign thread operation was not rejected");
    });
}

LAMP_TEST(SettlementCallbackCannotMutateSchedulerDuringPump) {
    WithContext([](::v8::Isolate* isolate, ::v8::Local<::v8::Context> context) {
        execution::V8PromiseContinuationScheduler scheduler(isolate);
        auto resolver = ::v8::Promise::Resolver::New(context).ToLocalChecked();
        bool rejected_mutation = false;
        Require(scheduler.Register(91, resolver->GetPromise(),
            [&](const Settlement&, Value) {
                try { (void)scheduler.Cancel(91); }
                catch (const std::logic_error&) { rejected_mutation = true; }
            }), "Registration failed");
        Require(resolver->Resolve(context, ::v8::Undefined(isolate)).FromMaybe(false),
                "Resolve failed");
        auto result = scheduler.Pump();
        Require(result.settled == 1, "Settlement missing");
        Require(rejected_mutation, "Reentrant mutation was not rejected");
        Require(scheduler.Empty(), "Settled continuation retained");
    });
}

LAMP_TEST(ThrowingCallbackDoesNotRetainSettledContinuation) {
    WithContext([](::v8::Isolate* isolate, ::v8::Local<::v8::Context> context) {
        execution::V8PromiseContinuationScheduler scheduler(isolate);
        auto resolver = ::v8::Promise::Resolver::New(context).ToLocalChecked();
        Require(scheduler.Register(101, resolver->GetPromise(),
            [](const Settlement&, Value) { throw std::runtime_error("callback failure"); }),
            "Registration failed");
        Require(resolver->Resolve(context, ::v8::Undefined(isolate)).FromMaybe(false),
                "Resolve failed");
        bool propagated = false;
        try { (void)scheduler.Pump(); }
        catch (const std::runtime_error&) { propagated = true; }
        Require(propagated, "Callback exception did not propagate");
        Require(scheduler.Empty(), "Terminal continuation retained after exception");
        Require(scheduler.Pump().settled == 0, "Terminal callback invoked again");
    });
}

LAMP_TEST(ThrowingCancellationCallbackDoesNotRetainContinuation) {
    WithContext([](::v8::Isolate* isolate, ::v8::Local<::v8::Context> context) {
        execution::V8PromiseContinuationScheduler scheduler(isolate);
        auto resolver = ::v8::Promise::Resolver::New(context).ToLocalChecked();
        Require(scheduler.Register(111, resolver->GetPromise(),
            [](const Settlement&, Value) { throw std::runtime_error("cancel failure"); }),
            "Registration failed");
        bool propagated = false;
        try { (void)scheduler.Cancel(111); }
        catch (const std::runtime_error&) { propagated = true; }
        Require(propagated, "Cancellation exception did not propagate");
        Require(scheduler.Empty(), "Cancelled continuation retained after exception");
    });
}

} // namespace

int main() {
    try {
        V8Environment environment;
        std::size_t passed = 0;
        for (const auto& test : Registry()) {
            try {
                test.function();
                ++passed;
                std::cout << "[PASS] " << test.name << '\n';
            } catch (const std::exception& error) {
                std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n';
                return 1;
            } catch (...) {
                std::cerr << "[FAIL] " << test.name << ": unknown exception\n";
                return 1;
            }
        }
        std::cout << "Lamp V8 Promise scheduler: " << passed << "/"
                  << Registry().size() << " tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[FATAL] " << error.what() << '\n';
        return 1;
    }
}
