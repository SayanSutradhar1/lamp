#pragma once

// ============================================================
// Lamp Framework
// Phase 7 - Serialization
//
// File: runtime/serialization/operation_codec_adapter.hpp
//
// Purpose:
//   Bridge structured Value handlers and SchemaRegistry into
//   bridge::INativeOperationHandler, handling input decode,
//   schema validation, output format selection, and encoding.
// ============================================================

#include "bridge/operation_registry.hpp"
#include "bridge/request_envelope.hpp"
#include "bridge/result_envelope.hpp"
#include "serialization/codec_negotiator.hpp"
#include "serialization/schema_registry.hpp"
#include "serialization/serialization_value.hpp"

#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace lamp::serialization {

struct StructuredOperationOutcome final {
    std::optional<Value> value;
    std::optional<bridge::OperationError> error;

    [[nodiscard]]
    static StructuredOperationOutcome Success(Value val) {
        StructuredOperationOutcome out;
        out.value.emplace(std::move(val));
        return out;
    }

    [[nodiscard]]
    static StructuredOperationOutcome Failure(
        bridge::OperationError err
    ) {
        StructuredOperationOutcome out;
        out.error.emplace(std::move(err));
        return out;
    }
};

using StructuredOperationHandlerFn = std::function<
    StructuredOperationOutcome(
        const bridge::RequestEnvelope&,
        const Value&
    )
>;

class SchemaAwareNativeHandler final
    : public bridge::INativeOperationHandler {
public:
    SchemaAwareNativeHandler(
        std::shared_ptr<const SchemaRegistry> registry,
        bridge::OperationDescriptor descriptor,
        StructuredOperationHandlerFn handler
    )
        : registry_(std::move(registry)),
          descriptor_(std::move(descriptor)),
          handler_(std::move(handler)) {
        if (!handler_) {
            throw std::invalid_argument(
                "SchemaAwareNativeHandler requires a handler"
            );
        }
    }

    [[nodiscard]]
    bridge::ResultEnvelope Execute(
        const bridge::RequestEnvelope& request
    ) override {
        auto decoded = PayloadCodec::DecodeRequest(
            registry_.get(),
            descriptor_,
            request.format,
            request.payload
        );

        if (!decoded.Ok()) {
            return bridge::ResultEnvelope::Failure(
                request,
                bridge::OperationError{
                    bridge::OperationErrorCode::InvalidArgument,
                    decoded.error.message,
                    "INVALID_OPERATION_INPUT",
                    false
                },
                bridge::ExecutionBackend::Native
            );
        }

        StructuredOperationOutcome outcome;
        try {
            outcome = handler_(request, *decoded.value);
        } catch (const std::exception&) {
            return bridge::ResultEnvelope::Failure(
                request,
                bridge::OperationError{
                    bridge::OperationErrorCode::ExecutionFailed,
                    "Operation execution failed",
                    "OPERATION_EXECUTION_FAILED",
                    false
                },
                bridge::ExecutionBackend::Native
            );
        }

        if (outcome.error.has_value()) {
            return bridge::ResultEnvelope::Failure(
                request,
                std::move(*outcome.error),
                bridge::ExecutionBackend::Native
            );
        }

        if (!outcome.value.has_value()) {
            return bridge::ResultEnvelope::Failure(
                request,
                bridge::OperationError{
                    bridge::OperationErrorCode::Internal,
                    "Handler returned empty outcome",
                    "EMPTY_HANDLER_OUTCOME",
                    false
                },
                bridge::ExecutionBackend::Native
            );
        }

        const auto outputFormat = ResolveOutputFormat(request);
        if (!outputFormat.has_value()) {
            return bridge::ResultEnvelope::Failure(
                request,
                bridge::OperationError{
                    bridge::OperationErrorCode::InvalidArgument,
                    "Requested response format is not acceptable",
                    "NOT_ACCEPTABLE_FORMAT",
                    false
                },
                bridge::ExecutionBackend::Native
            );
        }

        auto encoded = PayloadCodec::EncodeResponse(
            registry_.get(),
            descriptor_,
            *outputFormat,
            *outcome.value
        );

        if (!encoded.Ok()) {
            return bridge::ResultEnvelope::Failure(
                request,
                bridge::OperationError{
                    bridge::OperationErrorCode::SerializationFailed,
                    encoded.error.message,
                    "OUTPUT_SERIALIZATION_FAILED",
                    false
                },
                bridge::ExecutionBackend::Native
            );
        }

        return bridge::ResultEnvelope::Success(
            request,
            std::move(*encoded.value),
            *outputFormat,
            bridge::ExecutionBackend::Native
        );
    }

private:
    [[nodiscard]]
    std::optional<bridge::SerializationFormat> ResolveOutputFormat(
        const bridge::RequestEnvelope& request
    ) const {
        if (const auto accept =
                request.FindMetadata("lamp.accept")) {
            const auto neg = CodecNegotiator::NegotiateOutput(
                *accept,
                descriptor_.output_format
            );
            if (!neg.Ok()) {
                return std::nullopt;
            }
            return neg.format;
        }

        if (descriptor_.output_format !=
            bridge::SerializationFormat::Auto) {
            return descriptor_.output_format;
        }

        if (request.format != bridge::SerializationFormat::Auto) {
            return request.format;
        }

        return bridge::SerializationFormat::Protobuf;
    }

    std::shared_ptr<const SchemaRegistry> registry_;
    bridge::OperationDescriptor descriptor_;
    StructuredOperationHandlerFn handler_;
};

} // namespace lamp::serialization
