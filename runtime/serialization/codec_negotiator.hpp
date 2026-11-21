#pragma once

// ============================================================
// Lamp Framework
// Phase 7 - Serialization
//
// File: runtime/serialization/codec_negotiator.hpp
//
// Purpose:
//   Protobuf-first media-type negotiation (Content-Type and
//   Accept with quality weights) and unified schema-aware
//   request/response payload codec.
// ============================================================

#include "bridge/operation_descriptor.hpp"
#include "serialization/json_codec.hpp"
#include "serialization/protobuf_codec.hpp"
#include "serialization/schema_registry.hpp"
#include "serialization/serialization_value.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lamp::serialization {

enum class NegotiationStatus : std::uint8_t {
    Ok = 0,
    UnsupportedMediaType,
    NotAcceptable,
    InvalidHeader
};

struct NegotiationResult final {
    NegotiationStatus status = NegotiationStatus::Ok;
    bridge::SerializationFormat format =
        bridge::SerializationFormat::Protobuf;
    std::string message;

    [[nodiscard]]
    bool Ok() const noexcept {
        return status == NegotiationStatus::Ok;
    }
};

class CodecNegotiator final {
public:
    [[nodiscard]]
    static std::string_view MediaTypeForFormat(
        bridge::SerializationFormat format
    ) noexcept {
        switch (format) {
            case bridge::SerializationFormat::Protobuf:
                return "application/x-protobuf";
            case bridge::SerializationFormat::Json:
                return "application/json";
            case bridge::SerializationFormat::Binary:
                return "application/octet-stream";
            case bridge::SerializationFormat::Auto:
                return "application/x-protobuf";
        }
        return "application/octet-stream";
    }

    [[nodiscard]]
    static std::optional<bridge::SerializationFormat>
    FormatFromMediaType(std::string_view header) noexcept {
        const auto media = NormalizeMediaType(header);
        if (media.empty()) {
            return std::nullopt;
        }

        if (media == "application/x-protobuf" ||
            media == "application/protobuf" ||
            media == "application/vnd.google.protobuf") {
            return bridge::SerializationFormat::Protobuf;
        }

        if (media == "application/json" ||
            media == "application/problem+json") {
            return bridge::SerializationFormat::Json;
        }

        if (media == "application/octet-stream") {
            return bridge::SerializationFormat::Binary;
        }

        return std::nullopt;
    }

    [[nodiscard]]
    static NegotiationResult NegotiateInput(
        std::optional<std::string_view> contentType,
        bridge::SerializationFormat descriptorFormat,
        bool hasPayload = true
    ) {
        (void)hasPayload;

        if (!contentType.has_value() ||
            Trim(*contentType).empty()) {
            const auto resolved =
                descriptorFormat == bridge::SerializationFormat::Auto
                    ? bridge::SerializationFormat::Protobuf
                    : descriptorFormat;
            return {NegotiationStatus::Ok, resolved, {}};
        }

        const auto parsed = FormatFromMediaType(*contentType);
        if (!parsed) {
            return {
                NegotiationStatus::UnsupportedMediaType,
                bridge::SerializationFormat::Auto,
                "Unsupported Content-Type media type"
            };
        }

        if (descriptorFormat != bridge::SerializationFormat::Auto &&
            *parsed != descriptorFormat) {
            return {
                NegotiationStatus::UnsupportedMediaType,
                bridge::SerializationFormat::Auto,
                "Content-Type does not match operation input format"
            };
        }

        return {NegotiationStatus::Ok, *parsed, {}};
    }

    [[nodiscard]]
    static NegotiationResult NegotiateOutput(
        std::optional<std::string_view> acceptHeader,
        bridge::SerializationFormat descriptorFormat
    ) {
        if (!acceptHeader.has_value() ||
            Trim(*acceptHeader).empty()) {
            const auto resolved =
                descriptorFormat == bridge::SerializationFormat::Auto
                    ? bridge::SerializationFormat::Protobuf
                    : descriptorFormat;
            return {NegotiationStatus::Ok, resolved, {}};
        }

        struct Candidate final {
            bridge::SerializationFormat format;
            int rank;
            double quality;
            bool matched;
        };

        // Protobuf-first tie-break order: Protobuf (0) > Json (1) > Binary (2)
        Candidate candidates[3] = {
            {bridge::SerializationFormat::Protobuf, 0, -1.0, false},
            {bridge::SerializationFormat::Json, 1, -1.0, false},
            {bridge::SerializationFormat::Binary, 2, -1.0, false}
        };

        auto items = SplitComma(*acceptHeader);
        if (items.empty()) {
            return {
                NegotiationStatus::InvalidHeader,
                bridge::SerializationFormat::Auto,
                "Empty Accept header"
            };
        }

        for (const auto rawItem : items) {
            const auto item = Trim(rawItem);
            if (item.empty()) {
                continue;
            }

            std::size_t semi = item.find(';');
            const std::string media = NormalizeMediaType(
                semi == std::string_view::npos
                    ? item
                    : item.substr(0, semi)
            );
            if (media.empty() || media.find('/') == std::string::npos) {
                return {
                    NegotiationStatus::InvalidHeader,
                    bridge::SerializationFormat::Auto,
                    "Malformed Accept media range"
                };
            }

            double quality = 1.0;
            if (semi != std::string_view::npos) {
                if (!ParseQuality(item.substr(semi + 1), quality)) {
                    return {
                        NegotiationStatus::InvalidHeader,
                        bridge::SerializationFormat::Auto,
                        "Invalid Accept quality parameter"
                    };
                }
            }

            const bool isWildcardAll = (media == "*/*");
            const bool isWildcardApp = (media == "application/*");
            const auto exactFmt = FormatFromMediaType(media);

            for (auto& cand : candidates) {
                bool applies = false;
                int specificity = 0;

                if (exactFmt && *exactFmt == cand.format) {
                    applies = true;
                    specificity = 3;
                } else if (isWildcardApp && !cand.matched) {
                    applies = true;
                    specificity = 2;
                } else if (isWildcardAll && !cand.matched) {
                    applies = true;
                    specificity = 1;
                }

                if (applies) {
                    if (!cand.matched || specificity == 3) {
                        cand.quality = quality;
                        cand.matched = true;
                    }
                }
            }
        }

        if (descriptorFormat != bridge::SerializationFormat::Auto) {
            for (const auto& cand : candidates) {
                if (cand.format == descriptorFormat) {
                    if (cand.matched && cand.quality > 0.0) {
                        return {
                            NegotiationStatus::Ok,
                            descriptorFormat,
                            {}
                        };
                    }
                    return {
                        NegotiationStatus::NotAcceptable,
                        bridge::SerializationFormat::Auto,
                        "Accept header excludes operation output format"
                    };
                }
            }
        }

        const Candidate* best = nullptr;
        for (const auto& cand : candidates) {
            if (!cand.matched || cand.quality <= 0.0) {
                continue;
            }
            if (!best ||
                cand.quality > best->quality ||
                (cand.quality == best->quality &&
                 cand.rank < best->rank)) {
                best = &cand;
            }
        }

        if (!best) {
            return {
                NegotiationStatus::NotAcceptable,
                bridge::SerializationFormat::Auto,
                "No mutually acceptable output serialization format"
            };
        }

        return {NegotiationStatus::Ok, best->format, {}};
    }

private:
    [[nodiscard]]
    static std::string_view Trim(std::string_view sv) noexcept {
        while (!sv.empty() &&
               (sv.front() == ' ' || sv.front() == '\t')) {
            sv.remove_prefix(1);
        }
        while (!sv.empty() &&
               (sv.back() == ' ' || sv.back() == '\t')) {
            sv.remove_suffix(1);
        }
        return sv;
    }

    [[nodiscard]]
    static std::string NormalizeMediaType(
        std::string_view header
    ) {
        const auto semi = header.find(';');
        if (semi != std::string_view::npos) {
            header = header.substr(0, semi);
        }
        header = Trim(header);

        std::string lower;
        lower.reserve(header.size());
        for (const unsigned char ch : header) {
            lower.push_back(
                static_cast<char>(std::tolower(ch))
            );
        }
        return lower;
    }

    [[nodiscard]]
    static std::vector<std::string_view> SplitComma(
        std::string_view sv
    ) {
        std::vector<std::string_view> out;
        std::size_t start = 0;
        while (start <= sv.size()) {
            const auto comma = sv.find(',', start);
            if (comma == std::string_view::npos) {
                out.push_back(sv.substr(start));
                break;
            }
            out.push_back(sv.substr(start, comma - start));
            start = comma + 1;
        }
        return out;
    }

    [[nodiscard]]
    static bool ParseQuality(
        std::string_view params,
        double& quality
    ) noexcept {
        std::size_t start = 0;
        while (start <= params.size()) {
            const auto semi = params.find(';', start);
            const auto part = Trim(
                semi == std::string_view::npos
                    ? params.substr(start)
                    : params.substr(start, semi - start)
            );
            const auto eq = part.find('=');
            if (eq != std::string_view::npos) {
                const auto key = Trim(part.substr(0, eq));
                const auto val = Trim(part.substr(eq + 1));
                if (key.size() == 1 &&
                    (key[0] == 'q' || key[0] == 'Q')) {
                    double q = 0.0;
                    const auto [ptr, ec] = std::from_chars(
                        val.data(),
                        val.data() + val.size(),
                        q
                    );
                    if (ec != std::errc{} ||
                        ptr != val.data() + val.size() ||
                        q < 0.0 ||
                        q > 1.0) {
                        return false;
                    }
                    quality = q;
                }
            }
            if (semi == std::string_view::npos) {
                break;
            }
            start = semi + 1;
        }
        return true;
    }
};

class PayloadCodec final {
public:
    [[nodiscard]]
    static CodecResult<Value> DecodeRequest(
        const SchemaRegistry* registry,
        const bridge::OperationDescriptor& descriptor,
        bridge::SerializationFormat format,
        std::span<const std::uint8_t> payload
    ) {
        if (payload.size() > descriptor.limits.max_input_bytes) {
            return CodecResult<Value>::Failure(
                CodecErrorCode::PayloadTooLarge,
                "Request payload exceeds operation max_input_bytes"
            );
        }

        if (format == bridge::SerializationFormat::Auto) {
            format =
                descriptor.input_format ==
                        bridge::SerializationFormat::Auto
                    ? bridge::SerializationFormat::Protobuf
                    : descriptor.input_format;
        }

        if (descriptor.input_format !=
                bridge::SerializationFormat::Auto &&
            format != descriptor.input_format) {
            return CodecResult<Value>::Failure(
                CodecErrorCode::UnsupportedFormat,
                "Input format does not match operation descriptor"
            );
        }

        switch (format) {
            case bridge::SerializationFormat::Binary: {
                return CodecResult<Value>::Success(
                    Value(Value::Bytes(
                        payload.begin(),
                        payload.end()
                    ))
                );
            }

            case bridge::SerializationFormat::Protobuf: {
                if (descriptor.input_schema.Empty() || !registry) {
                    return CodecResult<Value>::Failure(
                        CodecErrorCode::MissingNestedSchema,
                        "Protobuf decoding requires registered input schema"
                    );
                }
                const auto schema =
                    registry->Find(descriptor.input_schema);
                if (!schema) {
                    return CodecResult<Value>::Failure(
                        CodecErrorCode::SchemaViolation,
                        "Input schema not found: " +
                            descriptor.input_schema.name
                    );
                }
                ProtobufCodecOptions opts;
                opts.max_message_bytes =
                    descriptor.limits.max_input_bytes;
                return ProtobufCodec::Decode(
                    *schema,
                    payload,
                    registry,
                    opts
                );
            }

            case bridge::SerializationFormat::Json: {
                JsonCodecOptions opts;
                opts.max_input_bytes =
                    descriptor.limits.max_input_bytes;

                if (!descriptor.input_schema.Empty() && registry) {
                    const auto schema =
                        registry->Find(descriptor.input_schema);
                    if (!schema) {
                        return CodecResult<Value>::Failure(
                            CodecErrorCode::SchemaViolation,
                            "Input schema not found: " +
                                descriptor.input_schema.name
                        );
                    }
                    if (payload.empty()) {
                        const Value emptyObj = Value::MakeObject();
                        if (const auto err =
                                registry->ValidateWithSchema(
                                    *schema,
                                    emptyObj
                                )) {
                            return CodecResult<Value>::Failure(
                                CodecErrorCode::SchemaViolation,
                                err->field_path + ": " + err->message
                            );
                        }
                        return CodecResult<Value>::Success(emptyObj);
                    }
                    return JsonCodec::DecodeWithSchema(
                        *schema,
                        payload,
                        registry,
                        opts
                    );
                }

                if (payload.empty()) {
                    return CodecResult<Value>::Success(
                        Value::MakeObject()
                    );
                }
                return JsonCodec::Decode(payload, opts);
            }

            case bridge::SerializationFormat::Auto:
                break;
        }

        return CodecResult<Value>::Failure(
            CodecErrorCode::UnsupportedFormat,
            "Unsupported input serialization format"
        );
    }

    [[nodiscard]]
    static CodecResult<std::vector<std::uint8_t>> EncodeResponse(
        const SchemaRegistry* registry,
        const bridge::OperationDescriptor& descriptor,
        bridge::SerializationFormat format,
        const Value& value
    ) {
        if (format == bridge::SerializationFormat::Auto) {
            format =
                descriptor.output_format ==
                        bridge::SerializationFormat::Auto
                    ? bridge::SerializationFormat::Protobuf
                    : descriptor.output_format;
        }

        if (descriptor.output_format !=
                bridge::SerializationFormat::Auto &&
            format != descriptor.output_format) {
            return CodecResult<std::vector<std::uint8_t>>::Failure(
                CodecErrorCode::UnsupportedFormat,
                "Output format does not match operation descriptor"
            );
        }

        switch (format) {
            case bridge::SerializationFormat::Binary: {
                std::vector<std::uint8_t> out;
                if (const auto* bytes = value.AsBytes()) {
                    out = *bytes;
                } else if (const auto str = value.AsString()) {
                    out.assign(str->begin(), str->end());
                } else {
                    return CodecResult<std::vector<std::uint8_t>>::Failure(
                        CodecErrorCode::InvalidInput,
                        "Binary output format requires Bytes or String value"
                    );
                }
                if (out.size() > descriptor.limits.max_output_bytes) {
                    return CodecResult<std::vector<std::uint8_t>>::Failure(
                        CodecErrorCode::PayloadTooLarge,
                        "Binary output exceeds operation max_output_bytes"
                    );
                }
                return CodecResult<std::vector<std::uint8_t>>::Success(
                    std::move(out)
                );
            }

            case bridge::SerializationFormat::Protobuf: {
                if (descriptor.output_schema.Empty() || !registry) {
                    return CodecResult<std::vector<std::uint8_t>>::Failure(
                        CodecErrorCode::MissingNestedSchema,
                        "Protobuf encoding requires registered output schema"
                    );
                }
                const auto schema =
                    registry->Find(descriptor.output_schema);
                if (!schema) {
                    return CodecResult<std::vector<std::uint8_t>>::Failure(
                        CodecErrorCode::SchemaViolation,
                        "Output schema not found: " +
                            descriptor.output_schema.name
                    );
                }
                ProtobufCodecOptions opts;
                opts.max_message_bytes =
                    descriptor.limits.max_output_bytes;
                return ProtobufCodec::Encode(
                    *schema,
                    value,
                    registry,
                    opts
                );
            }

            case bridge::SerializationFormat::Json: {
                JsonCodecOptions opts;
                opts.max_output_bytes =
                    descriptor.limits.max_output_bytes;

                if (!descriptor.output_schema.Empty() && registry) {
                    const auto schema =
                        registry->Find(descriptor.output_schema);
                    if (!schema) {
                        return CodecResult<std::vector<std::uint8_t>>::Failure(
                            CodecErrorCode::SchemaViolation,
                            "Output schema not found: " +
                                descriptor.output_schema.name
                        );
                    }
                    return JsonCodec::EncodeWithSchema(
                        *schema,
                        value,
                        registry,
                        opts
                    );
                }

                return JsonCodec::Encode(value, opts);
            }

            case bridge::SerializationFormat::Auto:
                break;
        }

        return CodecResult<std::vector<std::uint8_t>>::Failure(
            CodecErrorCode::UnsupportedFormat,
            "Unsupported output serialization format"
        );
    }
};

} // namespace lamp::serialization
