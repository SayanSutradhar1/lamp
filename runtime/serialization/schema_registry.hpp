#pragma once

// ============================================================
// Lamp Framework
// Phase 7 - Serialization
//
// File: runtime/serialization/schema_registry.hpp
//
// Purpose:
//   Schema definitions, thread-safe schema registry, and
//   recursive value validation for Lamp operations.
// ============================================================

#include "bridge/operation_descriptor.hpp"
#include "serialization/serialization_value.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lamp::serialization {

enum class FieldType : std::uint8_t {
    Bool = 0,
    Int64 = 1,
    SInt64 = 2,
    UInt64 = 3,
    Double = 4,
    String = 5,
    Bytes = 6,
    Message = 7
};

[[nodiscard]]
constexpr bool IsPackableScalar(FieldType type) noexcept {
    switch (type) {
        case FieldType::Bool:
        case FieldType::Int64:
        case FieldType::SInt64:
        case FieldType::UInt64:
        case FieldType::Double:
            return true;
        case FieldType::String:
        case FieldType::Bytes:
        case FieldType::Message:
            return false;
    }
    return false;
}

struct FieldDescriptor final {
    std::uint32_t field_number = 0;
    std::string name;
    FieldType type = FieldType::String;

    bool required = false;
    bool repeated = false;
    bool packed = true;

    std::optional<Value> default_value;
    bridge::SchemaReference nested_schema{};

    std::optional<std::size_t> min_length;
    std::optional<std::size_t> max_length;
    std::optional<double> min_value;
    std::optional<double> max_value;
};

struct MessageSchema final {
    bridge::SchemaReference ref;
    std::vector<FieldDescriptor> fields;
    bool allow_unknown_fields = false;

    [[nodiscard]]
    const FieldDescriptor* FindByName(
        std::string_view fieldName
    ) const noexcept {
        for (const auto& field : fields) {
            if (field.name == fieldName) {
                return &field;
            }
        }
        return nullptr;
    }

    [[nodiscard]]
    const FieldDescriptor* FindByNumber(
        std::uint32_t fieldNumber
    ) const noexcept {
        for (const auto& field : fields) {
            if (field.field_number == fieldNumber) {
                return &field;
            }
        }
        return nullptr;
    }
};

enum class SchemaStatus : std::uint8_t {
    Ok = 0,
    InvalidSchema,
    AlreadyExists,
    NotFound
};

struct SchemaRegisterResult final {
    SchemaStatus status = SchemaStatus::Ok;
    std::string message;

    [[nodiscard]]
    bool Ok() const noexcept {
        return status == SchemaStatus::Ok;
    }
};

enum class SchemaValidationCode : std::uint8_t {
    None = 0,
    SchemaNotFound,
    InvalidType,
    MissingRequiredField,
    UnknownField,
    LengthConstraint,
    RangeConstraint,
    MaxDepthExceeded
};

struct SchemaValidationError final {
    SchemaValidationCode code = SchemaValidationCode::None;
    std::string field_path;
    std::string message;
};

class SchemaRegistry final {
public:
    static constexpr std::uint32_t kMaxFieldNumber = 536870911U;
    static constexpr std::uint32_t kReservedStart = 19000U;
    static constexpr std::uint32_t kReservedEnd = 19999U;

    SchemaRegistry() = default;

    SchemaRegistry(const SchemaRegistry&) = delete;
    SchemaRegistry& operator=(const SchemaRegistry&) = delete;

    [[nodiscard]]
    SchemaRegisterResult Register(MessageSchema schema) {
        if (auto err = ValidateSchemaDefinition(schema)) {
            return {SchemaStatus::InvalidSchema, std::move(*err)};
        }

        const SchemaKey key{schema.ref.name, schema.ref.version};
        auto ptr = std::make_shared<const MessageSchema>(
            std::move(schema)
        );

        std::unique_lock lock(mutex_);
        if (schemas_.find(key) != schemas_.end()) {
            return {
                SchemaStatus::AlreadyExists,
                "Schema already registered"
            };
        }

        schemas_.emplace(key, std::move(ptr));
        return {SchemaStatus::Ok, {}};
    }

    [[nodiscard]]
    SchemaRegisterResult Replace(MessageSchema schema) {
        if (auto err = ValidateSchemaDefinition(schema)) {
            return {SchemaStatus::InvalidSchema, std::move(*err)};
        }

        const SchemaKey key{schema.ref.name, schema.ref.version};
        auto ptr = std::make_shared<const MessageSchema>(
            std::move(schema)
        );

        std::unique_lock lock(mutex_);
        schemas_.insert_or_assign(key, std::move(ptr));
        return {SchemaStatus::Ok, {}};
    }

    [[nodiscard]]
    std::shared_ptr<const MessageSchema> Find(
        std::string_view name,
        std::uint32_t version = 1
    ) const {
        std::shared_lock lock(mutex_);
        const auto it = schemas_.find(
            SchemaKey{std::string(name), version}
        );
        if (it == schemas_.end()) {
            return nullptr;
        }
        return it->second;
    }

    [[nodiscard]]
    std::shared_ptr<const MessageSchema> Find(
        const bridge::SchemaReference& ref
    ) const {
        return Find(ref.name, ref.version);
    }

    [[nodiscard]]
    bool Contains(
        std::string_view name,
        std::uint32_t version = 1
    ) const {
        return Find(name, version) != nullptr;
    }

    [[nodiscard]]
    std::size_t Size() const noexcept {
        std::shared_lock lock(mutex_);
        return schemas_.size();
    }

    [[nodiscard]]
    std::optional<SchemaValidationError> Validate(
        const bridge::SchemaReference& ref,
        const Value& value,
        std::size_t maxDepth = 32
    ) const {
        const auto schema = Find(ref);
        if (!schema) {
            return SchemaValidationError{
                SchemaValidationCode::SchemaNotFound,
                ref.name,
                "Schema not found in registry"
            };
        }
        return ValidateMessage(*schema, value, "", 0, maxDepth);
    }

    [[nodiscard]]
    std::optional<SchemaValidationError> ValidateWithSchema(
        const MessageSchema& schema,
        const Value& value,
        std::size_t maxDepth = 32
    ) const {
        return ValidateMessage(schema, value, "", 0, maxDepth);
    }

private:
    struct SchemaKey final {
        std::string name;
        std::uint32_t version = 1;

        [[nodiscard]]
        friend bool operator==(
            const SchemaKey&,
            const SchemaKey&
        ) noexcept = default;
    };

    struct SchemaKeyHash final {
        [[nodiscard]]
        std::size_t operator()(
            const SchemaKey& key
        ) const noexcept {
            const auto h1 = std::hash<std::string>{}(key.name);
            const auto h2 = std::hash<std::uint32_t>{}(key.version);
            return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL +
                         (h1 << 6) + (h1 >> 2));
        }
    };

    [[nodiscard]]
    static std::optional<std::string> ValidateSchemaDefinition(
        const MessageSchema& schema
    ) {
        if (schema.ref.Empty()) {
            return "Schema name cannot be empty";
        }
        if (schema.ref.version == 0) {
            return "Schema version must be positive";
        }

        std::unordered_set<std::uint32_t> numbers;
        std::unordered_set<std::string> names;

        for (const auto& field : schema.fields) {
            if (field.name.empty()) {
                return "Field name cannot be empty";
            }
            if (field.field_number == 0 ||
                field.field_number > kMaxFieldNumber) {
                return "Field number out of valid Protobuf range";
            }
            if (field.field_number >= kReservedStart &&
                field.field_number <= kReservedEnd) {
                return "Field number falls in Protobuf reserved range";
            }
            if (!numbers.insert(field.field_number).second) {
                return "Duplicate field number in schema";
            }
            if (!names.insert(field.name).second) {
                return "Duplicate field name in schema";
            }
            if (field.type == FieldType::Message &&
                field.nested_schema.Empty()) {
                return "Message field requires nested_schema";
            }
            if (field.min_length && field.max_length &&
                *field.min_length > *field.max_length) {
                return "Invalid min_length > max_length";
            }
            if (field.min_value && field.max_value &&
                *field.min_value > *field.max_value) {
                return "Invalid min_value > max_value";
            }
        }

        return std::nullopt;
    }

    [[nodiscard]]
    static std::string JoinPath(
        std::string_view base,
        std::string_view child
    ) {
        if (base.empty()) {
            return std::string(child);
        }
        std::string out(base);
        out.push_back('.');
        out.append(child);
        return out;
    }

    [[nodiscard]]
    std::optional<SchemaValidationError> ValidateMessage(
        const MessageSchema& schema,
        const Value& value,
        std::string_view path,
        std::size_t depth,
        std::size_t maxDepth
    ) const {
        if (depth > maxDepth) {
            return SchemaValidationError{
                SchemaValidationCode::MaxDepthExceeded,
                std::string(path),
                "Maximum schema nesting depth exceeded"
            };
        }

        const auto* obj = value.AsObject();
        if (!obj) {
            return SchemaValidationError{
                SchemaValidationCode::InvalidType,
                std::string(path),
                "Message value must be an object"
            };
        }

        if (!schema.allow_unknown_fields) {
            for (const auto& [key, val] : *obj) {
                (void)val;
                if (!schema.FindByName(key)) {
                    return SchemaValidationError{
                        SchemaValidationCode::UnknownField,
                        JoinPath(path, key),
                        "Unknown field is not permitted by schema"
                    };
                }
            }
        }

        for (const auto& field : schema.fields) {
            const std::string fieldPath = JoinPath(path, field.name);
            const Value* entry = value.Find(field.name);

            if (!entry || entry->IsNull()) {
                if (field.required) {
                    return SchemaValidationError{
                        SchemaValidationCode::MissingRequiredField,
                        fieldPath,
                        "Missing required field"
                    };
                }
                continue;
            }

            if (field.repeated) {
                const auto* arr = entry->AsArray();
                if (!arr) {
                    return SchemaValidationError{
                        SchemaValidationCode::InvalidType,
                        fieldPath,
                        "Repeated field must be an array"
                    };
                }

                if (field.min_length && arr->size() < *field.min_length) {
                    return SchemaValidationError{
                        SchemaValidationCode::LengthConstraint,
                        fieldPath,
                        "Array length below minimum"
                    };
                }
                if (field.max_length && arr->size() > *field.max_length) {
                    return SchemaValidationError{
                        SchemaValidationCode::LengthConstraint,
                        fieldPath,
                        "Array length exceeds maximum"
                    };
                }

                for (std::size_t i = 0; i < arr->size(); ++i) {
                    const std::string elemPath =
                        fieldPath + "[" + std::to_string(i) + "]";
                    if (auto err = ValidateElement(
                            field,
                            (*arr)[i],
                            elemPath,
                            depth,
                            maxDepth,
                            false
                        )) {
                        return err;
                    }
                }
            } else {
                if (auto err = ValidateElement(
                        field,
                        *entry,
                        fieldPath,
                        depth,
                        maxDepth,
                        true
                    )) {
                    return err;
                }
            }
        }

        return std::nullopt;
    }

    [[nodiscard]]
    std::optional<SchemaValidationError> ValidateElement(
        const FieldDescriptor& field,
        const Value& val,
        const std::string& path,
        std::size_t depth,
        std::size_t maxDepth,
        bool checkLengthConstraints
    ) const {
        switch (field.type) {
            case FieldType::Bool: {
                if (!val.IsBool()) {
                    return SchemaValidationError{
                        SchemaValidationCode::InvalidType,
                        path,
                        "Expected boolean value"
                    };
                }
                return std::nullopt;
            }

            case FieldType::Int64:
            case FieldType::SInt64: {
                const auto num = val.AsInt64();
                if (!num) {
                    return SchemaValidationError{
                        SchemaValidationCode::InvalidType,
                        path,
                        "Expected signed 64-bit integer"
                    };
                }
                return CheckNumericBounds(
                    field,
                    static_cast<double>(*num),
                    path
                );
            }

            case FieldType::UInt64: {
                const auto num = val.AsUInt64();
                if (!num) {
                    return SchemaValidationError{
                        SchemaValidationCode::InvalidType,
                        path,
                        "Expected unsigned 64-bit integer"
                    };
                }
                return CheckNumericBounds(
                    field,
                    static_cast<double>(*num),
                    path
                );
            }

            case FieldType::Double: {
                const auto num = val.AsDouble();
                if (!num || !std::isfinite(*num)) {
                    return SchemaValidationError{
                        SchemaValidationCode::InvalidType,
                        path,
                        "Expected finite floating-point number"
                    };
                }
                return CheckNumericBounds(field, *num, path);
            }

            case FieldType::String: {
                const auto str = val.AsString();
                if (!str) {
                    return SchemaValidationError{
                        SchemaValidationCode::InvalidType,
                        path,
                        "Expected string value"
                    };
                }
                if (checkLengthConstraints) {
                    if (field.min_length &&
                        str->size() < *field.min_length) {
                        return SchemaValidationError{
                            SchemaValidationCode::LengthConstraint,
                            path,
                            "String length below minimum"
                        };
                    }
                    if (field.max_length &&
                        str->size() > *field.max_length) {
                        return SchemaValidationError{
                            SchemaValidationCode::LengthConstraint,
                            path,
                            "String length exceeds maximum"
                        };
                    }
                }
                return std::nullopt;
            }

            case FieldType::Bytes: {
                const auto* bytes = val.AsBytes();
                if (!bytes) {
                    return SchemaValidationError{
                        SchemaValidationCode::InvalidType,
                        path,
                        "Expected binary bytes value"
                    };
                }
                if (checkLengthConstraints) {
                    if (field.min_length &&
                        bytes->size() < *field.min_length) {
                        return SchemaValidationError{
                            SchemaValidationCode::LengthConstraint,
                            path,
                            "Bytes length below minimum"
                        };
                    }
                    if (field.max_length &&
                        bytes->size() > *field.max_length) {
                        return SchemaValidationError{
                            SchemaValidationCode::LengthConstraint,
                            path,
                            "Bytes length exceeds maximum"
                        };
                    }
                }
                return std::nullopt;
            }

            case FieldType::Message: {
                const auto nested = Find(field.nested_schema);
                if (!nested) {
                    return SchemaValidationError{
                        SchemaValidationCode::SchemaNotFound,
                        path,
                        "Nested schema not found: " +
                            field.nested_schema.name
                    };
                }
                return ValidateMessage(
                    *nested,
                    val,
                    path,
                    depth + 1,
                    maxDepth
                );
            }
        }

        return SchemaValidationError{
            SchemaValidationCode::InvalidType,
            path,
            "Unsupported field type"
        };
    }

    [[nodiscard]]
    static std::optional<SchemaValidationError> CheckNumericBounds(
        const FieldDescriptor& field,
        double value,
        const std::string& path
    ) {
        if (field.min_value && value < *field.min_value) {
            return SchemaValidationError{
                SchemaValidationCode::RangeConstraint,
                path,
                "Numeric value below minimum"
            };
        }
        if (field.max_value && value > *field.max_value) {
            return SchemaValidationError{
                SchemaValidationCode::RangeConstraint,
                path,
                "Numeric value exceeds maximum"
            };
        }
        return std::nullopt;
    }

    mutable std::shared_mutex mutex_;
    std::unordered_map<
        SchemaKey,
        std::shared_ptr<const MessageSchema>,
        SchemaKeyHash
    > schemas_;
};

} // namespace lamp::serialization
