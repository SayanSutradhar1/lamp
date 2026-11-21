#pragma once

// Lamp Framework - Phase 5
// File: runtime/execution/v8_async_worker_integration.hpp
// C++20: worker-local adapter from V8 Promise settlement to Lamp task completion.
//
// Construct, call, and destroy on the V8 isolate's owning thread, with the
// isolate alive. Pump() must be called periodically by the worker/event loop;
// this class deliberately does not spawn threads or run an I/O event loop.

#include "execution/v8_isolate_worker_pool.hpp"
#include "execution/v8_promise_continuation_scheduler.hpp"

#include <v8.h>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lamp::execution {

// Converts a fulfilled/rejected JS value into a Lamp result. The value is a
// V8 Local handle, valid only during the callback on the owning worker.
// Implementations should serialize/copy any data before returning.
using V8AsyncResultMapper = std::function<bridge::ResultEnvelope(
    const bridge::RequestEnvelope&,
    V8PromiseSettlementKind,
    ::v8::Local<::v8::Value>)>;

class V8AsyncWorkerIntegration final {
public:
    using CompletionPtr = std::shared_ptr<AsyncTaskCompletion>;

    explicit V8AsyncWorkerIntegration(
        ::v8::Isolate* isolate,
        V8AsyncResultMapper mapper,
        V8PromiseSchedulerConfig config = {})
        : owner_(std::this_thread::get_id()),
          mapper_(std::move(mapper)),
          scheduler_(isolate, config) {
        if (!mapper_) {
            throw std::invalid_argument("V8 async integration requires a result mapper");
        }
    }

    V8AsyncWorkerIntegration(const V8AsyncWorkerIntegration&) = delete;
    V8AsyncWorkerIntegration& operator=(const V8AsyncWorkerIntegration&) = delete;
    V8AsyncWorkerIntegration(V8AsyncWorkerIntegration&&) = delete;
    V8AsyncWorkerIntegration& operator=(V8AsyncWorkerIntegration&&) = delete;

    // Caller must call AbandonAll() before destruction if outstanding tasks
    // require notification. Scheduler destruction releases persistent handles.
    ~V8AsyncWorkerIntegration() = default;

    // Completion must already be in Running state. A successful registration
    // transfers responsibility for publishing its final result to this adapter.
    // IDs must be unique among pending continuations.
    [[nodiscard]] bool Register(
        std::uint64_t id,
        ::v8::Local<::v8::Promise> promise,
        CompletionPtr completion,
        V8PromiseExecutionOptions options = {}) {
        CheckOwner();
        CheckNotPumping();
        if (!completion || promise.IsEmpty() || tracked_.contains(id)) {
            return false;
        }
        if (completion->CancellationRequested() ||
            completion->Request().IsExpired()) {
            return false;
        }

        // Capture the completion directly: the callback can publish a result
        // without relying on map iterator validity during scheduler.Pump().
        auto callback = [this, id, completion](
            const V8PromiseSettlement& settlement,
            ::v8::Local<::v8::Value> value) {
            // Remove tracking before user mapping, even if mapping throws.
            tracked_.erase(id);
            PublishSettlement(completion, settlement.kind, value);
        };

        if (!scheduler_.Register(id, promise, std::move(callback), options)) {
            return false;
        }
        try {
            tracked_.emplace(id, std::move(completion));
        } catch (...) {
            // Registration succeeded but bookkeeping failed: settle the
            // continuation immediately rather than leave an orphan.
            (void)scheduler_.Cancel(id);
            throw;
        }
        return true;
    }

    // Pump microtasks, then publish Promise settlements. Before pumping,
    // cancel continuations whose Lamp handle or request has been cancelled,
    // and those whose request deadline has elapsed.
    [[nodiscard]] V8PromisePumpResult Pump() {
        CheckOwner();
        CheckNotPumping();
        Guard guard(pumping_);

        std::vector<std::uint64_t> cancelled;
        std::vector<std::uint64_t> expired;
        cancelled.reserve(tracked_.size());
        expired.reserve(tracked_.size());
        for (const auto& [id, completion] : tracked_) {
            if (completion->CancellationRequested()) {
                cancelled.push_back(id);
            } else if (completion->Request().IsExpired()) {
                expired.push_back(id);
            }
        }
        for (const auto id : cancelled) {
            (void)scheduler_.Cancel(id);
        }
        // The Promise scheduler has no deadline-expiry API other than Poll.
        // Publish request deadline directly, then release the V8 handle via
        // cancellation. Completion is idempotent, so the cancellation callback
        // cannot overwrite the deadline result.
        for (const auto id : expired) {
            const auto it = tracked_.find(id);
            if (it == tracked_.end()) continue;
            const auto completion = it->second;
            Publish(completion, bridge::ResultEnvelope::DeadlineExceeded(
                completion->Request()));
            (void)scheduler_.Cancel(id);
        }
        return scheduler_.Pump();
    }

    [[nodiscard]] bool Cancel(std::uint64_t id) {
        CheckOwner();
        CheckNotPumping();
        return scheduler_.Cancel(id);
    }

    [[nodiscard]] std::size_t AbandonAll() {
        CheckOwner();
        CheckNotPumping();
        return scheduler_.AbandonAll();
    }

    [[nodiscard]] std::size_t PendingCount() const {
        CheckOwner();
        return scheduler_.PendingCount();
    }

    [[nodiscard]] bool Empty() const {
        CheckOwner();
        return scheduler_.Empty();
    }

private:
    struct Guard final {
        explicit Guard(bool& flag) : flag_(flag) { flag_ = true; }
        ~Guard() { flag_ = false; }
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
        bool& flag_;
    };

    void CheckOwner() const {
        if (std::this_thread::get_id() != owner_) {
            throw std::logic_error("V8 async integration accessed from foreign thread");
        }
    }

    void CheckNotPumping() const {
        if (pumping_) {
            throw std::logic_error("V8 async integration reentered during Pump");
        }
    }

    static void Publish(const CompletionPtr& completion,
                        bridge::ResultEnvelope result) noexcept {
        try {
            (void)completion->Complete(std::move(result));
        } catch (...) {
            // Completion publication is best-effort; the completion contract
            // is responsible for its own terminal-state guarantees.
        }
    }

    void PublishSettlement(const CompletionPtr& completion,
                           V8PromiseSettlementKind kind,
                           ::v8::Local<::v8::Value> value) noexcept {
        const auto& request = completion->Request();
        try {
            switch (kind) {
                case V8PromiseSettlementKind::Cancelled:
                    Publish(completion, bridge::ResultEnvelope::Cancelled(request));
                    return;
                case V8PromiseSettlementKind::DeadlineExceeded:
                    Publish(completion, bridge::ResultEnvelope::DeadlineExceeded(request));
                    return;
                case V8PromiseSettlementKind::Abandoned:
                    Publish(completion, bridge::ResultEnvelope::Failure(
                        request,
                        {bridge::OperationErrorCode::ExecutionFailed,
                         "V8 Promise abandoned before settlement",
                         "V8_PROMISE_ABANDONED", false},
                        bridge::ExecutionBackend::JavaScript));
                    return;
                case V8PromiseSettlementKind::Fulfilled:
                case V8PromiseSettlementKind::Rejected:
                    Publish(completion, mapper_(request, kind, value));
                    return;
            }
        } catch (...) {
            Publish(completion, bridge::ResultEnvelope::Failure(
                request,
                {bridge::OperationErrorCode::ExecutionFailed,
                 "V8 Promise result mapping failed",
                 "V8_PROMISE_MAPPING_EXCEPTION", false},
                bridge::ExecutionBackend::JavaScript));
        }
    }

    std::thread::id owner_;
    V8AsyncResultMapper mapper_;
    V8PromiseContinuationScheduler scheduler_;
    std::unordered_map<std::uint64_t, CompletionPtr> tracked_;
    bool pumping_ = false;
};

} // namespace lamp::execution
