
#pragma once

#include <cstdint>
#include <exception>
#include <memory>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>

namespace lamp::core {

// ============================================================
// Error Categories
// ============================================================

enum class ErrorCategory : std::uint8_t {
    General,
    Configuration,
    Transport,
    Routing,
    Validation,
    Authentication,
    Authorization,
    Serialization,
    Cache,
    Execution,
    V8,
    NativeTask,
    Streaming,
    Storage,
    Resource,
    Internal
};

// ============================================================
// Error Codes
// ============================================================

enum class ErrorCode : std::uint16_t {
    Ok = 0,

    Unknown = 1000,
    InvalidArgument = 1001,
    InvalidConfiguration = 1002,
    NotFound = 1003,
    AlreadyExists = 1004,
    UnsupportedOperation = 1005,
    NotImplemented = 1006,

    TransportFailure = 2000,
    ConnectionClosed = 2001,
    ProtocolError = 2002,
    RequestTooLarge = 2003,

    RouteNotFound = 3000,
    ValidationFailed = 3001,
    Unauthenticated = 3002,
    PermissionDenied = 3003,

    SerializationFailed = 4000,
    DeserializationFailed = 4001,
    SchemaMismatch = 4002,

    CacheFailure = 5000,
    CacheUnavailable = 5001,

    ExecutionFailed = 6000,
    ExecutionTimeout = 6001,
    ExecutionCancelled = 6002,
    V8InitializationFailed = 6003,
    V8ScriptError = 6004,
    V8OutOfMemory = 6005,
    NativeTaskFailed = 6006,

    StreamFailure = 7000,
    StreamCancelled = 7001,
    BackpressureExceeded = 7002,

    StorageFailure = 8000,
    StorageUnavailable = 8001,

    ResourceExhausted = 9000,
    QueueFull = 9001,
    RateLimitExceeded = 9002,

    InternalError = 10000
};

// ============================================================
// Error Metadata
// ============================================================

struct ErrorLocation {
    std::string file;
    std::string function;
    std::uint_least32_t line = 0;

    [[nodiscard]]
    static ErrorLocation current(
        const std::source_location& location =
            std::source_location::current()
    ) {
        return {
            location.file_name(),
            location.function_name(),
            location.line()
        };
    }
};

// ============================================================
// Error Classification
// ============================================================

[[nodiscard]]
constexpr ErrorCategory categoryOf(
    ErrorCode code
) noexcept {
    const auto value =
        static_cast<std::uint16_t>(code);

    if (code == ErrorCode::Ok) {
        return ErrorCategory::General;
    }

    if (code == ErrorCode::InvalidConfiguration) {
        return ErrorCategory::Configuration;
    }

    if (code == ErrorCode::InvalidArgument ||
        code == ErrorCode::ValidationFailed) {
        return ErrorCategory::Validation;
    }

    if (code == ErrorCode::Unauthenticated) {
        return ErrorCategory::Authentication;
    }

    if (code == ErrorCode::PermissionDenied) {
        return ErrorCategory::Authorization;
    }

    if (value >= 2000 && value < 3000) {
        return ErrorCategory::Transport;
    }

    if (code == ErrorCode::RouteNotFound) {
        return ErrorCategory::Routing;
    }

    if (value >= 4000 && value < 5000) {
        return ErrorCategory::Serialization;
    }

    if (value >= 5000 && value < 6000) {
        return ErrorCategory::Cache;
    }

    if (code == ErrorCode::V8InitializationFailed ||
        code == ErrorCode::V8ScriptError ||
        code == ErrorCode::V8OutOfMemory) {
        return ErrorCategory::V8;
    }

    if (code == ErrorCode::NativeTaskFailed) {
        return ErrorCategory::NativeTask;
    }

    if (value >= 6000 && value < 7000) {
        return ErrorCategory::Execution;
    }

    if (value >= 7000 && value < 8000) {
        return ErrorCategory::Streaming;
    }

    if (value >= 8000 && value < 9000) {
        return ErrorCategory::Storage;
    }

    if (value >= 9000 && value < 10000) {
        return ErrorCategory::Resource;
    }

    if (code == ErrorCode::InternalError) {
        return ErrorCategory::Internal;
    }

    return ErrorCategory::General;
}

// ============================================================
// Error Code Names
// ============================================================

[[nodiscard]]
constexpr std::string_view errorCodeName(
    ErrorCode code
) noexcept {
    switch (code) {
        case ErrorCode::Ok:
            return "OK";

        case ErrorCode::Unknown:
            return "UNKNOWN";
        case ErrorCode::InvalidArgument:
            return "INVALID_ARGUMENT";
        case ErrorCode::InvalidConfiguration:
            return "INVALID_CONFIGURATION";
        case ErrorCode::NotFound:
            return "NOT_FOUND";
        case ErrorCode::AlreadyExists:
            return "ALREADY_EXISTS";
        case ErrorCode::UnsupportedOperation:
            return "UNSUPPORTED_OPERATION";
        case ErrorCode::NotImplemented:
            return "NOT_IMPLEMENTED";

        case ErrorCode::TransportFailure:
            return "TRANSPORT_FAILURE";
        case ErrorCode::ConnectionClosed:
            return "CONNECTION_CLOSED";
        case ErrorCode::ProtocolError:
            return "PROTOCOL_ERROR";
        case ErrorCode::RequestTooLarge:
            return "REQUEST_TOO_LARGE";

        case ErrorCode::RouteNotFound:
            return "ROUTE_NOT_FOUND";
        case ErrorCode::ValidationFailed:
            return "VALIDATION_FAILED";
        case ErrorCode::Unauthenticated:
            return "UNAUTHENTICATED";
        case ErrorCode::PermissionDenied:
            return "PERMISSION_DENIED";

        case ErrorCode::SerializationFailed:
            return "SERIALIZATION_FAILED";
        case ErrorCode::DeserializationFailed:
            return "DESERIALIZATION_FAILED";
        case ErrorCode::SchemaMismatch:
            return "SCHEMA_MISMATCH";

        case ErrorCode::CacheFailure:
            return "CACHE_FAILURE";
        case ErrorCode::CacheUnavailable:
            return "CACHE_UNAVAILABLE";

        case ErrorCode::ExecutionFailed:
            return "EXECUTION_FAILED";
        case ErrorCode::ExecutionTimeout:
            return "EXECUTION_TIMEOUT";
        case ErrorCode::ExecutionCancelled:
            return "EXECUTION_CANCELLED";
        case ErrorCode::V8InitializationFailed:
            return "V8_INITIALIZATION_FAILED";
        case ErrorCode::V8ScriptError:
            return "V8_SCRIPT_ERROR";
        case ErrorCode::V8OutOfMemory:
            return "V8_OUT_OF_MEMORY";
        case ErrorCode::NativeTaskFailed:
            return "NATIVE_TASK_FAILED";

        case ErrorCode::StreamFailure:
            return "STREAM_FAILURE";
        case ErrorCode::StreamCancelled:
            return "STREAM_CANCELLED";
        case ErrorCode::BackpressureExceeded:
            return "BACKPRESSURE_EXCEEDED";

        case ErrorCode::StorageFailure:
            return "STORAGE_FAILURE";
        case ErrorCode::StorageUnavailable:
            return "STORAGE_UNAVAILABLE";

        case ErrorCode::ResourceExhausted:
            return "RESOURCE_EXHAUSTED";
        case ErrorCode::QueueFull:
            return "QUEUE_FULL";
        case ErrorCode::RateLimitExceeded:
            return "RATE_LIMIT_EXCEEDED";

        case ErrorCode::InternalError:
            return "INTERNAL_ERROR";
    }

    return "UNKNOWN_ERROR_CODE";
}

// ============================================================
// HTTP Status Mapping
// ============================================================

[[nodiscard]]
constexpr int httpStatusFor(
    ErrorCode code
) noexcept {
    switch (code) {
        case ErrorCode::Ok:
            return 200;

        case ErrorCode::InvalidArgument:
        case ErrorCode::ValidationFailed:
        case ErrorCode::DeserializationFailed:
        case ErrorCode::SchemaMismatch:
        case ErrorCode::ProtocolError:
            return 400;

        case ErrorCode::Unauthenticated:
            return 401;

        case ErrorCode::PermissionDenied:
            return 403;

        case ErrorCode::NotFound:
        case ErrorCode::RouteNotFound:
            return 404;

        case ErrorCode::AlreadyExists:
            return 409;

        case ErrorCode::RequestTooLarge:
            return 413;

        case ErrorCode::RateLimitExceeded:
            return 429;

        case ErrorCode::UnsupportedOperation:
        case ErrorCode::NotImplemented:
            return 501;

        case ErrorCode::ExecutionTimeout:
            return 504;

        case ErrorCode::ExecutionCancelled:
        case ErrorCode::StreamCancelled:
            return 499; // Non-standard; internal logging only.

        case ErrorCode::ResourceExhausted:
        case ErrorCode::QueueFull:
        case ErrorCode::CacheUnavailable:
        case ErrorCode::StorageUnavailable:
            return 503;

        default:
            return 500;
    }
}

// ============================================================
// Lamp Error
// ============================================================

class LampError final : public std::exception {
public:
    using Cause = std::shared_ptr<const LampError>;

    explicit LampError(
        ErrorCode code,
        std::string message,
        std::string publicMessage = {},
        Cause cause = nullptr,
        ErrorLocation location = ErrorLocation::current()
    )
        : code_(code),
          category_(categoryOf(code)),
          message_(std::move(message)),
          publicMessage_(std::move(publicMessage)),
          cause_(std::move(cause)),
          location_(std::move(location)) {

        if (publicMessage_.empty()) {
            publicMessage_ = defaultPublicMessage(code_);
        }
    }

    // --------------------------------------------------------
    // Accessors
    // --------------------------------------------------------

    [[nodiscard]]
    ErrorCode code() const noexcept {
        return code_;
    }

    [[nodiscard]]
    ErrorCategory category() const noexcept {
        return category_;
    }

    [[nodiscard]]
    const std::string& message() const noexcept {
        return message_;
    }

    [[nodiscard]]
    const std::string& publicMessage() const noexcept {
        return publicMessage_;
    }

    [[nodiscard]]
    const ErrorLocation& location() const noexcept {
        return location_;
    }

    [[nodiscard]]
    const Cause& cause() const noexcept {
        return cause_;
    }

    [[nodiscard]]
    int httpStatus() const noexcept {
        return httpStatusFor(code_);
    }

    [[nodiscard]]
    std::string_view name() const noexcept {
        return errorCodeName(code_);
    }

    const char* what() const noexcept override {
        return message_.c_str();
    }

    // --------------------------------------------------------
    // Error Chaining
    // --------------------------------------------------------

    [[nodiscard]]
    LampError withCause(Cause cause) const {
        return LampError(
            code_,
            message_,
            publicMessage_,
            std::move(cause),
            location_
        );
    }

    // --------------------------------------------------------
    // Safe Public Error Message
    // --------------------------------------------------------

    [[nodiscard]]
    static std::string defaultPublicMessage(
        ErrorCode code
    ) {
        switch (code) {
            case ErrorCode::InvalidArgument:
            case ErrorCode::ValidationFailed:
                return "Invalid request.";

            case ErrorCode::Unauthenticated:
                return "Authentication required.";

            case ErrorCode::PermissionDenied:
                return "Permission denied.";

            case ErrorCode::NotFound:
            case ErrorCode::RouteNotFound:
                return "Resource not found.";

            case ErrorCode::RequestTooLarge:
                return "Request body is too large.";

            case ErrorCode::RateLimitExceeded:
                return "Too many requests.";

            case ErrorCode::ExecutionTimeout:
                return "Request timed out.";

            case ErrorCode::ExecutionCancelled:
            case ErrorCode::StreamCancelled:
                return "Request cancelled.";

            case ErrorCode::UnsupportedOperation:
            case ErrorCode::NotImplemented:
                return "Operation not supported.";

            case ErrorCode::ResourceExhausted:
            case ErrorCode::QueueFull:
            case ErrorCode::CacheUnavailable:
            case ErrorCode::StorageUnavailable:
                return "Service temporarily unavailable.";

            default:
                return "Internal server error.";
        }
    }

private:
    ErrorCode code_;
    ErrorCategory category_;

    std::string message_;
    std::string publicMessage_;

    Cause cause_;
    ErrorLocation location_;
};

// ============================================================
// Error Factory
// ============================================================

[[nodiscard]]
inline LampError makeError(
    ErrorCode code,
    std::string message,
    std::string publicMessage = {},
    const std::source_location& location =
        std::source_location::current()
) {
    return LampError(
        code,
        std::move(message),
        std::move(publicMessage),
        nullptr,
        ErrorLocation::current(location)
    );
}

} // namespace lamp::core
