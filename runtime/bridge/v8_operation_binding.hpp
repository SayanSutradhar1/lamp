
#pragma once

// ============================================================
// Lamp Framework
// Phase 4 - Native Execution Bridge
//
// File: runtime/bridge/v8_operation_binding.hpp
//
// Purpose:
//   Execute registered Lamp operations inside V8.
//
// Dependencies:
//   - C++20
//   - V8
//   - execution_dispatcher.hpp
//
// Execution model:
//   - Isolate-local.
//   - Must be called on the owning V8 thread.
//   - Does not create or destroy isolates.
//   - Does not own the V8 platform.
//   - Does not manage an isolate pool.
//   - Does not expose HTTP request/response objects.
//
// Current supported operation contract:
//   - JSON input.
//   - Synchronous JavaScript functions.
//   - JSON output.
//
// Deferred:
//   - Promise-based operations.
//   - Protobuf codecs.
//   - Binary ArrayBuffer operations.
//   - Isolate-pool scheduling.
//   - Asynchronous cancellation.
// ============================================================

#include "execution_dispatcher.hpp"

#include <v8.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lamp::bridge {

// ============================================================
// V8 operation binding
// ============================================================

class V8OperationBinding final
    : public IJavaScriptOperationExecutor {

public:
    // --------------------------------------------------------
    // Construction
    // --------------------------------------------------------

    // The caller owns the isolate and its lifetime.
    //
    // Construction and destruction must occur on the
    // isolate's owning thread.
    //
    // The isolate must outlive this binding.
    //
    // A valid context must belong to the supplied isolate.

    V8OperationBinding(
        v8::Isolate* isolate,
        v8::Local<v8::Context> context
    )
        : isolate_(isolate),
          owner_thread_(std::this_thread::get_id()) {
        if (isolate_ != nullptr && !context.IsEmpty()) {
            context_.Reset(isolate_, context);
        }
    }

    ~V8OperationBinding() override {
        // The owner must destroy this object on the isolate
        // thread before disposing the isolate.
        //
        // Do not invoke this destructor concurrently with
        // Execute(), Register(), or Unregister().
        ClearUnsafe();
    }

    V8OperationBinding(
        const V8OperationBinding&
    ) = delete;

    V8OperationBinding& operator=(
        const V8OperationBinding&
    ) = delete;

    // --------------------------------------------------------
    // Register JavaScript operation
    // --------------------------------------------------------

    // Register an already-compiled JavaScript function.
    //
    // Handler IDs are supplied by Lamp's application
    // registration layer and must be nonzero.
    //
    // The binding stores a persistent V8 function handle.
    //
    // This method must be called with the correct isolate
    // entered and on the owning isolate thread.

    [[nodiscard]]
    bool Register(
        JavaScriptHandlerReference reference,
        v8::Local<v8::Function> function
    ) {
        if (!OnOwnerThread() ||
            isolate_ == nullptr ||
            context_.IsEmpty() ||
            !reference.IsValid() ||
            function.IsEmpty()) {
            return false;
        }

        if (handlers_.contains(reference.handler_id)) {
            return false;
        }

        auto entry = std::make_unique<HandlerEntry>();

        entry->function.Reset(isolate_, function);

        handlers_.emplace(
            reference.handler_id,
            std::move(entry)
        );

        return true;
    }

    // --------------------------------------------------------
    // Unregister JavaScript operation
    // --------------------------------------------------------

    [[nodiscard]]
    bool Unregister(
        JavaScriptHandlerReference reference
    ) {
        if (!OnOwnerThread() ||
            !reference.IsValid()) {
            return false;
        }

        const auto it =
            handlers_.find(reference.handler_id);

        if (it == handlers_.end()) {
            return false;
        }

        handlers_.erase(it);
        return true;
    }

    // --------------------------------------------------------
    // Handler lookup
    // --------------------------------------------------------

    [[nodiscard]]
    bool Contains(
        JavaScriptHandlerReference reference
    ) const noexcept {
        if (!OnOwnerThread() ||
            !reference.IsValid()) {
            return false;
        }

        return handlers_.contains(reference.handler_id);
    }

    // --------------------------------------------------------
    // Execute operation
    // --------------------------------------------------------

    [[nodiscard]]
    ResultEnvelope Execute(
        JavaScriptHandlerReference reference,
        const RequestEnvelope& request
    ) override {
        if (!OnOwnerThread()) {
            return Failure(
                request,
                OperationErrorCode::Unavailable,
                "V8_WRONG_THREAD",
                "JavaScript execution was requested "
                "from a non-owner thread"
            );
        }

        if (isolate_ == nullptr ||
            context_.IsEmpty()) {
            return Failure(
                request,
                OperationErrorCode::Unavailable,
                "V8_UNAVAILABLE",
                "JavaScript runtime is unavailable"
            );
        }

        if (!reference.IsValid()) {
            return Failure(
                request,
                OperationErrorCode::NotFound,
                "INVALID_JS_HANDLER",
                "Invalid JavaScript handler reference"
            );
        }

        const auto handler_it =
            handlers_.find(reference.handler_id);

        if (handler_it == handlers_.end()) {
            return Failure(
                request,
                OperationErrorCode::NotFound,
                "JS_HANDLER_NOT_FOUND",
                "JavaScript operation handler was not found"
            );
        }

        if (request.IsCancelled()) {
            return ResultEnvelope::Cancelled(request);
        }

        if (request.IsExpired()) {
            return ResultEnvelope::DeadlineExceeded(
                request
            );
        }

        // Only JSON is supported by this first binding.
        //
        // Auto is accepted for an empty input payload,
        // where the handler receives undefined.

        if (request.format != SerializationFormat::Json &&
            !(request.format ==
                  SerializationFormat::Auto &&
              request.payload.empty())) {
            return Failure(
                request,
                OperationErrorCode::SerializationFailed,
                "UNSUPPORTED_JS_INPUT_FORMAT",
                "JavaScript operation currently requires "
                "JSON input"
            );
        }

        v8::HandleScope handle_scope(isolate_);

        auto context = context_.Get(isolate_);

        if (context.IsEmpty()) {
            return Failure(
                request,
                OperationErrorCode::Unavailable,
                "V8_CONTEXT_UNAVAILABLE",
                "JavaScript context is unavailable"
            );
        }

        v8::Context::Scope context_scope(context);

        v8::TryCatch try_catch(isolate_);

        auto function =
            handler_it->second->function.Get(isolate_);

        if (function.IsEmpty()) {
            return Failure(
                request,
                OperationErrorCode::Unavailable,
                "V8_FUNCTION_UNAVAILABLE",
                "JavaScript handler is unavailable"
            );
        }

        // ----------------------------------------------------
        // Decode input
        // ----------------------------------------------------

        v8::Local<v8::Value> input =
            v8::Undefined(isolate_);

        if (!request.payload.empty()) {
            auto json_text =
                v8::String::NewFromUtf8(
                    isolate_,
                    reinterpret_cast<const char*>(
                        request.payload.data()
                    ),
                    v8::NewStringType::kNormal,
                    static_cast<int>(
                        request.payload.size()
                    )
                );

            if (json_text.IsEmpty()) {
                return Failure(
                    request,
                    OperationErrorCode::SerializationFailed,
                    "JSON_INPUT_DECODE_FAILED",
                    "Unable to decode JSON input"
                );
            }

            if (!v8::JSON::Parse(
                    context,
                    json_text.ToLocalChecked()
                ).ToLocal(&input)) {
                return Failure(
                    request,
                    OperationErrorCode::InvalidArgument,
                    "INVALID_JSON_INPUT",
                    "Operation input contains invalid JSON"
                );
            }
        }

        // ----------------------------------------------------
        // Execute handler
        // ----------------------------------------------------

        v8::Local<v8::Value> arguments[] = {
            input
        };

        v8::Local<v8::Value> output;

        if (!function->Call(
                context,
                v8::Undefined(isolate_),
                1,
                arguments
            ).ToLocal(&output)) {
            return Failure(
                request,
                OperationErrorCode::ExecutionFailed,
                "JS_OPERATION_EXCEPTION",
                "JavaScript operation execution failed"
            );
        }

        // This synchronous binding cannot safely resolve
        // promises. A future asynchronous executor will
        // own promise settlement and isolate scheduling.

        if (output->IsPromise()) {
            return Failure(
                request,
                OperationErrorCode::NotImplemented,
                "ASYNC_JS_OPERATION_UNSUPPORTED",
                "Promise-returning operations require "
                "the asynchronous V8 executor"
            );
        }

        // ----------------------------------------------------
        // Encode result
        // ----------------------------------------------------

        // A handler returning undefined is represented as
        // an empty result payload.
        //
        // All other values must be JSON-serializable.

        ResultEnvelope::Payload encoded;

        if (!output->IsUndefined()) {
            v8::Local<v8::String> json_output;

            if (!v8::JSON::Stringify(
                    context,
                    output
                ).ToLocal(&json_output)) {
                return Failure(
                    request,
                    OperationErrorCode::SerializationFailed,
                    "JSON_OUTPUT_ENCODE_FAILED",
                    "Unable to serialize operation result"
                );
            }

            v8::String::Utf8Value utf8(
                isolate_,
                json_output
            );

            if (*utf8 == nullptr) {
                return Failure(
                    request,
                    OperationErrorCode::SerializationFailed,
                    "JSON_OUTPUT_ENCODE_FAILED",
                    "Unable to encode JSON output as UTF-8"
                );
            }

            const auto* begin =
                reinterpret_cast<const std::uint8_t*>(
                    *utf8
                );

            encoded.assign(
                begin,
                begin + utf8.length()
            );
        }

        // ----------------------------------------------------
        // Post-execution lifecycle
        // ----------------------------------------------------

        if (request.IsCancelled()) {
            return ResultEnvelope::Cancelled(request);
        }

        if (request.IsExpired()) {
            return ResultEnvelope::DeadlineExceeded(
                request
            );
        }

        return ResultEnvelope::Success(
            request,
            std::move(encoded),
            SerializationFormat::Json,
            ExecutionBackend::JavaScript
        );
    }

    // --------------------------------------------------------
    // Clear handlers
    // --------------------------------------------------------

    // Must run on the owning isolate thread.
    //
    // Existing operation registrations must be retired
    // before their handler references are released.

    [[nodiscard]]
    bool Clear() {
        if (!OnOwnerThread()) {
            return false;
        }

        ClearUnsafe();
        return true;
    }

    // --------------------------------------------------------
    // Introspection
    // --------------------------------------------------------

    [[nodiscard]]
    std::size_t HandlerCount() const noexcept {
        if (!OnOwnerThread()) {
            return 0;
        }

        return handlers_.size();
    }

private:
    // ========================================================
    // Persistent handler entry
    // ========================================================

    struct HandlerEntry final {
        v8::Global<v8::Function> function;

        HandlerEntry() = default;

        HandlerEntry(const HandlerEntry&) = delete;

        HandlerEntry& operator=(const HandlerEntry&) =
            delete;
    };

    // ========================================================
    // Thread ownership
    // ========================================================

    [[nodiscard]]
    bool OnOwnerThread() const noexcept {
        return std::this_thread::get_id() ==
               owner_thread_;
    }

    // ========================================================
    // Failure factory
    // ========================================================

    [[nodiscard]]
    static ResultEnvelope Failure(
        const RequestEnvelope& request,
        OperationErrorCode code,
        std::string identifier,
        std::string message
    ) {
        OperationError error;

        error.code = code;
        error.identifier = std::move(identifier);
        error.message = std::move(message);

        return ResultEnvelope::Failure(
            request,
            std::move(error),
            ExecutionBackend::JavaScript
        );
    }

    // ========================================================
    // Internal cleanup
    // ========================================================

    void ClearUnsafe() noexcept {
        handlers_.clear();
        context_.Reset();
    }

    // ========================================================
    // State
    // ========================================================

    v8::Isolate* isolate_ = nullptr;

    std::thread::id owner_thread_;

    v8::Global<v8::Context> context_;

    std::unordered_map<
        std::uint64_t,
        std::unique_ptr<HandlerEntry>
    > handlers_;
};

} // namespace lamp::bridge
