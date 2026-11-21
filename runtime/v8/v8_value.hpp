#pragma once

#include <v8.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace lamp::v8 {

// ============================================================
// Value Conversion Errors
// ============================================================

class V8ValueError final : public std::runtime_error {
public:
    explicit V8ValueError(std::string message)
        : std::runtime_error(std::move(message)) {}
};

// ============================================================
// Native Value Types
// ============================================================

struct V8Undefined final {};
struct V8Null final {};

struct V8Value;
using V8ValuePtr = std::shared_ptr<V8Value>;

using V8ValueArray = std::vector<V8ValuePtr>;

using V8ValueObject =
    std::map<std::string, V8ValuePtr>;

// ============================================================
// Native Value Representation
// ============================================================
//
// Shared pointers allow recursive values to be represented
// without requiring a complete V8Value definition inside
// std::vector and std::map.
//
// The conversion functions always construct independent
// value trees and reject circular JavaScript references.
//
// ============================================================

struct V8Value final {

    using Storage = std::variant<
        V8Undefined,
        V8Null,
        bool,
        double,
        std::string,
        V8ValueArray,
        V8ValueObject
    >;

    Storage data;

    V8Value()
        : data(V8Undefined{}) {}

    explicit V8Value(Storage value)
        : data(std::move(value)) {}

    explicit V8Value(bool value)
        : data(value) {}

    explicit V8Value(double value)
        : data(value) {}

    explicit V8Value(std::string value)
        : data(std::move(value)) {}

    explicit V8Value(V8ValueArray value)
        : data(std::move(value)) {}

    explicit V8Value(V8ValueObject value)
        : data(std::move(value)) {}

    [[nodiscard]]
    bool isUndefined() const noexcept {
        return std::holds_alternative<V8Undefined>(data);
    }

    [[nodiscard]]
    bool isNull() const noexcept {
        return std::holds_alternative<V8Null>(data);
    }

    [[nodiscard]]
    bool isBoolean() const noexcept {
        return std::holds_alternative<bool>(data);
    }

    [[nodiscard]]
    bool isNumber() const noexcept {
        return std::holds_alternative<double>(data);
    }

    [[nodiscard]]
    bool isString() const noexcept {
        return std::holds_alternative<std::string>(data);
    }

    [[nodiscard]]
    bool isArray() const noexcept {
        return std::holds_alternative<V8ValueArray>(data);
    }

    [[nodiscard]]
    bool isObject() const noexcept {
        return std::holds_alternative<V8ValueObject>(data);
    }
};

// ============================================================
// Conversion Configuration
// ============================================================

struct V8ValueConfig final {

    // Maximum recursive nesting depth.
    std::size_t maxDepth = 64;

    // Maximum number of array elements or object properties
    // in any single container.
    std::size_t maxContainerEntries = 100000;

    // Maximum UTF-8 bytes per string.
    std::size_t maxStringBytes = 16 * 1024 * 1024;

    // Maximum total nodes converted in one operation.
    std::size_t maxNodes = 1000000;
};

// ============================================================
// V8 Value Converter
// ============================================================
//
// Responsibilities:
//
// - Convert JavaScript primitives into C++ values.
// - Convert JavaScript arrays and plain objects.
// - Convert native values back into JavaScript.
// - Enforce recursion and container limits.
// - Reject unsupported JavaScript types.
// - Detect circular references.
//
// Important:
//
// The caller must already have entered the isolate and
// established valid V8 handle and context scopes.
//
// The converter does not own an isolate or context.
//
// ============================================================

class V8ValueConverter final {
public:

    explicit V8ValueConverter(
        V8ValueConfig config = {}
    )
        : config_(std::move(config)) {

        if (config_.maxDepth == 0 ||
            config_.maxNodes == 0) {

            throw V8ValueError(
                "Conversion depth and node limits "
                "must be greater than zero"
            );
        }
    }

    // ========================================================
    // JavaScript -> C++
    // ========================================================

    [[nodiscard]]
    V8Value fromJavaScript(
        ::v8::Isolate* isolate,
        ::v8::Local<::v8::Context> context,
        ::v8::Local<::v8::Value> value
    ) const {
        validateContext(isolate, context);

        ConversionState state;

        return fromJavaScriptImpl(
            isolate,
            context,
            value,
            state,
            0
        );
    }

    // ========================================================
    // C++ -> JavaScript
    // ========================================================

    [[nodiscard]]
    ::v8::Local<::v8::Value> toJavaScript(
        ::v8::Isolate* isolate,
        ::v8::Local<::v8::Context> context,
        const V8Value& value
    ) const {
        validateContext(isolate, context);

        ConversionState state;

        return toJavaScriptImpl(
            isolate,
            context,
            value,
            state,
            0
        );
    }

    [[nodiscard]]
    const V8ValueConfig& config() const noexcept {
        return config_;
    }

private:

    // ========================================================
    // Conversion State
    // ========================================================

    struct ConversionState final {
        std::size_t nodes = 0;

        // Active objects on the current recursion path.
        //
        // This distinguishes circular references from
        // repeated references to non-circular objects.

        std::vector<::v8::Local<::v8::Object>>
            activeJavaScriptObjects;

        std::unordered_set<const V8Value*>
            activeNativeValues;
    };

    // ========================================================
    // Context Validation
    // ========================================================

    static void validateContext(
        ::v8::Isolate* isolate,
        ::v8::Local<::v8::Context> context
    ) {
        if (isolate == nullptr || context.IsEmpty()) {
            throw V8ValueError(
                "A valid V8 isolate and context are required"
            );
        }

        // if (context->GetIsolate() != isolate) {
        //     throw V8ValueError(
        //         "V8 context belongs to a different isolate"
        //     );
        // }
    }

    // ========================================================
    // Limit Enforcement
    // ========================================================

    void checkLimits(
        ConversionState& state,
        std::size_t depth
    ) const {
        if (depth > config_.maxDepth) {
            throw V8ValueError(
                "Maximum conversion depth exceeded"
            );
        }

        if (state.nodes >= config_.maxNodes) {
            throw V8ValueError(
                "Maximum conversion node count exceeded"
            );
        }

        ++state.nodes;
    }

    void checkContainerSize(
        std::size_t size
    ) const {
        if (size > config_.maxContainerEntries) {
            throw V8ValueError(
                "Maximum container entry count exceeded"
            );
        }
    }

    // ========================================================
    // UTF-8 Conversion
    // ========================================================

    [[nodiscard]]
    std::string readString(
        ::v8::Isolate* isolate,
        ::v8::Local<::v8::String> value
    ) const {
        const std::size_t length = value->Utf8Length(isolate);

        if (length < 0 ||
            static_cast<std::size_t>(length) >
                config_.maxStringBytes) {

            throw V8ValueError(
                "JavaScript string exceeds conversion limit"
            );
        }

        ::v8::String::Utf8Value utf8(isolate, value);

        if (*utf8 == nullptr) {
            throw V8ValueError(
                "Failed to convert JavaScript string to UTF-8"
            );
        }

        return std::string(
            *utf8,
            static_cast<std::size_t>(utf8.length())
        );
    }

    [[nodiscard]]
    ::v8::Local<::v8::String> makeString(
        ::v8::Isolate* isolate,
        std::string_view value
    ) const {
        if (value.size() > config_.maxStringBytes ||
            value.size() >
                static_cast<std::size_t>(
                    std::numeric_limits<int>::max()
                )) {

            throw V8ValueError(
                "Native string exceeds conversion limit"
            );
        }

        ::v8::Local<::v8::String> result;

        if (!::v8::String::NewFromUtf8(
                isolate,
                value.data(),
                ::v8::NewStringType::kNormal,
                static_cast<int>(value.size())
            ).ToLocal(&result)) {

            throw V8ValueError(
                "Failed to create JavaScript string"
            );
        }

        return result;
    }

    // ========================================================
    // JavaScript -> Native Implementation
    // ========================================================

    [[nodiscard]]
    V8Value fromJavaScriptImpl(
        ::v8::Isolate* isolate,
        ::v8::Local<::v8::Context> context,
        ::v8::Local<::v8::Value> value,
        ConversionState& state,
        std::size_t depth
    ) const {
        checkLimits(state, depth);

        if (value->IsUndefined()) {
            return V8Value{};
        }

        if (value->IsNull()) {
            return V8Value(
                V8Value::Storage(V8Null{})
            );
        }

        if (value->IsBoolean()) {
            return V8Value(value->BooleanValue(isolate));
        }

        if (value->IsNumber()) {
            return V8Value(
                value.As<::v8::Number>()->Value()
            );
        }

        if (value->IsString()) {
            return V8Value(
                readString(
                    isolate,
                    value.As<::v8::String>()
                )
            );
        }

        // Reject unsupported values rather than
        // silently coercing them.

        if (value->IsFunction() ||
            value->IsSymbol() ||
            value->IsBigInt() ||
            value->IsPromise() ||
            value->IsExternal() ||
            !value->IsObject()) {

            throw V8ValueError(
                "Unsupported JavaScript value type"
            );
        }

        auto object = value.As<::v8::Object>();

        // Reject cycles on the active recursion path.

        for (const auto& active :
             state.activeJavaScriptObjects) {

            if (active == object) {
                throw V8ValueError(
                    "Circular JavaScript reference detected"
                );
            }
        }

        state.activeJavaScriptObjects.push_back(object);

        // ----------------------------------------------------
        // Arrays
        // ----------------------------------------------------

        if (value->IsArray()) {
            auto array = value.As<::v8::Array>();

            const auto length = array->Length();

            checkContainerSize(length);

            V8ValueArray result;
            result.reserve(length);

            for (std::uint32_t i = 0; i < length; ++i) {
                ::v8::Local<::v8::Value> element;

                if (!array->Get(context, i).ToLocal(&element)) {
                    throw V8ValueError(
                        "Failed to read JavaScript array element"
                    );
                }

                result.push_back(
                    std::make_shared<V8Value>(
                        fromJavaScriptImpl(
                            isolate,
                            context,
                            element,
                            state,
                            depth + 1
                        )
                    )
                );
            }

            state.activeJavaScriptObjects.pop_back();

            return V8Value(std::move(result));
        }

        // ----------------------------------------------------
        // Plain Objects
        // ----------------------------------------------------
        //
        // Only objects with Object.prototype or a null
        // prototype are accepted.
        //
        // This prevents accidental conversion of Dates,
        // Maps, Sets, typed arrays, and other special
        // JavaScript objects.
        //
        // Note: Accessors and proxies may still execute
        // JavaScript during property enumeration or reads.
        //
        // ----------------------------------------------------

        if (value->IsDate() ||
            value->IsMap() ||
            value->IsSet() ||
            value->IsRegExp() ||
            value->IsArrayBuffer() ||
            value->IsArrayBufferView() ||
            value->IsNativeError()) {

            throw V8ValueError(
                "Unsupported JavaScript object type"
            );
        }

        auto prototype = object->GetPrototype();

        if (!prototype->IsNull()) {
            auto objectConstructor =
                context->Global()
                    ->Get(context, makeString(isolate, "Object"));

            ::v8::Local<::v8::Value> constructorValue;

            if (!objectConstructor.ToLocal(&constructorValue) ||
                !constructorValue->IsFunction()) {

                throw V8ValueError(
                    "Unable to validate JavaScript object prototype"
                );
            }

            auto constructor =
                constructorValue.As<::v8::Function>();

            ::v8::Local<::v8::Value> objectPrototype;

            if (!constructor
                    ->Get(context, makeString(isolate, "prototype"))
                    .ToLocal(&objectPrototype)) {

                throw V8ValueError(
                    "Unable to read Object.prototype"
                );
            }

            if (!prototype->StrictEquals(objectPrototype)) {
                throw V8ValueError(
                    "Only plain JavaScript objects are supported"
                );
            }
        }

        ::v8::Local<::v8::Array> keys;

        if (!object->GetOwnPropertyNames(context)
                 .ToLocal(&keys)) {

            throw V8ValueError(
                "Failed to enumerate JavaScript object properties"
            );
        }

        checkContainerSize(keys->Length());

        V8ValueObject result;

        for (std::uint32_t i = 0; i < keys->Length(); ++i) {
            ::v8::Local<::v8::Value> key;

            if (!keys->Get(context, i).ToLocal(&key) ||
                !key->IsString()) {

                throw V8ValueError(
                    "Invalid JavaScript object property key"
                );
            }

            auto keyString = key.As<::v8::String>();

            ::v8::Local<::v8::Value> property;

            if (!object->Get(context, keyString)
                     .ToLocal(&property)) {

                throw V8ValueError(
                    "Failed to read JavaScript object property"
                );
            }

            result.emplace(
                readString(isolate, keyString),
                std::make_shared<V8Value>(
                    fromJavaScriptImpl(
                        isolate,
                        context,
                        property,
                        state,
                        depth + 1
                    )
                )
            );
        }

        state.activeJavaScriptObjects.pop_back();

        return V8Value(std::move(result));
    }

    // ========================================================
    // Native -> JavaScript Implementation
    // ========================================================

    [[nodiscard]]
    ::v8::Local<::v8::Value> toJavaScriptImpl(
        ::v8::Isolate* isolate,
        ::v8::Local<::v8::Context> context,
        const V8Value& value,
        ConversionState& state,
        std::size_t depth
    ) const {
        checkLimits(state, depth);

        if (std::holds_alternative<V8Undefined>(value.data)) {
            return ::v8::Undefined(isolate);
        }

        if (std::holds_alternative<V8Null>(value.data)) {
            return ::v8::Null(isolate);
        }

        if (const auto* boolean =
                std::get_if<bool>(&value.data)) {

            return ::v8::Boolean::New(isolate, *boolean);
        }

        if (const auto* number =
                std::get_if<double>(&value.data)) {

            return ::v8::Number::New(isolate, *number);
        }

        if (const auto* string =
                std::get_if<std::string>(&value.data)) {

            return makeString(isolate, *string);
        }

        if (!state.activeNativeValues.insert(&value).second) {
            throw V8ValueError(
                "Circular native value reference detected"
            );
        }

        // ----------------------------------------------------
        // Native Arrays
        // ----------------------------------------------------

        if (const auto* array =
                std::get_if<V8ValueArray>(&value.data)) {

            checkContainerSize(array->size());

            auto result = ::v8::Array::New(
                isolate,
                static_cast<int>(array->size())
            );

            for (std::size_t i = 0; i < array->size(); ++i) {
                if (!(*array)[i]) {
                    throw V8ValueError(
                        "Null pointer in native value array"
                    );
                }

                auto element = toJavaScriptImpl(
                    isolate,
                    context,
                    *(*array)[i],
                    state,
                    depth + 1
                );

                if (!result->Set(
                        context,
                        static_cast<std::uint32_t>(i),
                        element
                    ).FromMaybe(false)) {

                    throw V8ValueError(
                        "Failed to set JavaScript array element"
                    );
                }
            }

            state.activeNativeValues.erase(&value);

            return result;
        }

        // ----------------------------------------------------
        // Native Objects
        // ----------------------------------------------------

        const auto* object =
            std::get_if<V8ValueObject>(&value.data);

        if (!object) {
            throw V8ValueError(
                "Unknown native value representation"
            );
        }

        checkContainerSize(object->size());

        auto result = ::v8::Object::New(isolate);

        for (const auto& [key, child] : *object) {
            if (!child) {
                throw V8ValueError(
                    "Null pointer in native value object"
                );
            }

            auto property = toJavaScriptImpl(
                isolate,
                context,
                *child,
                state,
                depth + 1
            );

            if (!result->CreateDataProperty(
                    context,
                    makeString(isolate, key),
                    property
                ).FromMaybe(false)) {

                throw V8ValueError(
                    "Failed to set JavaScript object property"
                );
            }
        }

        state.activeNativeValues.erase(&value);

        return result;
    }

    V8ValueConfig config_;
};

} // namespace lamp::v8