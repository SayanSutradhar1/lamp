
#pragma once

// ============================================================
// Lamp Framework
// Phase 4 - Native Execution Bridge
//
// File: runtime/bridge/request_envelope.hpp
//
// Purpose:
//   Transport-independent request representation.
//
// Dependencies:
//   - C++20 standard library
//   - operation_descriptor.hpp
//
// Ownership:
//   - Payload is owned by the envelope.
//   - Metadata is owned by the envelope.
//   - Cancellation state is shared across copies.
//   - No references to transport-owned buffers.
//
// Threading:
//   - Separate envelope copies may be used concurrently.
//   - Cancellation is thread-safe.
//   - Concurrent mutation of the same envelope is not safe.
// ============================================================

#include "operation_descriptor.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lamp::bridge {

// ============================================================
// Request identifiers
// ============================================================

using RequestId = std::uint64_t;

using TraceId = std::string;
using SpanId = std::string;

// ============================================================
// Transport classification
// ============================================================

// Identifies the source transport without exposing its
// implementation details to the execution pipeline.

enum class TransportKind : std::uint8_t {
    Unknown = 0,
    Http = 1,
    BinaryRpc = 2,
    Internal = 3
};

// ============================================================
// Request metadata
// ============================================================

// Metadata is an ordered sequence rather than a map.
//
// This preserves duplicate entries and avoids imposing
// transport-specific case-folding semantics.
//
// Reserved system metadata should be populated by trusted
// transport adapters, never copied blindly from client input.

struct RequestMetadataEntry final {
    std::string key;
    std::string value;
};

using RequestMetadata =
    std::vector<RequestMetadataEntry>;

// ============================================================
// Cancellation state
// ============================================================

// Cooperative cancellation shared by the dispatcher,
// native workers, and V8 execution controller.
//
// Cancellation does not forcibly terminate a C++ thread.
// V8 termination must be coordinated separately by the
// isolate's owning execution controller.

class RequestCancellationState final {
public:
    RequestCancellationState() = default;

    RequestCancellationState(
        const RequestCancellationState&
    ) = delete;

    RequestCancellationState& operator=(
        const RequestCancellationState&
    ) = delete;

    void Cancel() noexcept {
        cancelled_.store(
            true,
            std::memory_order_release
        );
    }

    [[nodiscard]]
    bool IsCancelled() const noexcept {
        return cancelled_.load(
            std::memory_order_acquire
        );
    }

private:
    std::atomic<bool> cancelled_{false};
};

// ============================================================
// Cancellation token
// ============================================================

// A token is a shared view of cancellation state.
//
// Tokens may be copied across threads.
//
// The default-constructed token has no cancellation source
// and is therefore not cancelled.

class RequestCancellationToken final {
public:
    RequestCancellationToken() = default;

    explicit RequestCancellationToken(
        std::shared_ptr<RequestCancellationState> state
    ) noexcept
        : state_(std::move(state)) {}

    // A combined token observes cancellation from either source.
    // No source is cancelled merely by combining tokens.
    [[nodiscard]]
    static RequestCancellationToken Combine(
        RequestCancellationToken first,
        RequestCancellationToken second
    );

    [[nodiscard]]
    bool IsCancelled() const noexcept;

    [[nodiscard]]
    bool CanBeCancelled() const noexcept {
        return static_cast<bool>(state_) ||
               static_cast<bool>(composite_);
    }

private:
    struct Composite;

    std::shared_ptr<RequestCancellationState> state_;
    std::shared_ptr<const Composite> composite_;
};

// The composite is immutable after construction. Its child
// tokens retain shared ownership of their cancellation states.
struct RequestCancellationToken::Composite final {
    RequestCancellationToken first;
    RequestCancellationToken second;

    Composite(
        RequestCancellationToken first_token,
        RequestCancellationToken second_token
    ) noexcept
        : first(std::move(first_token)),
          second(std::move(second_token)) {}
};

inline RequestCancellationToken
RequestCancellationToken::Combine(
    RequestCancellationToken first,
    RequestCancellationToken second
) {
    if (!first.CanBeCancelled()) {
        return second;
    }

    if (!second.CanBeCancelled()) {
        return first;
    }

    RequestCancellationToken combined;
    combined.composite_ = std::make_shared<Composite>(
        std::move(first), std::move(second)
    );
    return combined;
}

inline bool RequestCancellationToken::IsCancelled()
    const noexcept {
    return (state_ && state_->IsCancelled()) ||
           (composite_ &&
            (composite_->first.IsCancelled() ||
             composite_->second.IsCancelled()));
}

// ============================================================
// Cancellation source
// ============================================================

// The source owns the authority to request cancellation.
//
// Copying a source shares that authority. The dispatcher
// may keep one copy while a transport adapter keeps another.

class RequestCancellationSource final {
public:
    RequestCancellationSource()
        : state_(
            std::make_shared<RequestCancellationState>()
          ) {}

    explicit RequestCancellationSource(
        std::shared_ptr<RequestCancellationState> state
    ) noexcept
        : state_(std::move(state)) {}

    [[nodiscard]]
    RequestCancellationToken Token() const noexcept {
        return RequestCancellationToken(state_);
    }

    void Cancel() const noexcept {
        if (state_) {
            state_->Cancel();
        }
    }

    [[nodiscard]]
    bool IsCancelled() const noexcept {
        return state_ && state_->IsCancelled();
    }

private:
    std::shared_ptr<RequestCancellationState> state_;
};

// ============================================================
// Request deadline
// ============================================================

// Uses steady_clock to avoid changes caused by wall-clock
// adjustments.
//
// A missing deadline means the request has no envelope-level
// deadline. Runtime-wide policies may still apply.

struct RequestDeadline final {
    using Clock = std::chrono::steady_clock;

    std::optional<Clock::time_point> expires_at;

    [[nodiscard]]
    bool HasDeadline() const noexcept {
        return expires_at.has_value();
    }

    [[nodiscard]]
    bool IsExpired() const noexcept {
        return expires_at &&
               Clock::now() >= *expires_at;
    }

    [[nodiscard]]
    std::optional<Clock::duration>
    Remaining() const noexcept {
        if (!expires_at) {
            return std::nullopt;
        }

        const auto now = Clock::now();

        if (now >= *expires_at) {
            return Clock::duration::zero();
        }

        return *expires_at - now;
    }

    [[nodiscard]]
    static RequestDeadline After(
        Clock::duration duration
    ) noexcept {
        RequestDeadline deadline;

        if (duration <= Clock::duration::zero()) {
            deadline.expires_at = Clock::now();
            return deadline;
        }

        const auto now = Clock::now();
        const auto remaining =
            Clock::time_point::max() - now;

        if (duration >= remaining) {
            deadline.expires_at =
                Clock::time_point::max();
        } else {
            deadline.expires_at = now + duration;
        }

        return deadline;
    }
};

// ============================================================
// Request validation
// ============================================================

enum class RequestValidationCode : std::uint8_t {
    None = 0,
    InvalidOperationId,
    InvalidTransport,
    InvalidSerialization,
    InvalidPayloadSize,
    InvalidMetadata,
    InvalidTraceContext,
    ExpiredDeadline,
    Cancelled
};

struct RequestValidationError final {
    RequestValidationCode code =
        RequestValidationCode::None;

    std::string message;
};

// ============================================================
// Request envelope
// ============================================================

struct RequestEnvelope final {
    using Byte = std::uint8_t;
    using Payload = std::vector<Byte>;

    // --------------------------------------------------------
    // Identity
    // --------------------------------------------------------

    RequestId request_id = 0;

    OperationId operation_id;

    // --------------------------------------------------------
    // Transport and serialization
    // --------------------------------------------------------

    TransportKind transport = TransportKind::Unknown;

    SerializationFormat format =
        SerializationFormat::Auto;

    // --------------------------------------------------------
    // Payload
    // --------------------------------------------------------

    // Binary-safe owned payload.
    //
    // It may contain Protobuf, JSON, or raw bytes depending
    // on the declared serialization format.
    //
    // A transport adapter should move its decoded payload
    // into this vector whenever possible.
    Payload payload;

    // --------------------------------------------------------
    // Metadata
    // --------------------------------------------------------

    RequestMetadata metadata;

    // --------------------------------------------------------
    // Tracing
    // --------------------------------------------------------

    TraceId trace_id;
    SpanId span_id;

    // --------------------------------------------------------
    // Scheduling and lifecycle
    // --------------------------------------------------------

    RequestDeadline deadline;

    RequestCancellationToken cancellation;

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

    void SetPayload(
        std::span<const Byte> bytes
    ) {
        payload.assign(
            bytes.begin(),
            bytes.end()
        );
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
    // Lifecycle helpers
    // --------------------------------------------------------

    [[nodiscard]]
    bool IsCancelled() const noexcept {
        return cancellation.IsCancelled();
    }

    [[nodiscard]]
    bool IsExpired() const noexcept {
        return deadline.IsExpired();
    }

    [[nodiscard]]
    bool IsActive() const noexcept {
        return !IsCancelled() && !IsExpired();
    }

    // --------------------------------------------------------
    // Validation
    // --------------------------------------------------------

    [[nodiscard]]
    std::optional<RequestValidationError>
    Validate(
        const OperationDescriptor* descriptor = nullptr
    ) const {
        if (!OperationDescriptor::IsValidId(
                operation_id)) {
            return Error(
                RequestValidationCode::InvalidOperationId,
                "Invalid request operation identifier"
            );
        }

        if (!ValidTransport(transport)) {
            return Error(
                RequestValidationCode::InvalidTransport,
                "Invalid request transport"
            );
        }

        if (!ValidSerialization(format)) {
            return Error(
                RequestValidationCode::InvalidSerialization,
                "Invalid request serialization format"
            );
        }

        // The dispatcher must resolve Auto to a concrete
        // wire format before decoding or execution.
        if (format == SerializationFormat::Auto &&
            !payload.empty()) {
            return Error(
                RequestValidationCode::InvalidSerialization,
                "Non-empty request payload requires "
                "a concrete serialization format"
            );
        }

        if (descriptor != nullptr) {
            if (operation_id != descriptor->id) {
                return Error(
                    RequestValidationCode::
                        InvalidOperationId,
                    "Request operation does not match "
                    "the operation descriptor"
                );
            }

            if (payload.size() >
                descriptor->limits.max_input_bytes) {
                return Error(
                    RequestValidationCode::
                        InvalidPayloadSize,
                    "Request exceeds operation input limit"
                );
            }

            if (descriptor->input_format !=
                    SerializationFormat::Auto &&
                format != descriptor->input_format) {
                return Error(
                    RequestValidationCode::
                        InvalidSerialization,
                    "Request serialization does not match "
                    "the operation descriptor"
                );
            }
        }

        constexpr std::size_t kMaxMetadataEntries = 128;
        constexpr std::size_t kMaxMetadataKeyBytes = 128;
        constexpr std::size_t kMaxMetadataValueBytes =
            8U * 1024U;

        if (metadata.size() > kMaxMetadataEntries) {
            return Error(
                RequestValidationCode::InvalidMetadata,
                "Too many request metadata entries"
            );
        }

        for (const auto& entry : metadata) {
            if (entry.key.empty() ||
                entry.key.size() > kMaxMetadataKeyBytes ||
                entry.value.size() >
                    kMaxMetadataValueBytes) {
                return Error(
                    RequestValidationCode::InvalidMetadata,
                    "Invalid request metadata entry"
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
                        RequestValidationCode::
                            InvalidMetadata,
                        "Metadata keys must contain only "
                        "lowercase ASCII letters, digits, "
                        "hyphens, underscores or dots"
                    );
                }
            }
        }

        if (!ValidTraceContext(
                trace_id, span_id)) {
            return Error(
                RequestValidationCode::
                    InvalidTraceContext,
                "Invalid request trace context"
            );
        }

        if (IsCancelled()) {
            return Error(
                RequestValidationCode::Cancelled,
                "Request has been cancelled"
            );
        }

        if (IsExpired()) {
            return Error(
                RequestValidationCode::ExpiredDeadline,
                "Request deadline has expired"
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
    [[nodiscard]]
    static std::optional<RequestValidationError>
    Error(
        RequestValidationCode code,
        std::string message
    ) {
        return RequestValidationError{
            code,
            std::move(message)
        };
    }

    [[nodiscard]]
    static bool ValidTransport(
        TransportKind kind
    ) noexcept {
        switch (kind) {
            case TransportKind::Http:
            case TransportKind::BinaryRpc:
            case TransportKind::Internal:
                return true;

            case TransportKind::Unknown:
                return false;
        }

        return false;
    }

    [[nodiscard]]
    static bool ValidSerialization(
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
    static bool ValidTraceContext(
        std::string_view trace,
        std::string_view span
    ) noexcept {
        // Trace IDs are expected to be 32 hexadecimal
        // characters and span IDs 16 hexadecimal characters.
        //
        // Both may be omitted for requests without tracing.
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
