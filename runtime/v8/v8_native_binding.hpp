#pragma once

#include "v8_context.hpp"
#include "v8_value.hpp"

#include <v8.h>

#include <cstddef>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>
#include <unordered_set>

namespace lamp::v8 {

// ============================================================
// Native Binding Errors
// ============================================================

class V8NativeBindingError final : public std::runtime_error {
public:
    explicit V8NativeBindingError(std::string message)
        : std::runtime_error(std::move(message)) {}
};

// ============================================================
// Native Function Types
// ============================================================
//
// Native handlers receive converted JavaScript arguments
// and return a native V8Value.
//
// JavaScript exceptions are generated automatically when
// argument conversion or native execution fails.
//
// ============================================================

using V8NativeArguments = std::vector<V8Value>;

using V8NativeHandler = std::function<
    V8Value(const V8NativeArguments&)
>;

// ============================================================
// Native Binding Configuration
// ============================================================

struct V8NativeBindingConfig final {

    // Maximum arguments accepted by one native invocation.
    std::size_t maxArguments = 64;

    // Value conversion limits.
    V8ValueConfig valueConfig{};

    // Prefix used for JavaScript-facing native errors.
    std::string errorPrefix = "Lamp native binding";
};

// ============================================================
// Native Binding Registry
// ============================================================
//
// Responsibilities:
//
// - Register native C++ handlers.
// - Expose handlers as JavaScript global functions.
// - Convert JavaScript arguments to native values.
// - Convert native return values to JavaScript.
// - Translate C++ exceptions into JavaScript exceptions.
// - Support thread-safe handler registration and lookup.
//
// Lifetime:
//
// This registry must outlive every JavaScript function
// installed through it.
//
// The associated V8Context and V8Isolate must also remain
// valid during binding installation and invocation.
//
// Threading:
//
// Registration and lookup are synchronized.
//
// V8 operations are performed only while the associated
// isolate is entered through V8Context::Scope.
//
// ============================================================

class V8NativeBinding final {
public:

    // ========================================================
    // Construction
    // ========================================================

    explicit V8NativeBinding(
        V8Context& context,
        V8NativeBindingConfig config = {}
    )
        : context_(&context),
          config_(std::move(config)),
          converter_(config_.valueConfig) {

        if (config_.maxArguments == 0) {
            throw V8NativeBindingError(
                "Maximum native argument count "
                "must be greater than zero"
            );
        }

        if (!context_->isValid()) {
            throw V8NativeBindingError(
                "Cannot create bindings for an invalid context"
            );
        }
    }

    // ========================================================
    // Destruction
    // ========================================================

    ~V8NativeBinding() noexcept {
        dispose();
    }

    // ========================================================
    // Non-Copyable / Non-Movable
    // ========================================================

    V8NativeBinding(const V8NativeBinding&) = delete;
    V8NativeBinding& operator=(const V8NativeBinding&) = delete;

    V8NativeBinding(V8NativeBinding&&) = delete;
    V8NativeBinding& operator=(V8NativeBinding&&) = delete;

    // ========================================================
    // Register Native Function
    // ========================================================
    //
    // Registering a handler does not automatically expose
    // it to JavaScript.
    //
    // Call install() to create the JavaScript function.
    //
    // ========================================================

    void registerFunction(
        std::string name,
        V8NativeHandler handler
    ) {
        if (name.empty()) {
            throw V8NativeBindingError(
                "Native function name cannot be empty"
            );
        }

        if (!handler) {
            throw V8NativeBindingError(
                "Native function handler cannot be empty"
            );
        }

        std::lock_guard<std::mutex> lock(mutex_);

        if (disposed_) {
            throw V8NativeBindingError(
                "Cannot register functions after disposal"
            );
        }

        if (handlers_.contains(name)) {
            throw V8NativeBindingError(
                "Duplicate native function registration: " +
                name
            );
        }

        handlers_.emplace(
            std::move(name),
            std::move(handler)
        );
    }

    // ========================================================
    // Install One Function
    // ========================================================
    //
    // Creates a JavaScript function in the context's
    // global object.
    //
    // A small External callback payload points to the
    // registry and the registered function name.
    //
    // Callback payloads are owned by this registry.
    //
    // ========================================================

    void install(std::string_view name) {
        std::string functionName(name);

        {
            std::lock_guard<std::mutex> lock(mutex_);

            if (disposed_) {
                throw V8NativeBindingError(
                    "Cannot install functions after disposal"
                );
            }

            if (!handlers_.contains(functionName)) {
                throw V8NativeBindingError(
                    "Native function is not registered: " +
                    functionName
                );
            }

            if (installed_.contains(functionName)) {
                throw V8NativeBindingError(
                    "Native function is already installed: " +
                    functionName
                );
            }

            // Reserve the name before interacting with V8.
            // This prevents concurrent duplicate installation.
            installed_.emplace(functionName);
        }

        try {
            V8Context::Scope scope(*context_);

            auto* isolate = scope.isolate();
            auto context = scope.context();

            // Ensure V8 callback data remains valid.
            auto payload = std::make_unique<CallbackPayload>();

            payload->registry = this;
            payload->name = functionName;

            auto* rawPayload = payload.get();

            // Store payload ownership before publishing
            // the JavaScript function.
            {
                std::lock_guard<std::mutex> lock(mutex_);
                payloads_.push_back(std::move(payload));
            }

            auto data = ::v8::External::New(
                isolate,
                rawPayload,
                ::v8::kExternalPointerTypeTagDefault
            );

            auto functionTemplate =
                ::v8::FunctionTemplate::New(
                    isolate,
                    &V8NativeBinding::dispatchCallback,
                    data
                );

            ::v8::Local<::v8::Function> function;

            if (!functionTemplate
                    ->GetFunction(context)
                    .ToLocal(&function)) {

                throw V8NativeBindingError(
                    "Failed to create native JavaScript function: " +
                    functionName
                );
            }

            auto key = makeString(
                isolate,
                functionName
            );

            // Refuse to overwrite existing globals.
            const auto exists =
                context->Global()->Has(context, key);

            if (exists.IsNothing()) {
                throw V8NativeBindingError(
                    "Failed to inspect JavaScript global: " +
                    functionName
                );
            }

            if (exists.FromJust()) {
                throw V8NativeBindingError(
                    "JavaScript global already exists: " +
                    functionName
                );
            }

            if (!context->Global()
                     ->CreateDataProperty(
                         context,
                         key,
                         function
                     ).FromMaybe(false)) {

                throw V8NativeBindingError(
                    "Failed to install native function: " +
                    functionName
                );
            }

        } catch (...) {

            // Remove the installation reservation.
            //
            // Callback payloads remain owned until disposal
            // because a V8 function may have been created
            // before the installation failed.

            std::lock_guard<std::mutex> lock(mutex_);
            installed_.erase(functionName);

            throw;
        }
    }

    // ========================================================
    // Install All Registered Functions
    // ========================================================

    void installAll() {
        std::vector<std::string> names;

        {
            std::lock_guard<std::mutex> lock(mutex_);

            if (disposed_) {
                throw V8NativeBindingError(
                    "Cannot install functions after disposal"
                );
            }

            names.reserve(handlers_.size());

            for (const auto& [name, handler] : handlers_) {
                (void)handler;

                if (!installed_.contains(name)) {
                    names.push_back(name);
                }
            }
        }

        for (const auto& name : names) {
            install(name);
        }
    }

    // ========================================================
    // Inspection
    // ========================================================

    [[nodiscard]]
    bool contains(std::string_view name) const {
        std::lock_guard<std::mutex> lock(mutex_);

        return handlers_.contains(std::string(name));
    }

    [[nodiscard]]
    bool isInstalled(std::string_view name) const {
        std::lock_guard<std::mutex> lock(mutex_);

        return installed_.contains(std::string(name));
    }

    [[nodiscard]]
    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return handlers_.size();
    }

    // ========================================================
    // Explicit Disposal
    // ========================================================
    //
    // Disposal disables native handler execution.
    //
    // IMPORTANT:
    //
    // JavaScript functions created by this registry must
    // no longer be invoked after disposal.
    //
    // The context must be quiescent and must be destroyed
    // before the registry releases callback payloads.
    //
    // ========================================================

    void dispose() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);

        if (disposed_) {
            return;
        }

        disposed_ = true;

        handlers_.clear();
        installed_.clear();

        // Payloads remain allocated until the registry
        // itself is destroyed.
        //
        // This avoids immediate invalidation of callback
        // data during explicit disposal.
    }

private:

    // ========================================================
    // Callback Payload
    // ========================================================

    struct CallbackPayload final {
        V8NativeBinding* registry = nullptr;
        std::string name;
    };

    // ========================================================
    // UTF-8 String Creation
    // ========================================================

    [[nodiscard]]
    static ::v8::Local<::v8::String> makeString(
        ::v8::Isolate* isolate,
        std::string_view text
    ) {
        if (text.size() >
            static_cast<std::size_t>(
                std::numeric_limits<int>::max()
            )) {

            throw V8NativeBindingError(
                "Native function name exceeds V8 limits"
            );
        }

        ::v8::Local<::v8::String> result;

        if (!::v8::String::NewFromUtf8(
                isolate,
                text.data(),
                ::v8::NewStringType::kNormal,
                static_cast<int>(text.size())
            ).ToLocal(&result)) {

            throw V8NativeBindingError(
                "Failed to create V8 function name"
            );
        }

        return result;
    }

    // ========================================================
    // V8 Function Callback
    // ========================================================

    static void dispatchCallback(
        const ::v8::FunctionCallbackInfo<::v8::Value>& args
    ) {
        auto* isolate = args.GetIsolate();

        if (args.Data().IsEmpty()) {

            throwJavaScriptError(
                isolate,
                "Invalid native callback data"
            );

            return;
        }

        auto* payload = static_cast<CallbackPayload*>(
            args.Data()
                .As<::v8::External>()
                ->Value(::v8::kExternalPointerTypeTagDefault)
        );

        if (payload == nullptr ||
            payload->registry == nullptr) {

            throwJavaScriptError(
                isolate,
                "Native callback payload is unavailable"
            );

            return;
        }

        payload->registry->invoke(
            args,
            payload->name
        );
    }

    // ========================================================
    // Invoke Native Handler
    // ========================================================

    void invoke(
        const ::v8::FunctionCallbackInfo<::v8::Value>& args,
        const std::string& name
    ) noexcept {
        auto* isolate = args.GetIsolate();

        try {
            V8NativeHandler handler;

            {
                std::lock_guard<std::mutex> lock(mutex_);

                if (disposed_) {
                    throw V8NativeBindingError(
                        "Native binding registry is disposed"
                    );
                }

                const auto iterator = handlers_.find(name);

                if (iterator == handlers_.end()) {
                    throw V8NativeBindingError(
                        "Native function is no longer registered: " +
                        name
                    );
                }

                handler = iterator->second;
            }

            if (static_cast<std::size_t>(args.Length()) >
                config_.maxArguments) {

                throw V8NativeBindingError(
                    "Too many arguments for native function: " +
                    name
                );
            }

            auto context = isolate->GetCurrentContext();

            if (context.IsEmpty()) {
                throw V8NativeBindingError(
                    "Native function requires an active context"
                );
            }

            V8NativeArguments nativeArguments;

            nativeArguments.reserve(
                static_cast<std::size_t>(args.Length())
            );

            for (int i = 0; i < args.Length(); ++i) {
                nativeArguments.push_back(
                    converter_.fromJavaScript(
                        isolate,
                        context,
                        args[i]
                    )
                );
            }

            auto result = handler(nativeArguments);

            auto jsResult = converter_.toJavaScript(
                isolate,
                context,
                result
            );

            args.GetReturnValue().Set(jsResult);

        } catch (const std::exception& error) {

            throwJavaScriptError(
                isolate,
                config_.errorPrefix + ": " + error.what()
            );

        } catch (...) {

            throwJavaScriptError(
                isolate,
                config_.errorPrefix +
                    ": unknown native exception"
            );
        }
    }

    // ========================================================
    // Throw JavaScript Exception
    // ========================================================

    static void throwJavaScriptError(
        ::v8::Isolate* isolate,
        const std::string& message
    ) noexcept {
        // Do not allow C++ exceptions to escape through
        // the V8 C callback boundary.

        ::v8::Local<::v8::String> text;

        if (!::v8::String::NewFromUtf8(
                isolate,
                message.c_str(),
                ::v8::NewStringType::kNormal
            ).ToLocal(&text)) {

            isolate->ThrowException(
                ::v8::Undefined(isolate)
            );

            return;
        }

        isolate->ThrowException(
            ::v8::Exception::Error(text)
        );
    }

    // ========================================================
    // Internal State
    // ========================================================

    V8Context* context_ = nullptr;

    V8NativeBindingConfig config_{};

    V8ValueConverter converter_;

    mutable std::mutex mutex_;

    std::unordered_map<
        std::string,
        V8NativeHandler
    > handlers_;

    std::unordered_set<std::string> installed_;

    std::vector<
        std::unique_ptr<CallbackPayload>
    > payloads_;

    bool disposed_ = false;
};

} // namespace lamp::v8