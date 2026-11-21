#pragma once

// ============================================================
// Lamp Framework
// Phase 7 - Serialization
//
// File: runtime/serialization/json_codec.hpp
//
// Purpose:
//   Strict RFC 8259 UTF-8 JSON fallback codec over
//   lamp::serialization::Value with base64 binary-field
//   support and schema-aware decoding.
// ============================================================

#include "serialization/protobuf_codec.hpp"
#include "serialization/schema_registry.hpp"
#include "serialization/serialization_value.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lamp::serialization {

struct JsonCodecOptions final {
    std::size_t max_depth = 32;
    std::size_t max_input_bytes = 16U * 1024U * 1024U;
    std::size_t max_output_bytes = 16U * 1024U * 1024U;
    bool validate = true;
    bool apply_defaults = true;
};

class Base64 final {
public:
    [[nodiscard]]
    static std::string Encode(
        std::span<const std::uint8_t> bytes
    ) {
        static constexpr char kAlphabet[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
            "abcdefghijklmnopqrstuvwxyz0123456789+/";

        std::string out;
        out.reserve(((bytes.size() + 2) / 3) * 4);

        std::size_t i = 0;
        while (i + 3 <= bytes.size()) {
            const std::uint32_t triple =
                (static_cast<std::uint32_t>(bytes[i]) << 16U) |
                (static_cast<std::uint32_t>(bytes[i + 1]) << 8U) |
                static_cast<std::uint32_t>(bytes[i + 2]);
            out.push_back(kAlphabet[(triple >> 18U) & 0x3FU]);
            out.push_back(kAlphabet[(triple >> 12U) & 0x3FU]);
            out.push_back(kAlphabet[(triple >> 6U) & 0x3FU]);
            out.push_back(kAlphabet[triple & 0x3FU]);
            i += 3;
        }

        const std::size_t rem = bytes.size() - i;
        if (rem == 1) {
            const std::uint32_t triple =
                static_cast<std::uint32_t>(bytes[i]) << 16U;
            out.push_back(kAlphabet[(triple >> 18U) & 0x3FU]);
            out.push_back(kAlphabet[(triple >> 12U) & 0x3FU]);
            out.push_back('=');
            out.push_back('=');
        } else if (rem == 2) {
            const std::uint32_t triple =
                (static_cast<std::uint32_t>(bytes[i]) << 16U) |
                (static_cast<std::uint32_t>(bytes[i + 1]) << 8U);
            out.push_back(kAlphabet[(triple >> 18U) & 0x3FU]);
            out.push_back(kAlphabet[(triple >> 12U) & 0x3FU]);
            out.push_back(kAlphabet[(triple >> 6U) & 0x3FU]);
            out.push_back('=');
        }

        return out;
    }

    [[nodiscard]]
    static std::optional<std::vector<std::uint8_t>> Decode(
        std::string_view input
    ) {
        if (input.size() % 4 != 0) {
            return std::nullopt;
        }
        if (input.empty()) {
            return std::vector<std::uint8_t>{};
        }

        auto decodeChar = [](unsigned char ch) -> int {
            if (ch >= 'A' && ch <= 'Z') return ch - 'A';
            if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
            if (ch >= '0' && ch <= '9') return ch - '0' + 52;
            if (ch == '+') return 62;
            if (ch == '/') return 63;
            return -1;
        };

        std::size_t padding = 0;
        if (input.back() == '=') ++padding;
        if (input.size() >= 2 && input[input.size() - 2] == '=') {
            ++padding;
        }

        std::vector<std::uint8_t> out;
        out.reserve((input.size() / 4) * 3 - padding);

        for (std::size_t i = 0; i < input.size(); i += 4) {
            const bool isLast = (i + 4 == input.size());
            const int v0 = decodeChar(static_cast<unsigned char>(input[i]));
            const int v1 = decodeChar(static_cast<unsigned char>(input[i + 1]));
            if (v0 < 0 || v1 < 0) {
                return std::nullopt;
            }

            const char c2 = input[i + 2];
            const char c3 = input[i + 3];

            if (c2 == '=') {
                if (!isLast || c3 != '=') {
                    return std::nullopt;
                }
                if ((v1 & 0x0F) != 0) {
                    return std::nullopt;
                }
                out.push_back(
                    static_cast<std::uint8_t>((v0 << 2) | (v1 >> 4))
                );
                break;
            }

            const int v2 = decodeChar(static_cast<unsigned char>(c2));
            if (v2 < 0) {
                return std::nullopt;
            }

            if (c3 == '=') {
                if (!isLast) {
                    return std::nullopt;
                }
                if ((v2 & 0x03) != 0) {
                    return std::nullopt;
                }
                const std::uint32_t val =
                    (static_cast<std::uint32_t>(v0) << 12U) |
                    (static_cast<std::uint32_t>(v1) << 6U) |
                    static_cast<std::uint32_t>(v2);
                out.push_back(static_cast<std::uint8_t>((val >> 10U) & 0xFFU));
                out.push_back(static_cast<std::uint8_t>((val >> 2U) & 0xFFU));
                break;
            }

            const int v3 = decodeChar(static_cast<unsigned char>(c3));
            if (v3 < 0) {
                return std::nullopt;
            }

            const std::uint32_t triple =
                (static_cast<std::uint32_t>(v0) << 18U) |
                (static_cast<std::uint32_t>(v1) << 12U) |
                (static_cast<std::uint32_t>(v2) << 6U) |
                static_cast<std::uint32_t>(v3);
            out.push_back(static_cast<std::uint8_t>((triple >> 16U) & 0xFFU));
            out.push_back(static_cast<std::uint8_t>((triple >> 8U) & 0xFFU));
            out.push_back(static_cast<std::uint8_t>(triple & 0xFFU));
        }

        return out;
    }
};

class JsonCodec final {
public:
    [[nodiscard]]
    static CodecResult<std::string> EncodeString(
        const Value& value,
        JsonCodecOptions options = {}
    ) {
        std::string out;
        if (auto err = WriteValue(value, options, 0, out)) {
            return CodecResult<std::string>::Failure(
                err->code,
                std::move(err->message)
            );
        }
        if (out.size() > options.max_output_bytes) {
            return CodecResult<std::string>::Failure(
                CodecErrorCode::PayloadTooLarge,
                "Encoded JSON payload exceeds max_output_bytes"
            );
        }
        return CodecResult<std::string>::Success(std::move(out));
    }

    [[nodiscard]]
    static CodecResult<std::vector<std::uint8_t>> Encode(
        const Value& value,
        JsonCodecOptions options = {}
    ) {
        auto strRes = EncodeString(value, options);
        if (!strRes.Ok()) {
            return CodecResult<std::vector<std::uint8_t>>::Failure(
                strRes.error.code,
                std::move(strRes.error.message)
            );
        }
        std::vector<std::uint8_t> bytes(
            strRes.value->begin(),
            strRes.value->end()
        );
        return CodecResult<std::vector<std::uint8_t>>::Success(
            std::move(bytes)
        );
    }

    [[nodiscard]]
    static CodecResult<std::vector<std::uint8_t>> EncodeWithSchema(
        const MessageSchema& schema,
        const Value& value,
        const SchemaRegistry* registry = nullptr,
        JsonCodecOptions options = {}
    ) {
        if (options.validate) {
            if (registry != nullptr) {
                if (const auto err = registry->ValidateWithSchema(
                        schema,
                        value,
                        options.max_depth
                    )) {
                    return CodecResult<std::vector<std::uint8_t>>::Failure(
                        CodecErrorCode::SchemaViolation,
                        err->field_path + ": " + err->message
                    );
                }
            } else {
                SchemaRegistry emptyRegistry;
                if (const auto err = emptyRegistry.ValidateWithSchema(
                        schema,
                        value,
                        options.max_depth
                    )) {
                    return CodecResult<std::vector<std::uint8_t>>::Failure(
                        CodecErrorCode::SchemaViolation,
                        err->field_path + ": " + err->message
                    );
                }
            }
        }
        return Encode(value, options);
    }

    [[nodiscard]]
    static CodecResult<Value> DecodeString(
        std::string_view text,
        JsonCodecOptions options = {}
    ) {
        if (text.size() > options.max_input_bytes) {
            return CodecResult<Value>::Failure(
                CodecErrorCode::PayloadTooLarge,
                "JSON input exceeds max_input_bytes"
            );
        }

        Parser parser(text, options.max_depth);
        Value parsed;
        if (auto err = parser.ParseRoot(parsed)) {
            return CodecResult<Value>::Failure(
                err->code,
                std::move(err->message)
            );
        }
        return CodecResult<Value>::Success(std::move(parsed));
    }

    [[nodiscard]]
    static CodecResult<Value> Decode(
        std::span<const std::uint8_t> bytes,
        JsonCodecOptions options = {}
    ) {
        std::string_view text(
            reinterpret_cast<const char*>(bytes.data()),
            bytes.size()
        );
        return DecodeString(text, options);
    }

    [[nodiscard]]
    static CodecResult<Value> DecodeWithSchema(
        const MessageSchema& schema,
        std::span<const std::uint8_t> bytes,
        const SchemaRegistry* registry = nullptr,
        JsonCodecOptions options = {}
    ) {
        auto decoded = Decode(bytes, options);
        if (!decoded.Ok()) {
            return decoded;
        }

        if (auto err = CoerceAndApplyDefaults(
                schema,
                *decoded.value,
                registry,
                options,
                0
            )) {
            return CodecResult<Value>::Failure(
                err->code,
                std::move(err->message)
            );
        }

        if (options.validate) {
            if (registry != nullptr) {
                if (const auto err = registry->ValidateWithSchema(
                        schema,
                        *decoded.value,
                        options.max_depth
                    )) {
                    return CodecResult<Value>::Failure(
                        CodecErrorCode::SchemaViolation,
                        err->field_path + ": " + err->message
                    );
                }
            } else {
                SchemaRegistry emptyRegistry;
                if (const auto err = emptyRegistry.ValidateWithSchema(
                        schema,
                        *decoded.value,
                        options.max_depth
                    )) {
                    return CodecResult<Value>::Failure(
                        CodecErrorCode::SchemaViolation,
                        err->field_path + ": " + err->message
                    );
                }
            }
        }

        return decoded;
    }

private:
    static std::optional<CodecError> CoerceAndApplyDefaults(
        const MessageSchema& schema,
        Value& val,
        const SchemaRegistry* registry,
        const JsonCodecOptions& options,
        std::size_t depth
    ) {
        if (depth > options.max_depth) {
            return CodecError{
                CodecErrorCode::MaxDepthExceeded,
                "JSON schema coercion exceeded max_depth"
            };
        }

        auto* obj = val.AsObjectMutable();
        if (!obj) {
            return CodecError{
                CodecErrorCode::SchemaViolation,
                "Expected JSON object for message schema"
            };
        }

        for (const auto& field : schema.fields) {
            Value* entry = val.FindMutable(field.name);
            if (!entry || entry->IsNull()) {
                if (options.apply_defaults &&
                    field.default_value.has_value()) {
                    val.Set(field.name, *field.default_value);
                }
                continue;
            }

            if (field.type == FieldType::Bytes) {
                if (field.repeated) {
                    auto* arr = entry->AsArrayMutable();
                    if (arr) {
                        for (auto& item : *arr) {
                            if (const auto s = item.AsString()) {
                                auto b = Base64::Decode(*s);
                                if (!b) {
                                    return CodecError{
                                        CodecErrorCode::InvalidInput,
                                        "Invalid base64 for repeated bytes field " +
                                            field.name
                                    };
                                }
                                item = Value(std::move(*b));
                            }
                        }
                    }
                } else if (const auto s = entry->AsString()) {
                    auto b = Base64::Decode(*s);
                    if (!b) {
                        return CodecError{
                            CodecErrorCode::InvalidInput,
                            "Invalid base64 for bytes field " +
                                field.name
                        };
                    }
                    *entry = Value(std::move(*b));
                }
            } else if (field.type == FieldType::Message && registry) {
                const auto nested = registry->Find(field.nested_schema);
                if (!nested) {
                    continue;
                }
                if (field.repeated) {
                    if (auto* arr = entry->AsArrayMutable()) {
                        for (auto& item : *arr) {
                            if (auto err = CoerceAndApplyDefaults(
                                    *nested,
                                    item,
                                    registry,
                                    options,
                                    depth + 1
                                )) {
                                return err;
                            }
                        }
                    }
                } else {
                    if (auto err = CoerceAndApplyDefaults(
                            *nested,
                            *entry,
                            registry,
                            options,
                            depth + 1
                        )) {
                        return err;
                    }
                }
            }
        }

        return std::nullopt;
    }

    static std::optional<CodecError> WriteValue(
        const Value& value,
        const JsonCodecOptions& options,
        std::size_t depth,
        std::string& out
    ) {
        if (depth > options.max_depth) {
            return CodecError{
                CodecErrorCode::MaxDepthExceeded,
                "JSON encode exceeded max_depth"
            };
        }

        switch (value.Kind()) {
            case ValueKind::Null:
                out.append("null");
                return std::nullopt;

            case ValueKind::Bool:
                out.append(*value.AsBool() ? "true" : "false");
                return std::nullopt;

            case ValueKind::Int64: {
                std::array<char, 32> buf{};
                const auto [ptr, ec] = std::to_chars(
                    buf.data(),
                    buf.data() + buf.size(),
                    *value.AsInt64()
                );
                if (ec != std::errc{}) {
                    return CodecError{
                        CodecErrorCode::InvalidInput,
                        "Failed to format int64"
                    };
                }
                out.append(buf.data(), ptr);
                return std::nullopt;
            }

            case ValueKind::UInt64: {
                std::array<char, 32> buf{};
                const auto [ptr, ec] = std::to_chars(
                    buf.data(),
                    buf.data() + buf.size(),
                    *value.AsUInt64()
                );
                if (ec != std::errc{}) {
                    return CodecError{
                        CodecErrorCode::InvalidInput,
                        "Failed to format uint64"
                    };
                }
                out.append(buf.data(), ptr);
                return std::nullopt;
            }

            case ValueKind::Double: {
                const double d = *value.AsDouble();
                if (!std::isfinite(d)) {
                    return CodecError{
                        CodecErrorCode::InvalidInput,
                        "JSON does not support non-finite numbers"
                    };
                }
                std::array<char, 64> buf{};
                const auto [ptr, ec] = std::to_chars(
                    buf.data(),
                    buf.data() + buf.size(),
                    d
                );
                if (ec != std::errc{}) {
                    return CodecError{
                        CodecErrorCode::InvalidInput,
                        "Failed to format double"
                    };
                }
                out.append(buf.data(), ptr);
                return std::nullopt;
            }

            case ValueKind::String:
                WriteEscapedString(*value.AsString(), out);
                return std::nullopt;

            case ValueKind::Bytes: {
                const auto encoded = Base64::Encode(*value.AsBytes());
                WriteEscapedString(encoded, out);
                return std::nullopt;
            }

            case ValueKind::Array: {
                const auto& arr = *value.AsArray();
                out.push_back('[');
                for (std::size_t i = 0; i < arr.size(); ++i) {
                    if (i > 0) out.push_back(',');
                    if (auto err = WriteValue(
                            arr[i],
                            options,
                            depth + 1,
                            out
                        )) {
                        return err;
                    }
                    if (out.size() > options.max_output_bytes) {
                        return CodecError{
                            CodecErrorCode::PayloadTooLarge,
                            "Encoded JSON exceeds max_output_bytes"
                        };
                    }
                }
                out.push_back(']');
                return std::nullopt;
            }

            case ValueKind::Object: {
                const auto& obj = *value.AsObject();
                out.push_back('{');
                for (std::size_t i = 0; i < obj.size(); ++i) {
                    if (i > 0) out.push_back(',');
                    WriteEscapedString(obj[i].first, out);
                    out.push_back(':');
                    if (auto err = WriteValue(
                            obj[i].second,
                            options,
                            depth + 1,
                            out
                        )) {
                        return err;
                    }
                    if (out.size() > options.max_output_bytes) {
                        return CodecError{
                            CodecErrorCode::PayloadTooLarge,
                            "Encoded JSON exceeds max_output_bytes"
                        };
                    }
                }
                out.push_back('}');
                return std::nullopt;
            }
        }

        return CodecError{
            CodecErrorCode::InvalidInput,
            "Unknown value kind"
        };
    }

    static void WriteEscapedString(
        std::string_view text,
        std::string& out
    ) {
        static constexpr char kHex[] = "0123456789abcdef";
        out.push_back('"');
        for (const unsigned char ch : text) {
            switch (ch) {
                case '"': out.append("\\\""); break;
                case '\\': out.append("\\\\"); break;
                case '\b': out.append("\\b"); break;
                case '\f': out.append("\\f"); break;
                case '\n': out.append("\\n"); break;
                case '\r': out.append("\\r"); break;
                case '\t': out.append("\\t"); break;
                default:
                    if (ch < 0x20U) {
                        out.append("\\u00");
                        out.push_back(kHex[(ch >> 4U) & 0x0FU]);
                        out.push_back(kHex[ch & 0x0FU]);
                    } else {
                        out.push_back(static_cast<char>(ch));
                    }
                    break;
            }
        }
        out.push_back('"');
    }

    class Parser final {
    public:
        Parser(std::string_view input, std::size_t maxDepth)
            : input_(input), maxDepth_(maxDepth) {}

        std::optional<CodecError> ParseRoot(Value& out) {
            SkipWhitespace();
            if (auto err = ParseValue(out, 0)) {
                return err;
            }
            SkipWhitespace();
            if (!AtEnd()) {
                return CodecError{
                    CodecErrorCode::MalformedWireData,
                    "Unexpected trailing characters in JSON"
                };
            }
            return std::nullopt;
        }

    private:
        [[nodiscard]] bool AtEnd() const noexcept {
            return pos_ >= input_.size();
        }

        void SkipWhitespace() noexcept {
            while (pos_ < input_.size()) {
                const char ch = input_[pos_];
                if (ch == ' ' || ch == '\t' ||
                    ch == '\n' || ch == '\r') {
                    ++pos_;
                } else {
                    break;
                }
            }
        }

        std::optional<CodecError> ParseValue(
            Value& out,
            std::size_t depth
        ) {
            if (depth > maxDepth_) {
                return CodecError{
                    CodecErrorCode::MaxDepthExceeded,
                    "JSON decode exceeded max_depth"
                };
            }

            SkipWhitespace();
            if (AtEnd()) {
                return CodecError{
                    CodecErrorCode::MalformedWireData,
                    "Unexpected end of JSON input"
                };
            }

            const char ch = input_[pos_];
            if (ch == 'n') return ParseLiteral("null", Value::MakeNull(), out);
            if (ch == 't') return ParseLiteral("true", Value(true), out);
            if (ch == 'f') return ParseLiteral("false", Value(false), out);
            if (ch == '"') {
                std::string s;
                if (auto err = ParseString(s)) return err;
                out = Value(std::move(s));
                return std::nullopt;
            }
            if (ch == '[') return ParseArray(out, depth);
            if (ch == '{') return ParseObject(out, depth);
            if (ch == '-' || (ch >= '0' && ch <= '9')) {
                return ParseNumber(out);
            }

            return CodecError{
                CodecErrorCode::MalformedWireData,
                "Invalid JSON token"
            };
        }

        std::optional<CodecError> ParseLiteral(
            std::string_view expected,
            Value value,
            Value& out
        ) {
            if (input_.substr(pos_, expected.size()) != expected) {
                return CodecError{
                    CodecErrorCode::MalformedWireData,
                    "Invalid JSON literal"
                };
            }
            pos_ += expected.size();
            out = std::move(value);
            return std::nullopt;
        }

        static void AppendUtf8(std::uint32_t cp, std::string& out) {
            if (cp <= 0x7FU) {
                out.push_back(static_cast<char>(cp));
            } else if (cp <= 0x7FFU) {
                out.push_back(static_cast<char>(0xC0U | (cp >> 6U)));
                out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
            } else if (cp <= 0xFFFFU) {
                out.push_back(static_cast<char>(0xE0U | (cp >> 12U)));
                out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
                out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
            } else {
                out.push_back(static_cast<char>(0xF0U | (cp >> 18U)));
                out.push_back(static_cast<char>(0x80U | ((cp >> 12U) & 0x3FU)));
                out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
                out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
            }
        }

        bool ParseHex4(std::uint16_t& out) noexcept {
            if (pos_ + 4 > input_.size()) {
                return false;
            }
            std::uint16_t val = 0;
            for (int i = 0; i < 4; ++i) {
                const char ch = input_[pos_++];
                val <<= 4U;
                if (ch >= '0' && ch <= '9') {
                    val |= static_cast<std::uint16_t>(ch - '0');
                } else if (ch >= 'a' && ch <= 'f') {
                    val |= static_cast<std::uint16_t>(ch - 'a' + 10);
                } else if (ch >= 'A' && ch <= 'F') {
                    val |= static_cast<std::uint16_t>(ch - 'A' + 10);
                } else {
                    return false;
                }
            }
            out = val;
            return true;
        }

        std::optional<CodecError> ParseString(std::string& out) {
            if (AtEnd() || input_[pos_] != '"') {
                return CodecError{
                    CodecErrorCode::MalformedWireData,
                    "Expected '\"'"
                };
            }
            ++pos_;

            out.clear();
            while (pos_ < input_.size()) {
                const unsigned char ch =
                    static_cast<unsigned char>(input_[pos_++]);
                if (ch == '"') {
                    return std::nullopt;
                }
                if (ch < 0x20U) {
                    return CodecError{
                        CodecErrorCode::MalformedWireData,
                        "Unescaped control character in JSON string"
                    };
                }
                if (ch != '\\') {
                    out.push_back(static_cast<char>(ch));
                    continue;
                }

                if (AtEnd()) {
                    return CodecError{
                        CodecErrorCode::MalformedWireData,
                        "Unterminated escape sequence"
                    };
                }

                const char esc = input_[pos_++];
                switch (esc) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        std::uint16_t unit1 = 0;
                        if (!ParseHex4(unit1)) {
                            return CodecError{
                                CodecErrorCode::MalformedWireData,
                                "Invalid \\uXXXX escape"
                            };
                        }
                        if (unit1 >= 0xD800U && unit1 <= 0xDBFFU) {
                            if (pos_ + 2 > input_.size() ||
                                input_[pos_] != '\\' ||
                                input_[pos_ + 1] != 'u') {
                                return CodecError{
                                    CodecErrorCode::MalformedWireData,
                                    "Missing low surrogate in JSON string"
                                };
                            }
                            pos_ += 2;
                            std::uint16_t unit2 = 0;
                            if (!ParseHex4(unit2) ||
                                unit2 < 0xDC00U ||
                                unit2 > 0xDFFFU) {
                                return CodecError{
                                    CodecErrorCode::MalformedWireData,
                                    "Invalid low surrogate in JSON string"
                                };
                            }
                            const std::uint32_t cp =
                                0x10000U +
                                ((static_cast<std::uint32_t>(unit1 - 0xD800U)
                                  << 10U) |
                                 static_cast<std::uint32_t>(unit2 - 0xDC00U));
                            AppendUtf8(cp, out);
                        } else if (unit1 >= 0xDC00U && unit1 <= 0xDFFFU) {
                            return CodecError{
                                CodecErrorCode::MalformedWireData,
                                "Unexpected lone low surrogate"
                            };
                        } else {
                            AppendUtf8(unit1, out);
                        }
                        break;
                    }
                    default:
                        return CodecError{
                            CodecErrorCode::MalformedWireData,
                            "Invalid escape character in JSON string"
                        };
                }
            }

            return CodecError{
                CodecErrorCode::MalformedWireData,
                "Unterminated JSON string"
            };
        }

        std::optional<CodecError> ParseNumber(Value& out) {
            const std::size_t start = pos_;
            bool isFloat = false;
            bool negative = false;

            if (input_[pos_] == '-') {
                negative = true;
                ++pos_;
            }

            if (AtEnd() || input_[pos_] < '0' || input_[pos_] > '9') {
                return CodecError{
                    CodecErrorCode::MalformedWireData,
                    "Invalid JSON number"
                };
            }

            if (input_[pos_] == '0') {
                ++pos_;
                if (!AtEnd() &&
                    input_[pos_] >= '0' &&
                    input_[pos_] <= '9') {
                    return CodecError{
                        CodecErrorCode::MalformedWireData,
                        "Leading zero not permitted in JSON number"
                    };
                }
            } else {
                while (!AtEnd() &&
                       input_[pos_] >= '0' &&
                       input_[pos_] <= '9') {
                    ++pos_;
                }
            }

            if (!AtEnd() && input_[pos_] == '.') {
                isFloat = true;
                ++pos_;
                if (AtEnd() ||
                    input_[pos_] < '0' ||
                    input_[pos_] > '9') {
                    return CodecError{
                        CodecErrorCode::MalformedWireData,
                        "Invalid fraction in JSON number"
                    };
                }
                while (!AtEnd() &&
                       input_[pos_] >= '0' &&
                       input_[pos_] <= '9') {
                    ++pos_;
                }
            }

            if (!AtEnd() &&
                (input_[pos_] == 'e' || input_[pos_] == 'E')) {
                isFloat = true;
                ++pos_;
                if (!AtEnd() &&
                    (input_[pos_] == '+' || input_[pos_] == '-')) {
                    ++pos_;
                }
                if (AtEnd() ||
                    input_[pos_] < '0' ||
                    input_[pos_] > '9') {
                    return CodecError{
                        CodecErrorCode::MalformedWireData,
                        "Invalid exponent in JSON number"
                    };
                }
                while (!AtEnd() &&
                       input_[pos_] >= '0' &&
                       input_[pos_] <= '9') {
                    ++pos_;
                }
            }

            const std::string_view token =
                input_.substr(start, pos_ - start);

            if (!isFloat) {
                if (negative) {
                    std::int64_t val = 0;
                    const auto [ptr, ec] = std::from_chars(
                        token.data(),
                        token.data() + token.size(),
                        val
                    );
                    if (ec == std::errc{} &&
                        ptr == token.data() + token.size()) {
                        out = Value(val);
                        return std::nullopt;
                    }
                } else {
                    std::int64_t sval = 0;
                    const auto [sptr, sec] = std::from_chars(
                        token.data(),
                        token.data() + token.size(),
                        sval
                    );
                    if (sec == std::errc{} &&
                        sptr == token.data() + token.size()) {
                        out = Value(sval);
                        return std::nullopt;
                    }

                    std::uint64_t uval = 0;
                    const auto [uptr, uec] = std::from_chars(
                        token.data(),
                        token.data() + token.size(),
                        uval
                    );
                    if (uec == std::errc{} &&
                        uptr == token.data() + token.size()) {
                        out = Value(uval);
                        return std::nullopt;
                    }
                }
            }

            double dval = 0.0;
            const auto [ptr, ec] = std::from_chars(
                token.data(),
                token.data() + token.size(),
                dval
            );
            if (ec != std::errc{} ||
                ptr != token.data() + token.size() ||
                !std::isfinite(dval)) {
                return CodecError{
                    CodecErrorCode::MalformedWireData,
                    "JSON number out of range"
                };
            }

            out = Value(dval);
            return std::nullopt;
        }

        std::optional<CodecError> ParseArray(
            Value& out,
            std::size_t depth
        ) {
            ++pos_; // consume '['
            Value::Array arr;
            SkipWhitespace();

            if (!AtEnd() && input_[pos_] == ']') {
                ++pos_;
                out = Value(std::move(arr));
                return std::nullopt;
            }

            while (true) {
                Value item;
                if (auto err = ParseValue(item, depth + 1)) {
                    return err;
                }
                arr.push_back(std::move(item));

                SkipWhitespace();
                if (AtEnd()) {
                    return CodecError{
                        CodecErrorCode::MalformedWireData,
                        "Unterminated JSON array"
                    };
                }
                if (input_[pos_] == ']') {
                    ++pos_;
                    out = Value(std::move(arr));
                    return std::nullopt;
                }
                if (input_[pos_] != ',') {
                    return CodecError{
                        CodecErrorCode::MalformedWireData,
                        "Expected ',' or ']' in JSON array"
                    };
                }
                ++pos_;
            }
        }

        std::optional<CodecError> ParseObject(
            Value& out,
            std::size_t depth
        ) {
            ++pos_; // consume '{'
            Value obj = Value::MakeObject();
            SkipWhitespace();

            if (!AtEnd() && input_[pos_] == '}') {
                ++pos_;
                out = std::move(obj);
                return std::nullopt;
            }

            while (true) {
                SkipWhitespace();
                std::string key;
                if (auto err = ParseString(key)) {
                    return err;
                }

                SkipWhitespace();
                if (AtEnd() || input_[pos_] != ':') {
                    return CodecError{
                        CodecErrorCode::MalformedWireData,
                        "Expected ':' after JSON object key"
                    };
                }
                ++pos_;

                Value val;
                if (auto err = ParseValue(val, depth + 1)) {
                    return err;
                }
                obj.Set(std::move(key), std::move(val));

                SkipWhitespace();
                if (AtEnd()) {
                    return CodecError{
                        CodecErrorCode::MalformedWireData,
                        "Unterminated JSON object"
                    };
                }
                if (input_[pos_] == '}') {
                    ++pos_;
                    out = std::move(obj);
                    return std::nullopt;
                }
                if (input_[pos_] != ',') {
                    return CodecError{
                        CodecErrorCode::MalformedWireData,
                        "Expected ',' or '}' in JSON object"
                    };
                }
                ++pos_;
            }
        }

        std::string_view input_;
        std::size_t pos_ = 0;
        std::size_t maxDepth_;
    };
};

} // namespace lamp::serialization
