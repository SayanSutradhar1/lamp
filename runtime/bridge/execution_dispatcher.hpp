
#pragma once

// ============================================================
// Lamp Framework
// Phase 4 - Native Execution Bridge
//
// File: runtime/bridge/execution_dispatcher.hpp
//
// Purpose:
//   Resolve, validate, admit and execute Lamp operations.
//
// Dependencies:
//   - C++20 standard library
//   - operation_registry.hpp
//
// Design:
//   - Transport-independent.
//   - No V8 or HTTP headers.
//   - Native fast path without JavaScript.
//   - Pluggable JavaScript executor.
//   - Explicit backend selection.
//   - Thread-safe admission control.
//   - Cooperative cancellation.
//   - Structured exception conversion.
//
// Current scope:
//   - Synchronous dispatch.
//   - No request queue.
//   - No automatic retries.
//   - No cache implementation.
//   - No adaptive load-balancing heuristics.
//
// Important:
//   Synchronous execution cannot forcibly stop an
//   uncooperative handler when its deadline expires.
//   The executor must enforce interruption separately.
// ============================================================

#include "operation_cache_interface.hpp"
#include "operation_registry.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace lamp::bridge {

// ============================================================
// JavaScript execution interface
// ============================================================

// Implemented later by the V8 operation binding.
//
// The dispatcher never stores or manipulates V8 handles.
//
// Execute() must be safe to call according to the executor's
// documented threading model. A V8 implementation should
// marshal execution to the appropriate isolate thread.

class IJavaScriptOperationExecutor {
public:
    virtual ~IJavaScriptOperationExecutor() = default;

    [[nodiscard]]
    virtual ResultEnvelope Execute(
        JavaScriptHandlerReference handler,
        const RequestEnvelope& request
    ) = 0;
};

// ============================================================
// Dispatcher configuration
// ============================================================

enum class AdaptiveBackendPreference : std::uint8_t {
    PreferNative = 0,
    PreferJavaScript = 1
};

struct ExecutionDispatcherConfig final {
    AdaptiveBackendPreference adaptive_preference =
        AdaptiveBackendPreference::PreferNative;

    // Default concurrency limit when the descriptor does
    // not specify an operation-specific limit.
    //
    // Zero means unlimited at this dispatcher layer.
    std::uint32_t default_max_concurrency = 0;
};

// ============================================================
// Dispatcher
// ============================================================

class ExecutionDispatcher final {
public:
    using RevalidationCallback =
        std::function<void(RequestEnvelope)>;

    explicit ExecutionDispatcher(
        std::shared_ptr<OperationRegistry> registry,
        std::shared_ptr<IJavaScriptOperationExecutor>
            javascript_executor = {},
        ExecutionDispatcherConfig config = {},
        std::shared_ptr<IOperationCache> cache = {},
        RevalidationCallback revalidation_callback = {}
    )
        : registry_(std::move(registry)),
          javascript_executor_(
              std::move(javascript_executor)
          ),
          config_(config),
          cache_(std::move(cache)),
          revalidation_callback_(
              std::move(revalidation_callback)
          ) {}

    void SetCache(std::shared_ptr<IOperationCache> cache) {
        std::lock_guard lock(cache_mutex_);
        cache_ = std::move(cache);
    }

    void SetRevalidationCallback(
        RevalidationCallback callback
    ) {
        std::lock_guard lock(cache_mutex_);
        revalidation_callback_ = std::move(callback);
    }

    ExecutionDispatcher(
        const ExecutionDispatcher&
    ) = delete;

    ExecutionDispatcher& operator=(
        const ExecutionDispatcher&
    ) = delete;

    // --------------------------------------------------------
    // Dispatch
    // --------------------------------------------------------

    [[nodiscard]]
    ResultEnvelope Dispatch(
        const RequestEnvelope& request
    ) {
        return DispatchImpl(request, true);
    }

    // Executes the operation bypassing cache lookup, while still
    // updating the cache on a successful response (used for
    // background stale-while-revalidate refreshes).
    [[nodiscard]]
    ResultEnvelope DispatchFresh(
        const RequestEnvelope& request
    ) {
        return DispatchImpl(request, false);
    }

private:
    [[nodiscard]]
    ResultEnvelope DispatchImpl(
        const RequestEnvelope& request,
        bool allow_cache_lookup
    ) {
        using Clock = std::chrono::steady_clock;

        const auto started = Clock::now();

        // The registry is required.
        if (!registry_) {
            return Finalize(
                MakeFailure(
                    request,
                    OperationErrorCode::Unavailable,
                    "REGISTRY_UNAVAILABLE",
                    "Operation registry is unavailable",
                    true
                ),
                started
            );
        }

        // Resolve immutable registration snapshot.
        const auto registration =
            registry_->Find(request.operation_id);

        if (!registration) {
            return Finalize(
                MakeFailure(
                    request,
                    OperationErrorCode::NotFound,
                    "OPERATION_NOT_FOUND",
                    "Requested operation was not found"
                ),
                started
            );
        }

        const auto& descriptor =
            registration->descriptor;

        // Validate the request against the descriptor.
        if (const auto error =
                request.Validate(&descriptor)) {
            return Finalize(
                ValidationFailure(
                    request,
                    error->code,
                    error->message
                ),
                started
            );
        }

        // A zero descriptor timeout means that only the
        // request deadline and runtime-wide policies apply.
        //
        // The effective deadline is the earlier of:
        //   - the request's existing deadline
        //   - the descriptor's timeout from dispatch start
        //
        // The envelope is copied so its deadline can be
        // narrowed without mutating caller-owned state.

        RequestEnvelope invocation = request;

        if (descriptor.limits.timeout.count() > 0) {
            const auto timeout =
                std::chrono::duration_cast<
                    Clock::duration
                >(descriptor.limits.timeout);

            const auto remaining =
                Clock::time_point::max() - started;

            const auto operation_deadline =
                timeout >= remaining
                    ? Clock::time_point::max()
                    : started + timeout;

            if (!invocation.deadline.expires_at ||
                operation_deadline <
                    *invocation.deadline.expires_at) {
                invocation.deadline.expires_at =
                    operation_deadline;
            }
        }

        if (invocation.IsCancelled()) {
            return Finalize(
                ResultEnvelope::Cancelled(invocation),
                started
            );
        }

        if (invocation.IsExpired()) {
            return Finalize(
                ResultEnvelope::DeadlineExceeded(
                    invocation
                ),
                started
            );
        }

        // ----------------------------------------------------
        // Cache lookup
        // ----------------------------------------------------

        std::shared_ptr<IOperationCache> cacheSnapshot;
        RevalidationCallback revalSnapshot;
        {
            std::lock_guard lock(cache_mutex_);
            cacheSnapshot = cache_;
            revalSnapshot = revalidation_callback_;
        }

        const bool useCache =
            cacheSnapshot && descriptor.IsCacheEligible();

        if (useCache && allow_cache_lookup) {
            try {
                auto lookup = cacheSnapshot->Lookup(
                    descriptor,
                    invocation
                );

                if ((lookup.status == CacheResult::HitLocal ||
                     lookup.status == CacheResult::HitShared ||
                     lookup.status == CacheResult::Stale) &&
                    lookup.result.has_value()) {
                    ResultEnvelope hit =
                        std::move(*lookup.result);
                    hit.request_id = invocation.request_id;
                    hit.trace_id = invocation.trace_id;
                    hit.span_id = invocation.span_id;
                    hit.backend = ExecutionBackend::Cache;
                    hit.cache_result = lookup.status;

                    if (lookup.needs_revalidation &&
                        revalSnapshot) {
                        try {
                            revalSnapshot(invocation);
                        } catch (...) {
                        }
                    }

                    return Finalize(std::move(hit), started);
                }
            } catch (...) {
                // Cache lookup failures fall back to live execution.
            }
        }

        // ----------------------------------------------------
        // Admission control
        // ----------------------------------------------------

        const auto concurrency_limit =
            EffectiveConcurrencyLimit(descriptor);

        AdmissionGuard admission(
            *this,
            descriptor.id,
            concurrency_limit
        );

        if (!admission.Acquired()) {
            return Finalize(
                MakeFailure(
                    invocation,
                    OperationErrorCode::ResourceExhausted,
                    "OPERATION_BUSY",
                    "Operation concurrency limit exceeded",
                    true
                ),
                started
            );
        }

        // ----------------------------------------------------
        // Backend selection
        // ----------------------------------------------------

        const auto backend =
            SelectBackend(*registration);

        if (backend == ExecutionBackend::Unknown) {
            return Finalize(
                MakeFailure(
                    invocation,
                    OperationErrorCode::Unavailable,
                    "EXECUTION_BACKEND_UNAVAILABLE",
                    "No eligible execution backend is "
                    "currently available",
                    true
                ),
                started
            );
        }

        if (invocation.IsCancelled()) {
            return Finalize(
                ResultEnvelope::Cancelled(invocation),
                started
            );
        }

        if (invocation.IsExpired()) {
            return Finalize(
                ResultEnvelope::DeadlineExceeded(
                    invocation
                ),
                started
            );
        }

        // ----------------------------------------------------
        // Execute
        // ----------------------------------------------------

        const auto execution_started = Clock::now();

        ResultEnvelope result;

        try {
            switch (backend) {
                case ExecutionBackend::Native:
                    result =
                        registration->backends.native
                            ->Execute(invocation);
                    break;

                case ExecutionBackend::JavaScript:
                    result =
                        javascript_executor_->Execute(
                            registration->backends.javascript,
                            invocation
                        );
                    break;

                default:
                    result = MakeFailure(
                        invocation,
                        OperationErrorCode::Internal,
                        "INVALID_EXECUTION_BACKEND",
                        "Dispatcher selected an invalid "
                        "execution backend"
                    );
                    break;
            }
        } catch (const std::exception&) {
            // Do not expose exception::what() to clients.
            result = MakeFailure(
                invocation,
                OperationErrorCode::ExecutionFailed,
                "OPERATION_EXECUTION_FAILED",
                "Operation execution failed"
            );
        } catch (...) {
            result = MakeFailure(
                invocation,
                OperationErrorCode::ExecutionFailed,
                "OPERATION_EXECUTION_FAILED",
                "Operation execution failed"
            );
        }

        const auto execution_finished = Clock::now();

        result.metrics.execution_time =
            std::chrono::duration_cast<
                std::chrono::nanoseconds
            >(execution_finished - execution_started);

        // ----------------------------------------------------
        // Validate handler output
        // ----------------------------------------------------

        // A handler cannot change the identity of the
        // operation invocation.
        if (result.request_id != invocation.request_id ||
            result.operation_id != invocation.operation_id ||
            result.trace_id != invocation.trace_id ||
            result.span_id != invocation.span_id) {
            result = MakeFailure(
                invocation,
                OperationErrorCode::Internal,
                "INVALID_HANDLER_RESULT",
                "Operation handler returned an invalid "
                "result identity"
            );
        }

        if (const auto error =
                result.Validate(&descriptor)) {
            result = MakeFailure(
                invocation,
                OperationErrorCode::Internal,
                "INVALID_HANDLER_RESULT",
                "Operation handler returned an invalid "
                "result envelope"
            );
        }

        // The actual backend is determined by the
        // dispatcher, not by the handler.
        result.backend = backend;

        // ----------------------------------------------------
        // Post-execution lifecycle
        // ----------------------------------------------------

        // This is a best-effort lifecycle check.
        //
        // An operation may have performed side effects
        // before cancellation or deadline expiration.
        //
        // No automatic retry or rollback is attempted.

        if (invocation.IsCancelled()) {
            auto cancelled =
                ResultEnvelope::Cancelled(invocation);

            cancelled.backend = backend;

            cancelled.metrics.execution_time =
                result.metrics.execution_time;

            result = std::move(cancelled);
        } else if (invocation.IsExpired()) {
            auto expired =
                ResultEnvelope::DeadlineExceeded(
                    invocation
                );

            expired.backend = backend;

            expired.metrics.execution_time =
                result.metrics.execution_time;

            result = std::move(expired);
        }

        if (useCache) {
            result.cache_result = CacheResult::Miss;

            if (result.IsSuccess()) {
                try {
                    cacheSnapshot->Store(
                        descriptor,
                        invocation,
                        result
                    );
                } catch (...) {
                    // Cache population errors must not fail
                    // an otherwise successful operation.
                }
            }
        }

        return Finalize(std::move(result), started);
    }

    // ========================================================
    // Admission guard
    // ========================================================

    // Admission counters are keyed by operation ID.
    //
    // The guard releases its slot on all exit paths,
    // including exceptions.

    class AdmissionGuard final {
    public:
        AdmissionGuard(
            ExecutionDispatcher& dispatcher,
            const OperationId& operation_id,
            std::uint32_t limit
        )
            : dispatcher_(&dispatcher),
              operation_id_(operation_id),
              acquired_(
                  dispatcher.TryAcquire(
                      operation_id_,
                      limit
                  )
              ) {}

        ~AdmissionGuard() {
            if (acquired_) {
                dispatcher_->Release(operation_id_);
            }
        }

        AdmissionGuard(const AdmissionGuard&) = delete;

        AdmissionGuard& operator=(
            const AdmissionGuard&
        ) = delete;

        [[nodiscard]]
        bool Acquired() const noexcept {
            return acquired_;
        }

    private:
        ExecutionDispatcher* dispatcher_;
        OperationId operation_id_;
        bool acquired_;
    };

    // ========================================================
    // Admission control
    // ========================================================

    [[nodiscard]]
    bool TryAcquire(
        const OperationId& id,
        std::uint32_t limit
    ) {
        std::lock_guard lock(admission_mutex_);

        auto& active = active_operations_[id];

        if (limit != 0 && active >= limit) {
            return false;
        }

        ++active;
        return true;
    }

    void Release(
        const OperationId& id
    ) noexcept {
        std::lock_guard lock(admission_mutex_);

        const auto it = active_operations_.find(id);

        if (it == active_operations_.end()) {
            return;
        }

        if (it->second > 1) {
            --it->second;
        } else {
            active_operations_.erase(it);
        }
    }

    [[nodiscard]]
    std::uint32_t EffectiveConcurrencyLimit(
        const OperationDescriptor& descriptor
    ) const noexcept {
        if (descriptor.execution.concurrency ==
                ConcurrencyMode::Serialized) {
            return 1;
        }

        if (descriptor.limits.max_concurrency != 0) {
            return descriptor.limits.max_concurrency;
        }

        return config_.default_max_concurrency;
    }

    // ========================================================
    // Backend selection
    // ========================================================

    [[nodiscard]]
    ExecutionBackend SelectBackend(
        const OperationRegistration& registration
    ) const noexcept {
        const auto& descriptor =
            registration.descriptor;

        const auto& backends =
            registration.backends;

        const bool native_available =
            backends.HasNative();

        const bool javascript_available =
            backends.HasJavaScript() &&
            static_cast<bool>(javascript_executor_);

        switch (descriptor.execution.mode) {
            case ExecutionMode::Native:
                return native_available
                    ? ExecutionBackend::Native
                    : ExecutionBackend::Unknown;

            case ExecutionMode::JavaScript:
                return javascript_available
                    ? ExecutionBackend::JavaScript
                    : ExecutionBackend::Unknown;

            case ExecutionMode::Adaptive:
                break;
        }

        // Adaptive selection is deliberately deterministic
        // in this first implementation.
        //
        // Future scheduling policy may use measured load,
        // queue pressure and backend health.

        if (config_.adaptive_preference ==
                AdaptiveBackendPreference::PreferNative) {
            if (native_available) {
                return ExecutionBackend::Native;
            }

            if (javascript_available) {
                return ExecutionBackend::JavaScript;
            }
        } else {
            if (javascript_available) {
                return ExecutionBackend::JavaScript;
            }

            if (native_available) {
                return ExecutionBackend::Native;
            }
        }

        return ExecutionBackend::Unknown;
    }

    // ========================================================
    // Failure helpers
    // ========================================================

    [[nodiscard]]
    static ResultEnvelope MakeFailure(
        const RequestEnvelope& request,
        OperationErrorCode code,
        std::string identifier,
        std::string message,
        bool retryable = false
    ) {
        OperationError error;

        error.code = code;
        error.identifier = std::move(identifier);
        error.message = std::move(message);
        error.retryable = retryable;

        return ResultEnvelope::Failure(
            request,
            std::move(error)
        );
    }

    [[nodiscard]]
    static ResultEnvelope ValidationFailure(
        const RequestEnvelope& request,
        RequestValidationCode code,
        const std::string& message
    ) {
        switch (code) {
            case RequestValidationCode::Cancelled:
                return ResultEnvelope::Cancelled(request);

            case RequestValidationCode::ExpiredDeadline:
                return ResultEnvelope::DeadlineExceeded(
                    request
                );

            default:
                return MakeFailure(
                    request,
                    OperationErrorCode::InvalidArgument,
                    "INVALID_OPERATION_REQUEST",
                    message
                );
        }
    }

    // ========================================================
    // Finalization
    // ========================================================

    [[nodiscard]]
    static ResultEnvelope Finalize(
        ResultEnvelope result,
        std::chrono::steady_clock::time_point started
    ) noexcept {
        const auto finished =
            std::chrono::steady_clock::now();

        result.metrics.total_time =
            std::chrono::duration_cast<
                std::chrono::nanoseconds
            >(finished - started);

        return result;
    }

    // ========================================================
    // State
    // ========================================================

    std::shared_ptr<OperationRegistry> registry_;

    std::shared_ptr<IJavaScriptOperationExecutor>
        javascript_executor_;

    ExecutionDispatcherConfig config_;

    mutable std::mutex cache_mutex_;
    std::shared_ptr<IOperationCache> cache_;
    RevalidationCallback revalidation_callback_;

    mutable std::mutex admission_mutex_;

    std::unordered_map<OperationId, std::uint64_t>
        active_operations_;
};

} // namespace lamp::bridge
