#pragma once

// Lamp Framework - Phase 5
// File: runtime/execution/v8_promise_continuation_scheduler.hpp
// C++20. Worker-local, nonblocking Promise continuation scheduler.
//
// All public operations (including destruction) must run on the isolate's
// owning thread, before the isolate is disposed. This scheduler does not own
// the isolate, drive an event loop, or make asynchronous I/O progress.

#include "execution/v8_promise_execution_contract.hpp"

#include <v8.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lamp::execution {

struct V8PromiseSchedulerConfig final {
    // Upper bound on simultaneously tracked promises.
    std::size_t max_pending_continuations = 4096;

    // Maximum number of microtask checkpoints per Pump() call.
    // One is sufficient for most ordinary Promise chains.
    std::size_t checkpoints_per_pump = 1;
};

struct V8PromisePumpResult final {
    std::size_t checkpoints = 0;
    std::size_t settled = 0;
    std::size_t pending = 0;
};

class V8PromiseContinuationScheduler final {
public:
    explicit V8PromiseContinuationScheduler(
        ::v8::Isolate* isolate,
        V8PromiseSchedulerConfig config = {})
        : isolate_(isolate),
          owner_(std::this_thread::get_id()),
          config_(config) {
        if (!isolate_) {
            throw std::invalid_argument("Promise scheduler requires a V8 isolate");
        }
        if (config_.max_pending_continuations == 0 ||
            config_.checkpoints_per_pump == 0) {
            throw std::invalid_argument("Promise scheduler limits must be positive");
        }
    }

    V8PromiseContinuationScheduler(const V8PromiseContinuationScheduler&) = delete;
    V8PromiseContinuationScheduler& operator=(const V8PromiseContinuationScheduler&) = delete;
    V8PromiseContinuationScheduler(V8PromiseContinuationScheduler&&) = delete;
    V8PromiseContinuationScheduler& operator=(V8PromiseContinuationScheduler&&) = delete;

    ~V8PromiseContinuationScheduler() {
        // Caller must destroy this scheduler on the owner thread, before
        // disposing the isolate. Destruction deliberately does not invoke
        // user callbacks. Call AbandonAll() first if notification is needed.
        continuations_.clear();
    }

    [[nodiscard]] std::thread::id OwnerThread() const noexcept {
        return owner_;
    }

    [[nodiscard]] std::size_t PendingCount() const noexcept {
        return continuations_.size();
    }

    [[nodiscard]] bool Empty() const noexcept {
        return continuations_.empty();
    }

    [[nodiscard]] bool Contains(std::uint64_t id) const {
        CheckOwner();
        return continuations_.contains(id);
    }

    // Registers a Promise without executing any JavaScript or microtasks.
    // IDs are unique among currently pending continuations; callers should
    // use globally unique request/continuation IDs to avoid stale references.
    // Returns false if capacity is exhausted or the ID already exists.
    bool Register(
        std::uint64_t id,
        ::v8::Local<::v8::Promise> promise,
        V8PromiseSettlementCallback callback,
        V8PromiseExecutionOptions options = {}) {
        CheckOwner();
        CheckNotPumping();
        if (continuations_.contains(id) ||
            continuations_.size() >= config_.max_pending_continuations) {
            return false;
        }

        auto continuation = std::make_unique<V8PromiseContinuation>(
            id, isolate_, promise, std::move(callback), options);
        continuations_.emplace(id, std::move(continuation));
        return true;
    }

    // Performs bounded V8 microtask checkpoints and polls a snapshot of the
    // registered continuations. An unresolved Promise stays registered.
    //
    // Call from the owning worker with the isolate entered and a valid
    // HandleScope. This method does not wait for timers, network, or I/O.
    // A rejected Promise is reported as Rejected, not thrown as a C++ error.
    // Exceptions from settlement callbacks propagate to the caller, but
    // terminal continuations are still removed.
    V8PromisePumpResult Pump() {
        CheckOwner();
        CheckNotPumping();
        PumpGuard guard(pumping_);

        V8PromisePumpResult result;
        if (continuations_.empty()) {
            return result;
        }

        for (std::size_t i = 0; i < config_.checkpoints_per_pump; ++i) {
            isolate_->PerformMicrotaskCheckpoint();
            ++result.checkpoints;
        }

        std::vector<std::uint64_t> ids;
        ids.reserve(continuations_.size());
        for (const auto& entry : continuations_) {
            ids.push_back(entry.first);
        }

        for (const auto id : ids) {
            auto it = continuations_.find(id);
            if (it == continuations_.end()) {
                continue;
            }

            // Poll() transitions to terminal state before calling the user
            // callback. If the callback throws, erase before propagating.
            try {
                if (it->second->Poll()) {
                    continuations_.erase(it);
                    ++result.settled;
                }
            } catch (...) {
                if (!it->second->Pending()) {
                    continuations_.erase(it);
                }
                throw;
            }
        }

        result.pending = continuations_.size();
        return result;
    }

    // Cooperative cancellation: does not interrupt running JavaScript.
    // Returns false when the continuation is not present.
    bool Cancel(std::uint64_t id) {
        CheckOwner();
        CheckNotPumping();
        auto it = continuations_.find(id);
        if (it == continuations_.end()) {
            return false;
        }
        try {
            (void)it->second->Cancel();
        } catch (...) {
            if (!it->second->Pending()) {
                continuations_.erase(it);
            }
            throw;
        }
        continuations_.erase(it);
        return true;
    }

    // Explicitly publish Abandoned for each pending continuation. If a
    // callback throws, the affected continuation is still removed and the
    // exception propagates; the caller may call AbandonAll() again.
    std::size_t AbandonAll() {
        CheckOwner();
        CheckNotPumping();
        std::size_t abandoned = 0;
        while (!continuations_.empty()) {
            auto it = continuations_.begin();
            try {
                (void)it->second->Abandon();
            } catch (...) {
                if (!it->second->Pending()) {
                    continuations_.erase(it);
                }
                throw;
            }
            continuations_.erase(it);
            ++abandoned;
        }
        return abandoned;
    }

private:
    class PumpGuard final {
    public:
        explicit PumpGuard(bool& flag) : flag_(flag) { flag_ = true; }
        ~PumpGuard() { flag_ = false; }
        PumpGuard(const PumpGuard&) = delete;
        PumpGuard& operator=(const PumpGuard&) = delete;
    private:
        bool& flag_;
    };

    void CheckOwner() const {
        if (std::this_thread::get_id() != owner_) {
            throw std::logic_error("Promise scheduler accessed from a foreign thread");
        }
    }

    void CheckNotPumping() const {
        if (pumping_) {
            throw std::logic_error("Promise scheduler mutation during settlement callback");
        }
    }

    ::v8::Isolate* isolate_;
    std::thread::id owner_;
    V8PromiseSchedulerConfig config_;
    std::unordered_map<std::uint64_t, std::unique_ptr<V8PromiseContinuation>> continuations_;
    bool pumping_ = false;
};

} // namespace lamp::execution
