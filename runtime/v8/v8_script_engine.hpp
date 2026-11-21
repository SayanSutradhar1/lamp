#pragma once

#include "v8_context.hpp"

#include <v8.h>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace lamp::v8 {

// ============================================================
// Script Execution Errors
// ============================================================

enum class V8ScriptErrorType {
    Compilation,
    Runtime,
    Conversion,
    InvalidState
};

class V8ScriptError final : public std::runtime_error {
public:
    V8ScriptError(
        V8ScriptErrorType type,
        std::string message,
        std::string resourceName = {},
        int line = 0,
        int column = 0,
        std::string stackTrace = {}
    )
        : std::runtime_error(std::move(message)),
          type_(type),
          resourceName_(std::move(resourceName)),
          line_(line),
          column_(column),
          stackTrace_(std::move(stackTrace)) {}

    [[nodiscard]]
    V8ScriptErrorType type() const noexcept {
        return type_;
    }

    [[nodiscard]]
    const std::string& resourceName() const noexcept {
        return resourceName_;
    }

    [[nodiscard]]
    int line() const noexcept {
        return line_;
    }

    [[nodiscard]]
    int column() const noexcept {
        return column_;
    }

    [[nodiscard]]
    const std::string& stackTrace() const noexcept {
        return stackTrace_;
    }

private:
    V8ScriptErrorType type_;
    std::string resourceName_;
    int line_;
    int column_;
    std::string stackTrace_;
};

// ============================================================
// Script Configuration
// ============================================================

struct V8ScriptConfig final {
    // Appears in V8 exception diagnostics.
    std::string resourceName = "<lamp-script>";

    // V8 script-origin line and column offsets.
    // Both are zero-based.
    int lineOffset = 0;
    int columnOffset = 0;
};

// ============================================================
// Script Result
// ============================================================
//
// Represents a converted JavaScript primitive.
//
// Objects, arrays, functions, symbols, and BigInt values
// are reported as unsupported rather than implicitly
// stringified or converted to lossy numeric values.
//
// ============================================================

enum class V8ScriptValueType {
    Undefined,
    Null,
    Boolean,
    Number,
    String,
    Unsupported
};

struct V8ScriptResult final {
    V8ScriptValueType type = V8ScriptValueType::Undefined;

    bool booleanValue = false;
    double numberValue = 0.0;
    std::string stringValue;

    [[nodiscard]]
    bool isUndefined() const noexcept {
        return type == V8ScriptValueType::Undefined;
    }

    [[nodiscard]]
    bool isNull() const noexcept {
        return type == V8ScriptValueType::Null;
    }

    [[nodiscard]]
    bool isBoolean() const noexcept {
        return type == V8ScriptValueType::Boolean;
    }

    [[nodiscard]]
    bool isNumber() const noexcept {
        return type == V8ScriptValueType::Number;
    }

    [[nodiscard]]
    bool isString() const noexcept {
        return type == V8ScriptValueType::String;
    }

    [[nodiscard]]
    bool isSupported() const noexcept {
        return type != V8ScriptValueType::Unsupported;
    }
};

// ============================================================
// V8 Script Engine
// ============================================================
//
// Responsibilities:
//
// - Compile classic JavaScript scripts.
// - Execute scripts inside an existing V8Context.
// - Capture compilation and runtime exceptions.
// - Extract source locations and stack traces.
// - Convert JavaScript primitive results into C++ values.
//
// Non-responsibilities:
//
// - TypeScript transpilation.
// - ES module loading.
// - Promise/event-loop scheduling.
// - Execution timeouts or CPU quotas.
// - Operating-system-level sandboxing.
//
// Threading:
//
// V8Context::Scope coordinates isolate entry through
// V8's Locker. The caller must still ensure that context
// disposal cannot overlap script execution.
//
// ============================================================

class V8ScriptEngine final {
public:

    explicit V8ScriptEngine(V8Context& context) noexcept
        : context_(&context) {}

    V8ScriptEngine(const V8ScriptEngine&) = delete;
    V8ScriptEngine& operator=(const V8ScriptEngine&) = delete;

    V8ScriptEngine(V8ScriptEngine&&) = delete;
    V8ScriptEngine& operator=(V8ScriptEngine&&) = delete;

    // ========================================================
    // Execute JavaScript
    // ========================================================

    [[nodiscard]]
    V8ScriptResult execute(
        std::string_view source,
        const V8ScriptConfig& config = {}
    ) {
        validateConfig(config);

        if (context_ == nullptr || !context_->isValid()) {
            throw V8ScriptError(
                V8ScriptErrorType::InvalidState,
                "Cannot execute JavaScript in an invalid V8 context"
            );
        }

        V8Context::Scope scope(*context_);

        auto* isolate = scope.isolate();
        auto localContext = scope.context();

        ::v8::TryCatch tryCatch(isolate);

        // ----------------------------------------------------
        // Convert JavaScript Source
        // ----------------------------------------------------

        auto sourceString = makeV8String(
            isolate,
            source,
            "JavaScript source"
        );

        auto resourceName = makeV8String(
            isolate,
            config.resourceName,
            "Script resource name"
        );

        // ----------------------------------------------------
        // Configure Script Origin
        // ----------------------------------------------------

        ::v8::ScriptOrigin origin(
            resourceName,
            config.lineOffset,
            config.columnOffset
        );

        // ----------------------------------------------------
        // Compile Script
        // ----------------------------------------------------

        ::v8::Local<::v8::Script> script;

        if (!::v8::Script::Compile(
                localContext,
                sourceString,
                &origin
            ).ToLocal(&script)) {

            throw makeScriptError(
                isolate,
                localContext,
                tryCatch,
                V8ScriptErrorType::Compilation,
                config.resourceName
            );
        }

        // ----------------------------------------------------
        // Execute Script
        // ----------------------------------------------------

        ::v8::Local<::v8::Value> value;

        if (!script->Run(localContext).ToLocal(&value)) {
            throw makeScriptError(
                isolate,
                localContext,
                tryCatch,
                V8ScriptErrorType::Runtime,
                config.resourceName
            );
        }

        // ----------------------------------------------------
        // Convert Result
        // ----------------------------------------------------

        return convertResult(
            isolate,
            value
        );
    }

    // ========================================================
    // Execute Without Returning a Value
    // ========================================================
    //
    // Useful for initialization scripts that install
    // globals, functions, or application state.
    //
    // Compilation and runtime errors still propagate.
    //
    // ========================================================

    void executeVoid(
        std::string_view source,
        const V8ScriptConfig& config = {}
    ) {
        (void)execute(source, config);
    }

private:

    // ========================================================
    // Validate Configuration
    // ========================================================

    static void validateConfig(
        const V8ScriptConfig& config
    ) {
        if (config.lineOffset < 0 ||
            config.columnOffset < 0) {

            throw V8ScriptError(
                V8ScriptErrorType::InvalidState,
                "Script line and column offsets cannot be negative"
            );
        }
    }

    // ========================================================
    // Create V8 UTF-8 String
    // ========================================================

    [[nodiscard]]
    static ::v8::Local<::v8::String> makeV8String(
        ::v8::Isolate* isolate,
        std::string_view value,
        std::string_view description
    ) {
        if (value.size() >
            static_cast<std::size_t>(
                std::numeric_limits<int>::max()
            )) {

            throw V8ScriptError(
                V8ScriptErrorType::Conversion,
                std::string(description) +
                    " exceeds V8 string input limits"
            );
        }

        ::v8::Local<::v8::String> result;

        if (!::v8::String::NewFromUtf8(
                isolate,
                value.data(),
                ::v8::NewStringType::kNormal,
                static_cast<int>(value.size())
            ).ToLocal(&result)) {

            throw V8ScriptError(
                V8ScriptErrorType::Conversion,
                "Failed to convert " +
                    std::string(description) +
                    " into a V8 string"
            );
        }

        return result;
    }

    // ========================================================
    // Convert JavaScript Result
    // ========================================================

    [[nodiscard]]
    static V8ScriptResult convertResult(
        ::v8::Isolate* isolate,
        ::v8::Local<::v8::Value> value
    ) {
        V8ScriptResult result;

        if (value->IsUndefined()) {
            result.type = V8ScriptValueType::Undefined;
            return result;
        }

        if (value->IsNull()) {
            result.type = V8ScriptValueType::Null;
            return result;
        }

        if (value->IsBoolean()) {
            result.type = V8ScriptValueType::Boolean;
            result.booleanValue = value->BooleanValue(isolate);
            return result;
        }

        if (value->IsNumber()) {
            result.type = V8ScriptValueType::Number;
            result.numberValue = value.As<::v8::Number>()->Value();
            return result;
        }

        if (value->IsString()) {
            result.type = V8ScriptValueType::String;

            ::v8::String::Utf8Value utf8(
                isolate,
                value
            );

            if (*utf8 == nullptr) {
                throw V8ScriptError(
                    V8ScriptErrorType::Conversion,
                    "Failed to convert JavaScript string to UTF-8"
                );
            }

            result.stringValue.assign(
                *utf8,
                static_cast<std::size_t>(utf8.length())
            );

            return result;
        }

        result.type = V8ScriptValueType::Unsupported;
        return result;
    }

    // ========================================================
    // Extract Exception Information
    // ========================================================

    [[nodiscard]]
    static V8ScriptError makeScriptError(
        ::v8::Isolate* isolate,
        ::v8::Local<::v8::Context> context,
        ::v8::TryCatch& tryCatch,
        V8ScriptErrorType type,
        const std::string& fallbackResourceName
    ) {
        std::string message = "Unknown JavaScript exception";
        std::string resourceName = fallbackResourceName;
        std::string stackTrace;

        int line = 0;
        int column = 0;

        // ----------------------------------------------------
        // Exception Message
        // ----------------------------------------------------

        if (!tryCatch.Exception().IsEmpty()) {
            ::v8::String::Utf8Value exception(
                isolate,
                tryCatch.Exception()
            );

            if (*exception != nullptr) {
                message.assign(
                    *exception,
                    static_cast<std::size_t>(
                        exception.length()
                    )
                );
            }
        }

        // ----------------------------------------------------
        // Source Location
        // ----------------------------------------------------

        auto diagnostic = tryCatch.Message();

        if (!diagnostic.IsEmpty()) {

            auto scriptName = diagnostic->GetScriptResourceName();

            if (!scriptName.IsEmpty() &&
                scriptName->IsString()) {

                ::v8::String::Utf8Value name(
                    isolate,
                    scriptName
                );

                if (*name != nullptr) {
                    resourceName.assign(
                        *name,
                        static_cast<std::size_t>(
                            name.length()
                        )
                    );
                }
            }

            const auto maybeLine =
                diagnostic->GetLineNumber(context);

            if (!maybeLine.IsNothing()) {
                line = maybeLine.FromJust();
            }

            // V8 columns are zero-based; diagnostics
            // exposed by this class use one-based columns.

            const auto maybeColumn =
                diagnostic->GetStartColumn(context);

            if (!maybeColumn.IsNothing()) {
                column = maybeColumn.FromJust() + 1;
            }
        }

        // ----------------------------------------------------
        // Stack Trace
        // ----------------------------------------------------

        ::v8::Local<::v8::Value> stack;

        if (tryCatch.StackTrace(context).ToLocal(&stack) &&
            stack->IsString()) {

            ::v8::String::Utf8Value utf8(
                isolate,
                stack
            );

            if (*utf8 != nullptr) {
                stackTrace.assign(
                    *utf8,
                    static_cast<std::size_t>(
                        utf8.length()
                    )
                );
            }
        }

        return V8ScriptError(
            type,
            std::move(message),
            std::move(resourceName),
            line,
            column,
            std::move(stackTrace)
        );
    }

    // ========================================================
    // Internal State
    // ========================================================

    // Non-owning pointer.
    //
    // The V8Context must outlive this script engine.

    V8Context* context_ = nullptr;
};

} // namespace lamp::v8