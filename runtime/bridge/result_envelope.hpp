
#pragma once

// ============================================================
// Lamp Framework
// Phase 4 - Native Execution Bridge
//
// File: runtime/bridge/result_envelope.hpp
//
// Purpose:
//   Transport-independent operation result representation.
//
// Dependencies:
//   - C++20 standard library
//   - operation_descriptor.hpp
//   - request_envelope.hpp
//
// Design:
//   - No V8 dependency.
//   - No HTTP dependency.
//   - Binary-safe owned payload.
//   - Explicit success/failure outcomes.
//   - Structured application and runtime errors.
//   - Execution metrics and cache provenance.
//   - Value semantics.
//   - No native runtime pointers.
//
// Threading:
//   - Independent instances may be used concurrently.
//   - Concurrent mutation of one instance is not safe.
// ============================================================

#include "operation_descriptor.hpp"
#include "request_envelope.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lamp::bridge {

// ============================================================
// Result outcome
// ============================================================

enum class ResultOutcome : std::uint8_t {
    Success = 0,
    Failure = 1,
    Cancelled = 2,
    DeadlineExceeded = 3
};

// ============================================================
// Error classification
// ============================================================

// These codes describe operation-level failures.
//
// Transport adapters are responsible for translating them
// into their protocol-specific representation.
//
// For example:
//   NotFound -> HTTP 404
//   InvalidArgument -> HTTP 400
//
// HTTP status codes are deliberately not part of this API.

enum class OperationErrorCode : std::uint16_t {
    None = 0,

    InvalidArgument = 1,
    Unauthenticated = 2,
    PermissionDenied = 3,
    NotFound = 4,
    AlreadyExists = 5,
    Conflict = 6,
    ResourceExhausted = 7,
    FailedPrecondition = 8,
    Unavailable = 9,
    Internal = 10,
    NotImplemented = 11,

    Cancelled = 12,
    DeadlineExceeded = 13,

    SerializationFailed = 14,
    ExecutionFailed = 15
};

// ============================================================
// Structured error
// ============================================================

struct OperationError final {
    OperationErrorCode code = OperationErrorCode::None;

    // Safe, application-visible message.
    //
    // Internal exception messages, filesystem paths and
    // native stack traces must not be copied here unless
    // explicitly sanitized.
    std::string message;

    // Stable machine-readable error identifier.
    //
    // Examples:
    //   USER_NOT_FOUND
    //   INVALID_INPUT
    //   CACHE_UNAVAILABLE
    std::string identifier;

    // Indicates whether retrying might succeed.
    //
    // A retry is not automatically safe unless the
    // operation is idempotent or has a deduplication
    // mechanism.
    bool retryable = false;

    [[nodiscard]]
    bool HasError() const noexcept {
        return code != OperationErrorCode::None;
    }
};

// ============================================================
// Execution backend
// ============================================================

enum class ExecutionBackend : std::uint8_t {
    Unknown = 0,
    Native = 1,
    JavaScript = 2,
    Cache = 3
};

// ============================================================
// Cache provenance
// ============================================================

enum class CacheResult : std::uint8_t {
    NotChecked = 0,
    Miss = 1,
    HitLocal = 2,
    HitShared = 3,
    Stale = 4
};

// ============================================================
// Execution metrics
// ============================================================

// These metrics describe a single operation invocation.
//
// Durations are monotonic measurements.
//
// All values default to zero because an invocation may
// terminate before entering an execution backend.

struct ExecutionMetrics final {
    std::chrono::nanoseconds queue_time{0};

    std::chrono::nanoseconds execution_time{0};

    std::chrono::nanoseconds serialization_time{0};

    std::chrono::nanoseconds total_time{0};

    // Estimated native allocations attributable to the
    // operation, if supported by the runtime.
    std::uint64_t allocation_count = 0;

    // Estimated memory usage attributable to the operation.
    //
    // This is not automatically equivalent to V8 heap usage.
    std::uint64_t memory_bytes = 0;
};

// ============================================================
// Result metadata
// ============================================================

// Metadata is transport-independent and ordered.
//
// It may be used for safe application-defined metadata
// such as version identifiers or diagnostic correlation.
//
// It must not be treated as an HTTP header collection.

struct ResultMetadataEntry final {
    std::string key;
    std::string value;
};

using ResultMetadata =
    std::vector<ResultMetadataEntry>;

// ============================================================
// Result validation
// ============================================================

enum class ResultValidationCode : std::uint8_t {
    None = 0,
    InvalidOperationId,
    InvalidOutcome,
    InvalidSerialization,
    InvalidError,
    InvalidPayload,
    InvalidMetadata,
    InvalidBackend,
    InvalidCacheResult,
    InvalidMetrics,
    InvalidRequestId
};

struct ResultValidationError final {
    ResultValidationCode code =
        ResultValidationCode::None;

    std::string message;
};

// ============================================================
// Result envelope
// ============================================================

struct ResultEnvelope final {
    using Byte = std::uint8_t;
    using Payload = std::vector<Byte>;

    // --------------------------------------------------------
    // Identity
    // --------------------------------------------------------

    RequestId request_id = 0;

    OperationId operation_id;

    // --------------------------------------------------------
    // Outcome
    // --------------------------------------------------------

    ResultOutcome outcome = ResultOutcome::Success;

    OperationError error;

    // --------------------------------------------------------
    // Payload
    // --------------------------------------------------------

    SerializationFormat format =
        SerializationFormat::Auto;

    Payload payload;

    // --------------------------------------------------------
    // Execution provenance
    // --------------------------------------------------------

    ExecutionBackend backend =
        ExecutionBackend::Unknown;

    CacheResult cache_result =
        CacheResult::NotChecked;

    // --------------------------------------------------------
    // Metadata and tracing
    // --------------------------------------------------------

    ResultMetadata metadata;

    TraceId trace_id;
    SpanId span_id;

    // --------------------------------------------------------
    // Metrics
    // --------------------------------------------------------

    ExecutionMetrics metrics;

    // --------------------------------------------------------
    // Factories
    // --------------------------------------------------------

    [[nodiscard]]
    static ResultEnvelope Success(
        const RequestEnvelope& request,
        Payload body,
        SerializationFormat serialization,
        ExecutionBackend executed_by
    ) {
        ResultEnvelope result;

        result.CopyIdentity(request);
        result.outcome = ResultOutcome::Success;
        result.format = serialization;
        result.payload = std::move(body);
        result.backend = executed_by;

        return result;
    }

    [[nodiscard]]
    static ResultEnvelope Failure(
        const RequestEnvelope& request,
        OperationError failure,
        ExecutionBackend executed_by =
            ExecutionBackend::Unknown
    ) {
        ResultEnvelope result;

        result.CopyIdentity(request);
        result.outcome = ResultOutcome::Failure;
        result.error = std::move(failure);
        result.backend = executed_by;

        return result;
    }

    [[nodiscard]]
    static ResultEnvelope Cancelled(
        const RequestEnvelope& request
    ) {
        ResultEnvelope result;

        result.CopyIdentity(request);
        result.outcome = ResultOutcome::Cancelled;

        result.error.code =
            OperationErrorCode::Cancelled;

        result.error.message =
            "Operation was cancelled";

        result.error.identifier =
            "OPERATION_CANCELLED";

        return result;
    }

    [[nodiscard]]
    static ResultEnvelope DeadlineExceeded(
        const RequestEnvelope& request
    ) {
        ResultEnvelope result;

        result.CopyIdentity(request);
        result.outcome =
            ResultOutcome::DeadlineExceeded;

        result.error.code =
            OperationErrorCode::DeadlineExceeded;

        result.error.message =
            "Operation deadline exceeded";

        result.error.identifier =
            "DEADLINE_EXCEEDED";

        return result;
    }

    // --------------------------------------------------------
    // State helpers
    // --------------------------------------------------------

    [[nodiscard]]
    bool IsSuccess() const noexcept {
        return outcome == ResultOutcome::Success;
    }

    [[nodiscard]]
    bool IsFailure() const noexcept {
        return outcome != ResultOutcome::Success;
    }

    [[nodiscard]]
    bool IsCancelled() const noexcept {
        return outcome == ResultOutcome::Cancelled;
    }

    [[nodiscard]]
    bool IsDeadlineExceeded() const noexcept {
        return outcome ==
               ResultOutcome::DeadlineExceeded;
    }

    [[nodiscard]]
    bool IsCacheHit() const noexcept {
        return cache_result == CacheResult::HitLocal ||
               cache_result == CacheResult::HitShared ||
               cache_result == CacheResult::Stale;
    }

    // --------------------------------------------------------
    // Payload helpers
    // --------------------------------------------------------

    [[nodiscard]]
    std::span<const Byte> PayloadView() const noexcept {
        return {
            payload.data(),
            payload.size()
        };
    }

    [[nodiscard]]
    std::size_t PayloadSize() const noexcept {
        return payload.size();
    }

    [[nodiscard]]
    bool HasPayload() const noexcept {
        return !payload.empty();
    }

    void SetPayload(Payload bytes) {
        payload = std::move(bytes);
    }

    // --------------------------------------------------------
    // Metadata helpers
    // --------------------------------------------------------

    void AddMetadata(
        std::string key,
        std::string value
    ) {
        metadata.push_back({
            std::move(key),
            std::move(value)
        });
    }

    [[nodiscard]]
    std::optional<std::string_view> FindMetadata(
        std::string_view key
    ) const noexcept {
        for (const auto& entry : metadata) {
            if (entry.key == key) {
                return entry.value;
            }
        }

        return std::nullopt;
    }

    // --------------------------------------------------------
    // Validation
    // --------------------------------------------------------

    [[nodiscard]]
    std::optional<ResultValidationError>
    Validate(
        const OperationDescriptor* descriptor = nullptr
    ) const {
        if (!OperationDescriptor::IsValidId(
                operation_id)) {
            return Error(
                ResultValidationCode::InvalidOperationId,
                "Invalid result operation identifier"
            );
        }

        if (descriptor != nullptr &&
            operation_id != descriptor->id) {
            return Error(
                ResultValidationCode::InvalidOperationId,
                "Result operation does not match descriptor"
            );
        }

        if (!ValidOutcome(outcome)) {
            return Error(
                ResultValidationCode::InvalidOutcome,
                "Invalid operation result outcome"
            );
        }

        if (!ValidSerialization(format)) {
            return Error(
                ResultValidationCode::InvalidSerialization,
                "Invalid result serialization format"
            );
        }

        if (!ValidBackend(backend)) {
            return Error(
                ResultValidationCode::InvalidBackend,
                "Invalid execution backend"
            );
        }

        if (!ValidCacheResult(cache_result)) {
            return Error(
                ResultValidationCode::InvalidCacheResult,
                "Invalid cache result"
            );
        }

        if (outcome == ResultOutcome::Success) {
            if (error.HasError()) {
                return Error(
                    ResultValidationCode::InvalidError,
                    "Successful result cannot contain "
                    "an operation error"
                );
            }

            if (!payload.empty() &&
                format == SerializationFormat::Auto) {
                return Error(
                    ResultValidationCode::
                        InvalidSerialization,
                    "Non-empty successful result requires "
                    "a concrete serialization format"
                );
            }
        } else {
            if (!error.HasError()) {
                return Error(
                    ResultValidationCode::InvalidError,
                    "Failed result requires an error code"
                );
            }

            if (!payload.empty()) {
                return Error(
                    ResultValidationCode::InvalidPayload,
                    "Failed result cannot contain "
                    "a success payload"
                );
            }

            if (outcome == ResultOutcome::Cancelled &&
                error.code !=
                    OperationErrorCode::Cancelled) {
                return Error(
                    ResultValidationCode::InvalidError,
                    "Cancelled result requires "
                    "the Cancelled error code"
                );
            }

            if (outcome ==
                    ResultOutcome::DeadlineExceeded &&
                error.code !=
                    OperationErrorCode::DeadlineExceeded) {
                return Error(
                    ResultValidationCode::InvalidError,
                    "Deadline-exceeded result requires "
                    "the DeadlineExceeded error code"
                );
            }
        }

        if (descriptor != nullptr &&
            outcome == ResultOutcome::Success) {
            if (payload.size() >
                descriptor->limits.max_output_bytes) {
                return Error(
                    ResultValidationCode::InvalidPayload,
                    "Result exceeds operation output limit"
                );
            }

            if (descriptor->output_format !=
                    SerializationFormat::Auto &&
                format != descriptor->output_format) {
                return Error(
                    ResultValidationCode::
                        InvalidSerialization,
                    "Result serialization does not match "
                    "operation descriptor"
                );
            }
        }

        constexpr std::size_t kMaxMetadataEntries = 128;
        constexpr std::size_t kMaxMetadataKeyBytes = 128;
        constexpr std::size_t kMaxMetadataValueBytes =
            8U * 1024U;

        if (metadata.size() > kMaxMetadataEntries) {
            return Error(
                ResultValidationCode::InvalidMetadata,
                "Too many result metadata entries"
            );
        }

        for (const auto& entry : metadata) {
            if (entry.key.empty() ||
                entry.key.size() > kMaxMetadataKeyBytes ||
                entry.value.size() >
                    kMaxMetadataValueBytes) {
                return Error(
                    ResultValidationCode::InvalidMetadata,
                    "Invalid result metadata entry"
                );
            }

            for (unsigned char ch : entry.key) {
                const bool valid =
                    (ch >= 'a' && ch <= 'z') ||
                    (ch >= '0' && ch <= '9') ||
                    ch == '-' ||
                    ch == '_' ||
                    ch == '.';

                if (!valid) {
                    return Error(
                        ResultValidationCode::InvalidMetadata,
                        "Invalid result metadata key"
                    );
                }
            }
        }

        if (!ValidTraceContext(trace_id, span_id)) {
            return Error(
                ResultValidationCode::InvalidMetadata,
                "Invalid result trace context"
            );
        }

        if (metrics.queue_time.count() < 0 ||
            metrics.execution_time.count() < 0 ||
            metrics.serialization_time.count() < 0 ||
            metrics.total_time.count() < 0) {
            return Error(
                ResultValidationCode::InvalidMetrics,
                "Execution metrics cannot be negative"
            );
        }

        return std::nullopt;
    }

    [[nodiscard]]
    bool IsValid(
        const OperationDescriptor* descriptor = nullptr
    ) const {
        return !Validate(descriptor).has_value();
    }

private:
    void CopyIdentity(
        const RequestEnvelope& request
    ) {
        request_id = request.request_id;
        operation_id = request.operation_id;
        trace_id = request.trace_id;
        span_id = request.span_id;
    }

    [[nodiscard]]
    static std::optional<ResultValidationError>
    Error(
        ResultValidationCode code,
        std::string message
    ) {
        return ResultValidationError{
            code,
            std::move(message)
        };
    }

    [[nodiscard]]
    static bool ValidOutcome(
        ResultOutcome value
    ) noexcept {
        switch (value) {
            case ResultOutcome::Success:
            case ResultOutcome::Failure:
            case ResultOutcome::Cancelled:
            case ResultOutcome::DeadlineExceeded:
                return true;
        }

        return false;
    }

    [[nodiscard]]
    static bool ValidSerialization(
        SerializationFormat value
    ) noexcept {
        switch (value) {
            case SerializationFormat::Auto:
            case SerializationFormat::Protobuf:
            case SerializationFormat::Json:
            case SerializationFormat::Binary:
                return true;
        }

        return false;
    }

    [[nodiscard]]
    static bool ValidBackend(
        ExecutionBackend value
    ) noexcept {
        switch (value) {
            case ExecutionBackend::Unknown:
            case ExecutionBackend::Native:
            case ExecutionBackend::JavaScript:
            case ExecutionBackend::Cache:
                return true;
        }

        return false;
    }

    [[nodiscard]]
    static bool ValidCacheResult(
        CacheResult value
    ) noexcept {
        switch (value) {
            case CacheResult::NotChecked:
            case CacheResult::Miss:
            case CacheResult::HitLocal:
            case CacheResult::HitShared:
            case CacheResult::Stale:
                return true;
        }

        return false;
    }

    [[nodiscard]]
    static bool ValidTraceContext(
        std::string_view trace,
        std::string_view span
    ) noexcept {
        if (trace.empty() && span.empty()) {
            return true;
        }

        if (trace.size() != 32 ||
            span.size() != 16) {
            return false;
        }

        const auto is_hex = [](
            std::string_view value
        ) noexcept {
            for (unsigned char ch : value) {
                const bool valid =
                    (ch >= '0' && ch <= '9') ||
                    (ch >= 'a' && ch <= 'f');

                if (!valid) {
                    return false;
                }
            }

            return true;
        };

        return is_hex(trace) && is_hex(span);
    }
};

} // namespace lamp::bridge
