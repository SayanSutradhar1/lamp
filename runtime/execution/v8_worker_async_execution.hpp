#pragma once

// Lamp Framework - Phase 5
// File: runtime/execution/v8_worker_async_execution.hpp
// C++20: worker-local execution result contract for synchronous and Promise work.
//
// This header intentionally does not include v8_isolate_worker_pool.hpp or
// v8_async_worker_integration.hpp. The pool and adapter can depend on this
// contract without introducing a circular dependency.
//
// All V8 Local handles in this interface are strictly worker-thread local.
// The executor must not retain them after returning. The pool must register
// a returned Promise with its worker-local continuation scheduler before the
// current V8 HandleScope ends. Neither this contract nor a Promise alone
// provides an external I/O/timer event loop.

#include "execution/async_execution_contract.hpp"
#include "execution/v8_promise_execution_contract.hpp"

#include <v8.h>

#include <chrono>
#include <cstddef>
#include <functional>
#include <stdexcept>
#include <utility>
#include <variant>

namespace lamp::execution {

struct V8WorkerExecutionContext;

// An executor can return a completed bridge result or a JavaScript Promise.
// The Promise remains a Local handle: it must be consumed by the worker before
// leaving the active HandleScope. It must never be stored in a cross-thread
// task queue or passed to another isolate.
class V8WorkerAsyncResult final {
public:
    using Immediate = bridge::ResultEnvelope;
    using Promise = ::v8::Local<::v8::Promise>;

    V8WorkerAsyncResult(Immediate result)
        : value_(std::in_place_type<Immediate>, std::move(result)) {}

    V8WorkerAsyncResult(Promise promise)
        : value_(std::in_place_type<Promise>, CheckedPromise(promise)) {}

    [[nodiscard]] static V8WorkerAsyncResult Completed(Immediate result) {
        return V8WorkerAsyncResult(std::move(result));
    }

    [[nodiscard]] static V8WorkerAsyncResult Pending(Promise promise) {
        return V8WorkerAsyncResult(promise);
    }

    [[nodiscard]] bool IsCompleted() const noexcept {
        return std::holds_alternative<Immediate>(value_);
    }

    [[nodiscard]] bool IsPromise() const noexcept {
        return std::holds_alternative<Promise>(value_);
    }

    // Accessors throw std::bad_variant_access for the wrong alternative.
    [[nodiscard]] const Immediate& CompletedResult() const & {
        return std::get<Immediate>(value_);
    }

    [[nodiscard]] Immediate TakeCompletedResult() && {
        return std::get<Immediate>(std::move(value_));
    }

    [[nodiscard]] Promise PromiseHandle() const {
        return std::get<Promise>(value_);
    }

private:
    [[nodiscard]] static Promise CheckedPromise(Promise promise) {
        if (promise.IsEmpty()) {
            throw std::invalid_argument(
                "V8 asynchronous executor returned an empty Promise");
        }
        return promise;
    }

    std::variant<Immediate, Promise> value_;
};

// Invoked on the owning worker with the isolate and context entered.
// The pool owns task lifecycle and completion publication. The callback must
// not persist V8 Local handles, and should return promptly without blocking
// the isolate thread waiting for the returned Promise to settle.
using V8WorkerAsyncOperationExecutor = std::function<V8WorkerAsyncResult(
    const bridge::RequestEnvelope&,
    const V8WorkerExecutionContext&)>;

// Converts fulfilled/rejected JS values to an owned bridge result. The
// settlement kind is either Fulfilled or Rejected; cancellation, deadlines,
// and abandonment are handled by the continuation adapter itself. The value
// is valid only during this call on the isolate's owning worker thread.
// A mapper exception is converted by the adapter to a terminal failure.
using V8WorkerPromiseResultMapper = std::function<bridge::ResultEnvelope(
    const bridge::RequestEnvelope&,
    V8PromiseSettlementKind,
    ::v8::Local<::v8::Value>)>;

// Configuration for the Promise-aware worker path. These are explicit bounds:
// the pool must enforce the per-worker continuation cap and arrange bounded
// checkpoints/wakeups, cancellation, deadlines, and shutdown cleanup.
struct V8WorkerAsyncExecutionConfig final {
    std::size_t max_pending_continuations_per_worker = 1024;
    std::size_t microtask_checkpoints_per_pump = 1;
    std::chrono::milliseconds idle_pump_interval{5};

    void Validate() const {
        if (max_pending_continuations_per_worker == 0 ||
            microtask_checkpoints_per_pump == 0 ||
            idle_pump_interval <= std::chrono::milliseconds::zero()) {
            throw std::invalid_argument(
                "V8 asynchronous worker limits must be positive");
        }
    }
};

} // namespace lamp::execution
