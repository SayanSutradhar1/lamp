#pragma once

// ============================================================
// Lamp Framework
// Phase 7 - Serialization
//
// File: runtime/serialization/serialization_value.hpp
//
// Purpose:
//   Transport-neutral, binary-safe structured value model
//   shared by Protobuf wire codecs, JSON fallback codecs,
//   and schema validation.
// ============================================================

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace lamp::serialization {

enum class ValueKind : std::uint8_t {
    Null = 0,
    Bool = 1,
    Int64 = 2,
    UInt64 = 3,
    Double = 4,
    String = 5,
    Bytes = 6,
    Array = 7,
    Object = 8
};

class Value final {
public:
    using Bytes = std::vector<std::uint8_t>;
    using Array = std::vector<Value>;
    using ObjectEntry = std::pair<std::string, Value>;
    using Object = std::vector<ObjectEntry>;

    using Storage = std::variant<
        std::monostate,
        bool,
        std::int64_t,
        std::uint64_t,
        double,
        std::string,
        Bytes,
        Array,
        Object
    >;

    Value() = default;
    Value(std::nullptr_t) noexcept : storage_(std::monostate{}) {}
    Value(bool v) noexcept : storage_(v) {}
    Value(int v) noexcept : storage_(static_cast<std::int64_t>(v)) {}
    Value(std::int64_t v) noexcept : storage_(v) {}
    Value(std::uint64_t v) noexcept : storage_(v) {}
    Value(double v) noexcept : storage_(v) {}
    Value(const char* v) : storage_(std::string(v ? v : "")) {}
    Value(std::string v) : storage_(std::move(v)) {}
    Value(std::string_view v) : storage_(std::string(v)) {}
    Value(Bytes v) : storage_(std::move(v)) {}
    Value(Array v) : storage_(std::move(v)) {}
    Value(Object v) : storage_(std::move(v)) {}

    [[nodiscard]]
    static Value MakeNull() noexcept {
        return Value{};
    }

    [[nodiscard]]
    static Value MakeArray(Array items = {}) {
        return Value(std::move(items));
    }

    [[nodiscard]]
    static Value MakeObject(Object entries = {}) {
        return Value(std::move(entries));
    }

    [[nodiscard]]
    ValueKind Kind() const noexcept {
        return static_cast<ValueKind>(storage_.index());
    }

    [[nodiscard]] bool IsNull() const noexcept {
        return std::holds_alternative<std::monostate>(storage_);
    }

    [[nodiscard]] bool IsBool() const noexcept {
        return std::holds_alternative<bool>(storage_);
    }

    [[nodiscard]] bool IsInt64() const noexcept {
        return std::holds_alternative<std::int64_t>(storage_);
    }

    [[nodiscard]] bool IsUInt64() const noexcept {
        return std::holds_alternative<std::uint64_t>(storage_);
    }

    [[nodiscard]] bool IsDouble() const noexcept {
        return std::holds_alternative<double>(storage_);
    }

    [[nodiscard]] bool IsNumber() const noexcept {
        return IsInt64() || IsUInt64() || IsDouble();
    }

    [[nodiscard]] bool IsString() const noexcept {
        return std::holds_alternative<std::string>(storage_);
    }

    [[nodiscard]] bool IsBytes() const noexcept {
        return std::holds_alternative<Bytes>(storage_);
    }

    [[nodiscard]] bool IsArray() const noexcept {
        return std::holds_alternative<Array>(storage_);
    }

    [[nodiscard]] bool IsObject() const noexcept {
        return std::holds_alternative<Object>(storage_);
    }

    [[nodiscard]] std::optional<bool> AsBool() const noexcept {
        if (const auto* p = std::get_if<bool>(&storage_)) {
            return *p;
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::int64_t> AsInt64() const noexcept {
        if (const auto* p = std::get_if<std::int64_t>(&storage_)) {
            return *p;
        }
        if (const auto* p = std::get_if<std::uint64_t>(&storage_)) {
            if (*p <= static_cast<std::uint64_t>(
                          std::numeric_limits<std::int64_t>::max())) {
                return static_cast<std::int64_t>(*p);
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::uint64_t> AsUInt64() const noexcept {
        if (const auto* p = std::get_if<std::uint64_t>(&storage_)) {
            return *p;
        }
        if (const auto* p = std::get_if<std::int64_t>(&storage_)) {
            if (*p >= 0) {
                return static_cast<std::uint64_t>(*p);
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<double> AsDouble() const noexcept {
        if (const auto* p = std::get_if<double>(&storage_)) {
            return *p;
        }
        if (const auto* p = std::get_if<std::int64_t>(&storage_)) {
            return static_cast<double>(*p);
        }
        if (const auto* p = std::get_if<std::uint64_t>(&storage_)) {
            return static_cast<double>(*p);
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::string_view> AsString() const noexcept {
        if (const auto* p = std::get_if<std::string>(&storage_)) {
            return std::string_view(*p);
        }
        return std::nullopt;
    }

    [[nodiscard]] const Bytes* AsBytes() const noexcept {
        return std::get_if<Bytes>(&storage_);
    }

    [[nodiscard]] Bytes* AsBytesMutable() noexcept {
        return std::get_if<Bytes>(&storage_);
    }

    [[nodiscard]] const Array* AsArray() const noexcept {
        return std::get_if<Array>(&storage_);
    }

    [[nodiscard]] Array* AsArrayMutable() noexcept {
        return std::get_if<Array>(&storage_);
    }

    [[nodiscard]] const Object* AsObject() const noexcept {
        return std::get_if<Object>(&storage_);
    }

    [[nodiscard]] Object* AsObjectMutable() noexcept {
        return std::get_if<Object>(&storage_);
    }

    [[nodiscard]] const Value* Find(std::string_view key) const noexcept {
        const auto* obj = AsObject();
        if (!obj) {
            return nullptr;
        }
        for (const auto& [k, v] : *obj) {
            if (k == key) {
                return &v;
            }
        }
        return nullptr;
    }

    [[nodiscard]] Value* FindMutable(std::string_view key) noexcept {
        auto* obj = AsObjectMutable();
        if (!obj) {
            return nullptr;
        }
        for (auto& [k, v] : *obj) {
            if (k == key) {
                return &v;
            }
        }
        return nullptr;
    }

    void Set(std::string key, Value val) {
        if (!IsObject()) {
            storage_ = Object{};
        }
        auto& obj = std::get<Object>(storage_);
        for (auto& [k, existing] : obj) {
            if (k == key) {
                existing = std::move(val);
                return;
            }
        }
        obj.emplace_back(std::move(key), std::move(val));
    }

    void PushBack(Value val) {
        if (!IsArray()) {
            storage_ = Array{};
        }
        std::get<Array>(storage_).push_back(std::move(val));
    }

    [[nodiscard]]
    friend bool operator==(const Value& a, const Value& b) noexcept = default;

private:
    Storage storage_{std::monostate{}};
};

} // namespace lamp::serialization
