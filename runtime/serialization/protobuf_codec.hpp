#pragma once

// ============================================================
// Lamp Framework
// Phase 7 - Serialization
//
// File: runtime/serialization/protobuf_codec.hpp
//
// Purpose:
//   Schema-driven Protobuf wire encoder and decoder over
//   lamp::serialization::Value with strict bounds and
//   validation.
// ============================================================

#include "serialization/protobuf_wire.hpp"
#include "serialization/schema_registry.hpp"
#include "serialization/serialization_value.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace lamp::serialization {

enum class CodecErrorCode : std::uint8_t {
    None = 0,
    InvalidInput,
    MalformedWireData,
    SchemaViolation,
    MissingNestedSchema,
    PayloadTooLarge,
    MaxDepthExceeded,
    UnsupportedFormat
};

struct CodecError final {
    CodecErrorCode code = CodecErrorCode::None;
    std::string message;
};

template <typename T>
struct CodecResult final {
    std::optional<T> value;
    CodecError error;

    [[nodiscard]]
    bool Ok() const noexcept {
        return value.has_value() &&
               error.code == CodecErrorCode::None;
    }

    [[nodiscard]]
    explicit operator bool() const noexcept {
        return Ok();
    }

    [[nodiscard]]
    static CodecResult Success(T val) {
        CodecResult res;
        res.value.emplace(std::move(val));
        return res;
    }

    [[nodiscard]]
    static CodecResult Failure(
        CodecErrorCode code,
        std::string message
    ) {
        CodecResult res;
        res.error = CodecError{code, std::move(message)};
        return res;
    }
};

struct ProtobufCodecOptions final {
    std::size_t max_depth = 32;
    std::size_t max_message_bytes = 16U * 1024U * 1024U;
    bool validate = true;
    bool apply_defaults = true;
};

class ProtobufCodec final {
public:
    [[nodiscard]]
    static CodecResult<std::vector<std::uint8_t>> Encode(
        const MessageSchema& schema,
        const Value& value,
        const SchemaRegistry* registry = nullptr,
        ProtobufCodecOptions options = {}
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

        ProtobufWriter writer;
        if (auto err = EncodeMessage(
                schema,
                value,
                registry,
                options,
                0,
                writer
            )) {
            return CodecResult<std::vector<std::uint8_t>>::Failure(
                err->code,
                std::move(err->message)
            );
        }

        auto bytes = writer.Take();
        if (bytes.size() > options.max_message_bytes) {
            return CodecResult<std::vector<std::uint8_t>>::Failure(
                CodecErrorCode::PayloadTooLarge,
                "Encoded Protobuf payload exceeds max_message_bytes"
            );
        }

        return CodecResult<std::vector<std::uint8_t>>::Success(
            std::move(bytes)
        );
    }

    [[nodiscard]]
    static CodecResult<Value> Decode(
        const MessageSchema& schema,
        std::span<const std::uint8_t> bytes,
        const SchemaRegistry* registry = nullptr,
        ProtobufCodecOptions options = {}
    ) {
        if (bytes.size() > options.max_message_bytes) {
            return CodecResult<Value>::Failure(
                CodecErrorCode::PayloadTooLarge,
                "Protobuf payload exceeds max_message_bytes"
            );
        }

        Value result;
        if (auto err = DecodeMessage(
                schema,
                bytes,
                registry,
                options,
                0,
                result
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
                        result,
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
                        result,
                        options.max_depth
                    )) {
                    return CodecResult<Value>::Failure(
                        CodecErrorCode::SchemaViolation,
                        err->field_path + ": " + err->message
                    );
                }
            }
        }

        return CodecResult<Value>::Success(std::move(result));
    }

private:
    [[nodiscard]]
    static std::optional<CodecError> EncodeMessage(
        const MessageSchema& schema,
        const Value& value,
        const SchemaRegistry* registry,
        const ProtobufCodecOptions& options,
        std::size_t depth,
        ProtobufWriter& writer
    ) {
        if (depth > options.max_depth) {
            return CodecError{
                CodecErrorCode::MaxDepthExceeded,
                "Protobuf encode exceeded max_depth"
            };
        }

        const auto* obj = value.AsObject();
        if (!obj) {
            return CodecError{
                CodecErrorCode::SchemaViolation,
                "Expected object value for Protobuf message"
            };
        }

        for (const auto& field : schema.fields) {
            const Value* entry = value.Find(field.name);
            if (!entry || entry->IsNull()) {
                continue;
            }

            if (field.repeated) {
                const auto* arr = entry->AsArray();
                if (!arr) {
                    return CodecError{
                        CodecErrorCode::SchemaViolation,
                        "Expected array for repeated field " +
                            field.name
                    };
                }

                if (arr->empty()) {
                    continue;
                }

                if (field.packed && IsPackableScalar(field.type)) {
                    ProtobufWriter packedWriter;
                    for (const auto& item : *arr) {
                        if (auto err = EncodeScalarValue(
                                field,
                                item,
                                packedWriter
                            )) {
                            return err;
                        }
                    }
                    writer.WriteTag(
                        field.field_number,
                        WireType::LengthDelimited
                    );
                    writer.WriteBytes(packedWriter.Data());
                } else {
                    for (const auto& item : *arr) {
                        if (auto err = EncodeFieldElement(
                                field,
                                item,
                                registry,
                                options,
                                depth,
                                writer
                            )) {
                            return err;
                        }
                    }
                }
            } else {
                if (auto err = EncodeFieldElement(
                        field,
                        *entry,
                        registry,
                        options,
                        depth,
                        writer
                    )) {
                    return err;
                }
            }
        }

        return std::nullopt;
    }

    [[nodiscard]]
    static std::optional<CodecError> EncodeFieldElement(
        const FieldDescriptor& field,
        const Value& item,
        const SchemaRegistry* registry,
        const ProtobufCodecOptions& options,
        std::size_t depth,
        ProtobufWriter& writer
    ) {
        switch (field.type) {
            case FieldType::Bool:
            case FieldType::Int64:
            case FieldType::SInt64:
            case FieldType::UInt64:
                writer.WriteTag(field.field_number, WireType::Varint);
                return EncodeScalarValue(field, item, writer);

            case FieldType::Double:
                writer.WriteTag(field.field_number, WireType::Fixed64);
                return EncodeScalarValue(field, item, writer);

            case FieldType::String: {
                const auto str = item.AsString();
                if (!str) {
                    return CodecError{
                        CodecErrorCode::SchemaViolation,
                        "Expected string for field " + field.name
                    };
                }
                writer.WriteTag(
                    field.field_number,
                    WireType::LengthDelimited
                );
                writer.WriteString(*str);
                return std::nullopt;
            }

            case FieldType::Bytes: {
                const auto* bytes = item.AsBytes();
                if (!bytes) {
                    return CodecError{
                        CodecErrorCode::SchemaViolation,
                        "Expected bytes for field " + field.name
                    };
                }
                writer.WriteTag(
                    field.field_number,
                    WireType::LengthDelimited
                );
                writer.WriteBytes(*bytes);
                return std::nullopt;
            }

            case FieldType::Message: {
                if (!registry) {
                    return CodecError{
                        CodecErrorCode::MissingNestedSchema,
                        "SchemaRegistry required for nested message " +
                            field.nested_schema.name
                    };
                }
                const auto nested = registry->Find(field.nested_schema);
                if (!nested) {
                    return CodecError{
                        CodecErrorCode::MissingNestedSchema,
                        "Nested schema not found: " +
                            field.nested_schema.name
                    };
                }

                ProtobufWriter nestedWriter;
                if (auto err = EncodeMessage(
                        *nested,
                        item,
                        registry,
                        options,
                        depth + 1,
                        nestedWriter
                    )) {
                    return err;
                }
                writer.WriteTag(
                    field.field_number,
                    WireType::LengthDelimited
                );
                writer.WriteBytes(nestedWriter.Data());
                return std::nullopt;
            }
        }

        return CodecError{
            CodecErrorCode::InvalidInput,
            "Unsupported field type"
        };
    }

    [[nodiscard]]
    static std::optional<CodecError> EncodeScalarValue(
        const FieldDescriptor& field,
        const Value& item,
        ProtobufWriter& writer
    ) {
        switch (field.type) {
            case FieldType::Bool: {
                const auto b = item.AsBool();
                if (!b) {
                    return CodecError{
                        CodecErrorCode::SchemaViolation,
                        "Expected bool for field " + field.name
                    };
                }
                writer.WriteVarint(*b ? 1ULL : 0ULL);
                return std::nullopt;
            }

            case FieldType::Int64: {
                const auto v = item.AsInt64();
                if (!v) {
                    return CodecError{
                        CodecErrorCode::SchemaViolation,
                        "Expected int64 for field " + field.name
                    };
                }
                writer.WriteSignedVarint(*v);
                return std::nullopt;
            }

            case FieldType::SInt64: {
                const auto v = item.AsInt64();
                if (!v) {
                    return CodecError{
                        CodecErrorCode::SchemaViolation,
                        "Expected sint64 for field " + field.name
                    };
                }
                writer.WriteZigZagVarint(*v);
                return std::nullopt;
            }

            case FieldType::UInt64: {
                const auto v = item.AsUInt64();
                if (!v) {
                    return CodecError{
                        CodecErrorCode::SchemaViolation,
                        "Expected uint64 for field " + field.name
                    };
                }
                writer.WriteVarint(*v);
                return std::nullopt;
            }

            case FieldType::Double: {
                const auto v = item.AsDouble();
                if (!v) {
                    return CodecError{
                        CodecErrorCode::SchemaViolation,
                        "Expected double for field " + field.name
                    };
                }
                writer.WriteDouble(*v);
                return std::nullopt;
            }

            default:
                return CodecError{
                    CodecErrorCode::InvalidInput,
                    "Non-scalar type in scalar encoder"
                };
        }
    }

    [[nodiscard]]
    static std::optional<CodecError> DecodeMessage(
        const MessageSchema& schema,
        std::span<const std::uint8_t> bytes,
        const SchemaRegistry* registry,
        const ProtobufCodecOptions& options,
        std::size_t depth,
        Value& out
    ) {
        if (depth > options.max_depth) {
            return CodecError{
                CodecErrorCode::MaxDepthExceeded,
                "Protobuf decode exceeded max_depth"
            };
        }

        out = Value::MakeObject();
        ProtobufReader reader(bytes);

        while (!reader.AtEnd()) {
            std::uint32_t fieldNumber = 0;
            WireType wireType = WireType::Varint;
            if (!reader.ReadTag(fieldNumber, wireType)) {
                return CodecError{
                    CodecErrorCode::MalformedWireData,
                    "Malformed Protobuf field tag"
                };
            }

            const FieldDescriptor* field =
                schema.FindByNumber(fieldNumber);
            if (!field) {
                if (!schema.allow_unknown_fields) {
                    return CodecError{
                        CodecErrorCode::SchemaViolation,
                        "Unknown Protobuf field number " +
                            std::to_string(fieldNumber)
                    };
                }
                if (!reader.SkipField(wireType)) {
                    return CodecError{
                        CodecErrorCode::MalformedWireData,
                        "Malformed unknown Protobuf field"
                    };
                }
                continue;
            }

            if (field->repeated &&
                IsPackableScalar(field->type) &&
                wireType == WireType::LengthDelimited) {
                std::span<const std::uint8_t> packedSlice;
                if (!reader.ReadLengthDelimited(packedSlice)) {
                    return CodecError{
                        CodecErrorCode::MalformedWireData,
                        "Truncated packed repeated field"
                    };
                }

                ProtobufReader packedReader(packedSlice);
                Value* arrVal = out.FindMutable(field->name);
                if (!arrVal) {
                    out.Set(field->name, Value::MakeArray());
                    arrVal = out.FindMutable(field->name);
                }

                const WireType elemWire =
                    field->type == FieldType::Double
                        ? WireType::Fixed64
                        : WireType::Varint;

                while (!packedReader.AtEnd()) {
                    Value decodedElem;
                    if (auto err = DecodeScalarValue(
                            *field,
                            elemWire,
                            packedReader,
                            decodedElem
                        )) {
                        return err;
                    }
                    arrVal->PushBack(std::move(decodedElem));
                }
                continue;
            }

            Value decoded;
            if (auto err = DecodeFieldElement(
                    *field,
                    wireType,
                    reader,
                    registry,
                    options,
                    depth,
                    decoded
                )) {
                return err;
            }

            if (field->repeated) {
                Value* arrVal = out.FindMutable(field->name);
                if (!arrVal) {
                    out.Set(field->name, Value::MakeArray());
                    arrVal = out.FindMutable(field->name);
                }
                arrVal->PushBack(std::move(decoded));
            } else {
                out.Set(field->name, std::move(decoded));
            }
        }

        if (options.apply_defaults) {
            for (const auto& field : schema.fields) {
                if (field.default_value.has_value() &&
                    !out.Find(field.name)) {
                    out.Set(field.name, *field.default_value);
                }
            }
        }

        return std::nullopt;
    }

    [[nodiscard]]
    static std::optional<CodecError> DecodeFieldElement(
        const FieldDescriptor& field,
        WireType wireType,
        ProtobufReader& reader,
        const SchemaRegistry* registry,
        const ProtobufCodecOptions& options,
        std::size_t depth,
        Value& out
    ) {
        switch (field.type) {
            case FieldType::Bool:
            case FieldType::Int64:
            case FieldType::SInt64:
            case FieldType::UInt64:
            case FieldType::Double:
                return DecodeScalarValue(field, wireType, reader, out);

            case FieldType::String: {
                if (wireType != WireType::LengthDelimited) {
                    return CodecError{
                        CodecErrorCode::MalformedWireData,
                        "Wire type mismatch for string field " +
                            field.name
                    };
                }
                std::span<const std::uint8_t> slice;
                if (!reader.ReadLengthDelimited(slice)) {
                    return CodecError{
                        CodecErrorCode::MalformedWireData,
                        "Truncated string field " + field.name
                    };
                }
                std::string str(
                    reinterpret_cast<const char*>(slice.data()),
                    slice.size()
                );
                out = Value(std::move(str));
                return std::nullopt;
            }

            case FieldType::Bytes: {
                if (wireType != WireType::LengthDelimited) {
                    return CodecError{
                        CodecErrorCode::MalformedWireData,
                        "Wire type mismatch for bytes field " +
                            field.name
                    };
                }
                std::span<const std::uint8_t> slice;
                if (!reader.ReadLengthDelimited(slice)) {
                    return CodecError{
                        CodecErrorCode::MalformedWireData,
                        "Truncated bytes field " + field.name
                    };
                }
                Value::Bytes bytes(slice.begin(), slice.end());
                out = Value(std::move(bytes));
                return std::nullopt;
            }

            case FieldType::Message: {
                if (wireType != WireType::LengthDelimited) {
                    return CodecError{
                        CodecErrorCode::MalformedWireData,
                        "Wire type mismatch for message field " +
                            field.name
                    };
                }
                if (!registry) {
                    return CodecError{
                        CodecErrorCode::MissingNestedSchema,
                        "SchemaRegistry required for nested message " +
                            field.nested_schema.name
                    };
                }
                const auto nested = registry->Find(field.nested_schema);
                if (!nested) {
                    return CodecError{
                        CodecErrorCode::MissingNestedSchema,
                        "Nested schema not found: " +
                            field.nested_schema.name
                    };
                }
                std::span<const std::uint8_t> slice;
                if (!reader.ReadLengthDelimited(slice)) {
                    return CodecError{
                        CodecErrorCode::MalformedWireData,
                        "Truncated nested message " + field.name
                    };
                }
                return DecodeMessage(
                    *nested,
                    slice,
                    registry,
                    options,
                    depth + 1,
                    out
                );
            }
        }

        return CodecError{
            CodecErrorCode::InvalidInput,
            "Unsupported field type"
        };
    }

    [[nodiscard]]
    static std::optional<CodecError> DecodeScalarValue(
        const FieldDescriptor& field,
        WireType wireType,
        ProtobufReader& reader,
        Value& out
    ) {
        switch (field.type) {
            case FieldType::Bool: {
                if (wireType != WireType::Varint) {
                    return CodecError{
                        CodecErrorCode::MalformedWireData,
                        "Expected varint for bool field " + field.name
                    };
                }
                std::uint64_t raw = 0;
                if (!reader.ReadVarint(raw)) {
                    return CodecError{
                        CodecErrorCode::MalformedWireData,
                        "Truncated bool varint"
                    };
                }
                out = Value(raw != 0);
                return std::nullopt;
            }

            case FieldType::Int64: {
                if (wireType != WireType::Varint) {
                    return CodecError{
                        CodecErrorCode::MalformedWireData,
                        "Expected varint for int64 field " + field.name
                    };
                }
                std::uint64_t raw = 0;
                if (!reader.ReadVarint(raw)) {
                    return CodecError{
                        CodecErrorCode::MalformedWireData,
                        "Truncated int64 varint"
                    };
                }
                out = Value(static_cast<std::int64_t>(raw));
                return std::nullopt;
            }

            case FieldType::SInt64: {
                if (wireType != WireType::Varint) {
                    return CodecError{
                        CodecErrorCode::MalformedWireData,
                        "Expected varint for sint64 field " +
                            field.name
                    };
                }
                std::uint64_t raw = 0;
                if (!reader.ReadVarint(raw)) {
                    return CodecError{
                        CodecErrorCode::MalformedWireData,
                        "Truncated sint64 varint"
                    };
                }
                out = Value(ZigZagDecode64(raw));
                return std::nullopt;
            }

            case FieldType::UInt64: {
                if (wireType != WireType::Varint) {
                    return CodecError{
                        CodecErrorCode::MalformedWireData,
                        "Expected varint for uint64 field " +
                            field.name
                    };
                }
                std::uint64_t raw = 0;
                if (!reader.ReadVarint(raw)) {
                    return CodecError{
                        CodecErrorCode::MalformedWireData,
                        "Truncated uint64 varint"
                    };
                }
                out = Value(raw);
                return std::nullopt;
            }

            case FieldType::Double: {
                if (wireType != WireType::Fixed64) {
                    return CodecError{
                        CodecErrorCode::MalformedWireData,
                        "Expected fixed64 for double field " +
                            field.name
                    };
                }
                double d = 0.0;
                if (!reader.ReadDouble(d)) {
                    return CodecError{
                        CodecErrorCode::MalformedWireData,
                        "Truncated double fixed64"
                    };
                }
                out = Value(d);
                return std::nullopt;
            }

            default:
                return CodecError{
                    CodecErrorCode::InvalidInput,
                    "Non-scalar type in scalar decoder"
                };
        }
    }
};

} // namespace lamp::serialization
