
#pragma once

// ============================================================
// Lamp Framework
// Phase 4 - Native Execution Bridge
//
// File: runtime/bridge/operation_descriptor.hpp
//
// Purpose:
//   Transport-independent operation metadata and policies.
//
// Dependencies:
//   C++20 standard library only.
//
// Design:
//   - No V8 dependency.
//   - No HTTP dependency.
//   - No Protobuf dependency.
//   - No owning runtime pointers.
//   - Value semantics.
//   - Explicit validation.
//   - Deterministic operation identity.
// ============================================================

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lamp::bridge {

// ============================================================
// Operation identity
// ============================================================

// Stable logical identifier used by the operation registry.
//
// Examples:
//   users.get
//   users.create
//   analytics.aggregate
//   system.health
//
// Operation identifiers are transport-independent.
//
// A transport adapter may map:
//   HTTP GET /users/123 -> users.get
//
// But that mapping is not part of the operation identity.

using OperationId = std::string;

// ============================================================
// Execution policy
// ============================================================

enum class ExecutionMode : std::uint8_t {
    // Dispatcher chooses an eligible execution backend.
    Adaptive = 0,

    // Execute through a registered C++ native handler.
    Native = 1,

    // Execute through an embedded V8 isolate.
    JavaScript = 2
};

// Determines whether the operation may execute concurrently.
//
// This is a scheduling constraint, not an automatic guarantee
// that a handler implementation is thread-safe.

enum class ConcurrencyMode : std::uint8_t {
    // Scheduler may run independent invocations concurrently.
    Concurrent = 0,

    // Scheduler must serialize invocations of this operation.
    Serialized = 1
};

// ============================================================
// Serialization policy
// ============================================================

enum class SerializationFormat : std::uint8_t {
    // Negotiate a mutually supported format.
    Auto = 0,

    // Protobuf-compatible binary representation.
    Protobuf = 1,

    // UTF-8 JSON representation.
    Json = 2,

    // Uninterpreted binary payload.
    Binary = 3
};

// ============================================================
// Cache policy
// ============================================================

enum class CacheMode : std::uint8_t {
    Disabled = 0,

    // Process-local cache.
    Local = 1,

    // Local cache with optional shared L2 lookup.
    Layered = 2
};

struct CachePolicy final {
    CacheMode mode = CacheMode::Disabled;

    std::chrono::milliseconds ttl{0};

    // Maximum serialized result size eligible for caching.
    //
    // Zero means no descriptor-level limit. Global runtime
    // limits still apply.
    std::size_t max_entry_bytes = 0;

    // When enabled, the runtime may reuse a cached result
    // while a background refresh is being performed.
    //
    // The actual refresh lifecycle belongs to the cache
    // implementation, not to this descriptor.
    bool stale_while_revalidate = false;

    [[nodiscard]]
    bool Enabled() const noexcept {
        return mode != CacheMode::Disabled;
    }
};

// ============================================================
// Resource limits
// ============================================================

struct OperationLimits final {
    // Maximum accepted request payload size.
    std::size_t max_input_bytes = 8U * 1024U * 1024U;

    // Maximum produced response payload size.
    //
    // Streaming output requires a separate policy and is not
    // implied by this limit.
    std::size_t max_output_bytes = 16U * 1024U * 1024U;

    // Maximum time allowed for one operation invocation.
    //
    // Zero means no operation-specific timeout.
    // A runtime-wide deadline may still apply.
    std::chrono::milliseconds timeout{30'000};

    // Maximum simultaneous invocations of this operation.
    //
    // Zero means the runtime-wide limit applies.
    std::uint32_t max_concurrency = 0;

    // Maximum queued invocations waiting for capacity.
    //
    // Zero means no operation-specific queue allowance;
    // the dispatcher may still apply global admission rules.
    std::uint32_t max_queue_depth = 0;
};

// ============================================================
// Execution policy
// ============================================================

struct ExecutionPolicy final {
    ExecutionMode mode = ExecutionMode::Adaptive;

    ConcurrencyMode concurrency =
        ConcurrencyMode::Concurrent;

    // Eligible operations may be moved to a native worker.
    //
    // This flag does not mean that arbitrary JavaScript
    // functions can run directly on C++ worker threads.
    bool allow_worker_execution = true;

    // When true, an eligible handler may run on the
    // dispatcher's current execution thread.
    //
    // The scheduler must still respect latency budgets.
    bool allow_inline_execution = false;

    // Controls whether the dispatcher may select a
    // different registered backend when the preferred
    // backend is unavailable.
    bool allow_backend_fallback = false;
};

// ============================================================
// Schema metadata
// ============================================================

// Identifies an application-level schema without embedding
// any specific schema library into the native bridge.
//
// The schema registry will resolve these identifiers later.

struct SchemaReference final {
    std::string name;
    std::uint32_t version = 1;

    [[nodiscard]]
    bool Empty() const noexcept {
        return name.empty();
    }
};

// ============================================================
// Operation capabilities
// ============================================================

struct OperationCapabilities final {
    // Indicates that repeating the same invocation should
    // not cause additional externally visible effects.
    bool idempotent = false;

    // Indicates that the operation is intended to be
    // observational rather than mutating application state.
    //
    // This is a declaration by the application, not a
    // property Lamp can automatically prove.
    bool read_only = false;

    // Indicates that the operation supports a streaming
    // result through a future streaming interface.
    bool streaming = false;

    // Indicates that the operation supports cooperative
    // cancellation.
    bool cancellable = true;
};

// ============================================================
// Validation errors
// ============================================================

enum class OperationValidationCode : std::uint8_t {
    None = 0,
    EmptyId,
    InvalidId,
    InvalidExecutionMode,
    InvalidConcurrencyMode,
    InvalidSerializationFormat,
    InvalidCacheMode,
    InvalidCacheTtl,
    InvalidCacheConfiguration,
    InvalidInputLimit,
    InvalidOutputLimit,
    InvalidTimeout,
    InvalidConcurrencyLimit,
    InvalidSchema,
    InvalidStreamingConfiguration
};

struct OperationValidationError final {
    OperationValidationCode code =
        OperationValidationCode::None;

    std::string message;
};

// ============================================================
// Operation descriptor
// ============================================================

struct OperationDescriptor final {
    // Logical operation identity.
    OperationId id;

    // Human-readable metadata.
    std::string description;

    // Execution configuration.
    ExecutionPolicy execution;

    // Serialization preferences.
    SerializationFormat input_format =
        SerializationFormat::Auto;

    SerializationFormat output_format =
        SerializationFormat::Auto;

    // Caching configuration.
    CachePolicy cache;

    // Resource constraints.
    OperationLimits limits;

    // Schema identifiers.
    SchemaReference input_schema;
    SchemaReference output_schema;

    // Behavioral declarations.
    OperationCapabilities capabilities;

    // Optional application-defined labels.
    //
    // These labels are metadata only. They must not be
    // interpreted as authentication or authorization.
    std::vector<std::string> tags;

    // --------------------------------------------------------
    // Identity validation
    // --------------------------------------------------------

    [[nodiscard]]
    static bool IsValidId(
        std::string_view value
    ) noexcept {
        // A bounded identifier avoids unbounded registry
        // keys, pathological allocations and oversized logs.
        if (value.empty() || value.size() > 128) {
            return false;
        }

        bool previous_separator = true;

        for (unsigned char ch : value) {
            const bool alpha =
                (ch >= 'a' && ch <= 'z') ||
                (ch >= 'A' && ch <= 'Z');

            const bool digit =
                ch >= '0' && ch <= '9';

            const bool separator =
                ch == '.' || ch == '-' || ch == '_';

            if (!alpha && !digit && !separator) {
                return false;
            }

            if (separator) {
                if (previous_separator) {
                    return false;
                }

                previous_separator = true;
            } else {
                previous_separator = false;
            }
        }

        return !previous_separator;
    }

    // --------------------------------------------------------
    // Validation
    // --------------------------------------------------------

    [[nodiscard]]
    std::optional<OperationValidationError>
    Validate() const {
        if (id.empty()) {
            return Error(
                OperationValidationCode::EmptyId,
                "Operation identifier cannot be empty"
            );
        }

        if (!IsValidId(id)) {
            return Error(
                OperationValidationCode::InvalidId,
                "Invalid operation identifier"
            );
        }

        if (!ValidExecutionMode(execution.mode)) {
            return Error(
                OperationValidationCode::InvalidExecutionMode,
                "Invalid operation execution mode"
            );
        }

        if (!ValidConcurrencyMode(
                execution.concurrency)) {
            return Error(
                OperationValidationCode::InvalidConcurrencyMode,
                "Invalid operation concurrency mode"
            );
        }

        if (!ValidSerializationFormat(input_format) ||
            !ValidSerializationFormat(output_format)) {
            return Error(
                OperationValidationCode::
                    InvalidSerializationFormat,
                "Invalid serialization format"
            );
        }

        if (!ValidCacheMode(cache.mode)) {
            return Error(
                OperationValidationCode::InvalidCacheMode,
                "Invalid cache mode"
            );
        }

        if (cache.ttl.count() < 0) {
            return Error(
                OperationValidationCode::InvalidCacheTtl,
                "Cache TTL cannot be negative"
            );
        }

        if (cache.Enabled()) {
            if (cache.ttl.count() <= 0) {
                return Error(
                    OperationValidationCode::InvalidCacheTtl,
                    "Enabled cache requires a positive TTL"
                );
            }

            if (!capabilities.read_only ||
                !capabilities.idempotent) {
                return Error(
                    OperationValidationCode::
                        InvalidCacheConfiguration,
                    "Caching requires a read-only, "
                    "idempotent operation"
                );
            }

            if (capabilities.streaming) {
                return Error(
                    OperationValidationCode::
                        InvalidStreamingConfiguration,
                    "Streaming operations cannot use "
                    "the result cache"
                );
            }
        } else {
            if (cache.stale_while_revalidate) {
                return Error(
                    OperationValidationCode::
                        InvalidCacheConfiguration,
                    "Stale-while-revalidate requires "
                    "an enabled cache"
                );
            }
        }

        if (limits.max_input_bytes == 0) {
            return Error(
                OperationValidationCode::InvalidInputLimit,
                "Maximum input size must be positive"
            );
        }

        if (limits.max_output_bytes == 0) {
            return Error(
                OperationValidationCode::InvalidOutputLimit,
                "Maximum output size must be positive"
            );
        }

        if (limits.timeout.count() < 0) {
            return Error(
                OperationValidationCode::InvalidTimeout,
                "Operation timeout cannot be negative"
            );
        }

        if (execution.concurrency ==
                ConcurrencyMode::Serialized &&
            limits.max_concurrency > 1) {
            return Error(
                OperationValidationCode::
                    InvalidConcurrencyLimit,
                "Serialized operations cannot specify "
                "concurrency greater than one"
            );
        }

        if (!ValidSchema(input_schema) ||
            !ValidSchema(output_schema)) {
            return Error(
                OperationValidationCode::InvalidSchema,
                "Invalid schema reference"
            );
        }

        return std::nullopt;
    }

    [[nodiscard]]
    bool IsValid() const {
        return !Validate().has_value();
    }

    // --------------------------------------------------------
    // Runtime policy helpers
    // --------------------------------------------------------

    [[nodiscard]]
    bool IsCacheEligible() const noexcept {
        return cache.Enabled() &&
               capabilities.read_only &&
               capabilities.idempotent &&
               !capabilities.streaming;
    }

    [[nodiscard]]
    bool RequiresJavaScript() const noexcept {
        return execution.mode == ExecutionMode::JavaScript;
    }

    [[nodiscard]]
    bool RequiresNativeExecution() const noexcept {
        return execution.mode == ExecutionMode::Native;
    }

    [[nodiscard]]
    bool UsesAdaptiveExecution() const noexcept {
        return execution.mode == ExecutionMode::Adaptive;
    }

private:
    [[nodiscard]]
    static std::optional<OperationValidationError>
    Error(
        OperationValidationCode code,
        std::string message
    ) {
        return OperationValidationError{
            code,
            std::move(message)
        };
    }

    [[nodiscard]]
    static bool ValidExecutionMode(
        ExecutionMode mode
    ) noexcept {
        switch (mode) {
            case ExecutionMode::Adaptive:
            case ExecutionMode::Native:
            case ExecutionMode::JavaScript:
                return true;
        }

        return false;
    }

    [[nodiscard]]
    static bool ValidConcurrencyMode(
        ConcurrencyMode mode
    ) noexcept {
        switch (mode) {
            case ConcurrencyMode::Concurrent:
            case ConcurrencyMode::Serialized:
                return true;
        }

        return false;
    }

    [[nodiscard]]
    static bool ValidSerializationFormat(
        SerializationFormat format
    ) noexcept {
        switch (format) {
            case SerializationFormat::Auto:
            case SerializationFormat::Protobuf:
            case SerializationFormat::Json:
            case SerializationFormat::Binary:
                return true;
        }

        return false;
    }

    [[nodiscard]]
    static bool ValidCacheMode(
        CacheMode mode
    ) noexcept {
        switch (mode) {
            case CacheMode::Disabled:
            case CacheMode::Local:
            case CacheMode::Layered:
                return true;
        }

        return false;
    }

    [[nodiscard]]
    static bool ValidSchema(
        const SchemaReference& schema
    ) noexcept {
        // An empty name represents an unspecified schema.
        // An explicit schema must have a positive version.
        return schema.name.empty() ||
               schema.version > 0;
    }
};

} // namespace lamp::bridge
