#pragma once

// Lamp Framework - Phase 5
// File: runtime/execution/v8_promise_execution_contract.hpp
// C++20. Worker-local contract for tracking JavaScript Promise settlement.
// This header does not pump microtasks, schedule continuations, or own a V8 isolate.

#include <v8.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace lamp::execution {

// A pending Promise is NOT a completed operation. Settlement must be observed
// on the same isolate thread that created the persistent handles.
enum class V8PromiseState : std::uint8_t {
    Pending,
    Fulfilled,
    Rejected,
    Cancelled,
    DeadlineExceeded,
    Abandoned
};

[[nodiscard]] constexpr bool IsTerminal(V8PromiseState state) noexcept {
    return state != V8PromiseState::Pending;
}

enum class V8PromiseSettlementKind : std::uint8_t {
    Fulfilled,
    Rejected,
    Cancelled,
    DeadlineExceeded,
    Abandoned
};

struct V8PromiseExecutionOptions final {
    // Zero disables the limit. This is a wall-clock budget, not a V8 interrupt.
    std::chrono::steady_clock::duration max_pending_time{};
    // A zero value means no explicit limit; the worker decides its checkpoint policy.
    std::size_t max_microtask_checkpoints = 0;
};

struct V8PromiseSettlement final {
    V8PromiseSettlementKind kind;
    std::uint64_t continuation_id;
    std::chrono::steady_clock::duration elapsed{};
    std::size_t checkpoints = 0;
};

// Callbacks are invoked synchronously on the isolate's owning thread.
// V8 Local handles passed to the callback are valid only during that callback.
using V8PromiseSettlementCallback = std::function<void(
    const V8PromiseSettlement&,
    ::v8::Local<::v8::Value>)>;

// Worker-owned, noncopyable continuation. All methods other than the trivial
// accessors must be called from the creating thread while the isolate lives.
// Destruction must occur before isolate disposal.
class V8PromiseContinuation final {
public:
    using Clock = std::chrono::steady_clock;

    V8PromiseContinuation(
        std::uint64_t id,
        ::v8::Isolate* isolate,
        ::v8::Local<::v8::Promise> promise,
        V8PromiseSettlementCallback callback,
        V8PromiseExecutionOptions options = {})
        : id_(id),
          isolate_(isolate),
          owner_(std::this_thread::get_id()),
          callback_(std::move(callback)),
          options_(options),
          started_(Clock::now()) {
        if (!isolate_ || promise.IsEmpty() || !callback_) {
            throw std::invalid_argument("Promise continuation requires isolate, promise and callback");
        }
        if (options_.max_pending_time < Clock::duration::zero()) {
            throw std::invalid_argument("Promise timeout cannot be negative");
        }
        promise_.Reset(isolate_, promise);
    }

    V8PromiseContinuation(const V8PromiseContinuation&) = delete;
    V8PromiseContinuation& operator=(const V8PromiseContinuation&) = delete;
    V8PromiseContinuation(V8PromiseContinuation&&) = delete;
    V8PromiseContinuation& operator=(V8PromiseContinuation&&) = delete;

    ~V8PromiseContinuation() {
        // Caller must destroy this object on its owning thread, before V8 disposal.
        // A debug assertion or worker-owned container can enforce this at integration.
        promise_.Reset();
    }

    [[nodiscard]] std::uint64_t Id() const noexcept { return id_; }
    [[nodiscard]] V8PromiseState State() const noexcept { return state_; }
    [[nodiscard]] bool Pending() const noexcept { return state_ == V8PromiseState::Pending; }
    [[nodiscard]] std::size_t Checkpoints() const noexcept { return checkpoints_; }
    [[nodiscard]] std::thread::id OwnerThread() const noexcept { return owner_; }

    // Poll after the worker has performed a microtask checkpoint. A pending
    // promise remains pending; no blocking or spin loop is performed here.
    // Returns true exactly when this call publishes a terminal settlement.
    bool Poll() {
        CheckOwner();
        if (!Pending()) return false;
        ++checkpoints_;

        ::v8::HandleScope scope(isolate_);
        auto promise = promise_.Get(isolate_);
        if (promise.IsEmpty()) {
            return Finish(V8PromiseState::Abandoned,
                          V8PromiseSettlementKind::Abandoned,
                          ::v8::Undefined(isolate_));
        }

        switch (promise->State()) {
            case ::v8::Promise::kFulfilled:
                return Finish(V8PromiseState::Fulfilled,
                              V8PromiseSettlementKind::Fulfilled,
                              promise->Result());
            case ::v8::Promise::kRejected:
                return Finish(V8PromiseState::Rejected,
                              V8PromiseSettlementKind::Rejected,
                              promise->Result());
            case ::v8::Promise::kPending:
                break;
        }

        if (options_.max_pending_time != Clock::duration::zero() &&
            Clock::now() - started_ >= options_.max_pending_time) {
            return Finish(V8PromiseState::DeadlineExceeded,
                          V8PromiseSettlementKind::DeadlineExceeded,
                          ::v8::Undefined(isolate_));
        }
        if (options_.max_microtask_checkpoints != 0 &&
            checkpoints_ >= options_.max_microtask_checkpoints) {
            return Finish(V8PromiseState::DeadlineExceeded,
                          V8PromiseSettlementKind::DeadlineExceeded,
                          ::v8::Undefined(isolate_));
        }
        return false;
    }

    bool Cancel() {
        CheckOwner();
        if (!Pending()) return false;
        ::v8::HandleScope scope(isolate_);
        return Finish(V8PromiseState::Cancelled,
                      V8PromiseSettlementKind::Cancelled,
                      ::v8::Undefined(isolate_));
    }

    bool Abandon() {
        CheckOwner();
        if (!Pending()) return false;
        ::v8::HandleScope scope(isolate_);
        return Finish(V8PromiseState::Abandoned,
                      V8PromiseSettlementKind::Abandoned,
                      ::v8::Undefined(isolate_));
    }

private:
    void CheckOwner() const {
        if (std::this_thread::get_id() != owner_) {
            throw std::logic_error("V8 Promise continuation accessed from a foreign thread");
        }
    }

    bool Finish(V8PromiseState next,
                V8PromiseSettlementKind kind,
                ::v8::Local<::v8::Value> value) {
        state_ = next; // terminal before callback; prevents duplicate publication
        promise_.Reset();
        const V8PromiseSettlement settlement{
            kind, id_, Clock::now() - started_, checkpoints_
        };
        callback_(settlement, value);
        return true;
    }

    std::uint64_t id_;
    ::v8::Isolate* isolate_;
    std::thread::id owner_;
    V8PromiseSettlementCallback callback_;
    V8PromiseExecutionOptions options_;
    Clock::time_point started_;
    ::v8::Global<::v8::Promise> promise_;
    V8PromiseState state_ = V8PromiseState::Pending;
    std::size_t checkpoints_ = 0;
};

} // namespace lamp::execution
