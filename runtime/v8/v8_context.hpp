#pragma once

#include "v8_isolate.hpp"

#include <v8.h>

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace lamp::v8 {

// ============================================================
// V8 Context Errors
// ============================================================

class V8ContextError final : public std::runtime_error {
public:
    explicit V8ContextError(const std::string& message)
        : std::runtime_error(message) {}
};

// ============================================================
// V8 Context Configuration
// ============================================================

struct V8ContextConfig final {

    // Optional identifier used for diagnostics.
    //
    // This value is not exposed to JavaScript automatically.

    std::string name = "lamp";

    // Whether JavaScript code generation from strings
    // is permitted.
    //
    // When disabled, JavaScript eval() and Function()
    // constructor compilation are restricted.
    //
    // This is a hardening option, not a security sandbox.

    bool allowCodeGenerationFromStrings = true;
};

// ============================================================
// V8 Context
// ============================================================
//
// Responsibilities:
//
// - Create and own a persistent JavaScript context.
// - Maintain the context's global environment.
// - Provide scoped access to the context.
// - Support native global-object templates.
// - Manage context disposal.
// - Enforce basic lifecycle checks.
//
// Ownership:
//
// V8Context borrows a V8Isolate.
//
// The isolate must outlive this context.
//
// Threading:
//
// Construction, execution, and destruction must be
// coordinated by the isolate's owning execution thread.
//
// This class does not provide concurrent isolate access.
//
// ============================================================

class V8Context final {
public:

    // ========================================================
    // Scoped Context Access
    // ========================================================
    //
    // Creates the V8 scopes required to interact with
    // the context.
    //
    // The returned local context is valid only while
    // this scope remains alive.
    //
    // ========================================================

    class Scope final {
    public:

        explicit Scope(V8Context& owner)
            : isolate_(owner.requireIsolate()),
              locker_(isolate_),
              isolateScope_(isolate_),
              handleScope_(isolate_),
              context_(owner.requireContext(isolate_)),
              contextScope_(context_) {}

        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

        Scope(Scope&&) = delete;
        Scope& operator=(Scope&&) = delete;

        [[nodiscard]]
        ::v8::Isolate* isolate() const noexcept {
            return isolate_;
        }

        [[nodiscard]]
        ::v8::Local<::v8::Context> context() const {
            return context_;
        }

        [[nodiscard]]
        ::v8::Local<::v8::Object> global() const {
            return context_->Global();
        }

    private:

        ::v8::Isolate* isolate_;

        // The locker must be constructed before
        // entering the isolate.

        ::v8::Locker locker_;

        ::v8::Isolate::Scope isolateScope_;

        // Local handles created during this scope
        // are released when handleScope_ is destroyed.

        ::v8::HandleScope handleScope_;

        ::v8::Local<::v8::Context> context_;

        ::v8::Context::Scope contextScope_;
    };

    // ========================================================
    // Construction
    // ========================================================

    explicit V8Context(
        V8Isolate& isolate,
        V8ContextConfig config = {}
    )
        : isolate_(&isolate),
          config_(std::move(config)) {

        initialize({});
    }

    // ========================================================
    // Construction With Global Template
    // ========================================================
    //
    // The caller must already hold the correct V8
    // isolate lock and handle scope when constructing
    // a global object template.
    //
    // The template must belong to this isolate.
    //
    // ========================================================

    V8Context(
        V8Isolate& isolate,
        ::v8::Local<::v8::ObjectTemplate> globalTemplate,
        V8ContextConfig config = {}
    )
        : isolate_(&isolate),
          config_(std::move(config)) {

        initialize(globalTemplate);
    }

    // ========================================================
    // Destruction
    // ========================================================

    ~V8Context() noexcept {
        dispose();
    }

    // ========================================================
    // Non-Copyable / Non-Movable
    // ========================================================

    V8Context(const V8Context&) = delete;
    V8Context& operator=(const V8Context&) = delete;

    V8Context(V8Context&&) = delete;
    V8Context& operator=(V8Context&&) = delete;

    // ========================================================
    // Scoped Execution
    // ========================================================
    //
    // Creates a temporary execution scope.
    //
    // Usage:
    //
    //   V8Context context(isolate);
    //
    //   {
    //       V8Context::Scope scope(context);
    //
    //       auto local = scope.context();
    //
    //       // Execute JavaScript here.
    //   }
    //
    // ========================================================

    template <typename Callback>
    decltype(auto) withScope(Callback&& callback) {
        Scope scope(*this);

        return std::forward<Callback>(callback)(
            scope.isolate(),
            scope.context()
        );
    }

    // ========================================================
    // Explicit Disposal
    // ========================================================
    //
    // All active execution scopes must be destroyed
    // before this method is called.
    //
    // The owning isolate must remain alive.
    //
    // ========================================================

    void dispose() noexcept {
        if (isolate_ == nullptr || context_.IsEmpty()) {
            return;
        }

        auto* isolate = isolate_->get();

        if (isolate == nullptr) {
            // This indicates a lifecycle violation:
            // the isolate was disposed before its context.
            //
            // We cannot safely reset a persistent handle
            // after its isolate has been destroyed.

            std::terminate();
        }

        ::v8::Locker locker(isolate);
        ::v8::Isolate::Scope isolateScope(isolate);
        ::v8::HandleScope handleScope(isolate);

        context_.Reset();
    }

    // ========================================================
    // State Inspection
    // ========================================================

    [[nodiscard]]
    bool isValid() const noexcept {
        return isolate_ != nullptr &&
               isolate_->isValid() &&
               !context_.IsEmpty();
    }

    [[nodiscard]]
    const V8ContextConfig& config() const noexcept {
        return config_;
    }

private:

    // ========================================================
    // Initialize Context
    // ========================================================

    void initialize(
        ::v8::Local<::v8::ObjectTemplate> globalTemplate
    ) {
        auto* isolate = requireIsolate();

        ::v8::Locker locker(isolate);
        ::v8::Isolate::Scope isolateScope(isolate);
        ::v8::HandleScope handleScope(isolate);

        auto context = ::v8::Context::New(
            isolate,
            nullptr,
            globalTemplate
        );

        if (context.IsEmpty()) {
            throw V8ContextError(
                "Failed to create V8 context"
            );
        }

        context->AllowCodeGenerationFromStrings(
            config_.allowCodeGenerationFromStrings
        );

        context_.Reset(isolate, context);
    }

    // ========================================================
    // Validate Isolate
    // ========================================================

    [[nodiscard]]
    ::v8::Isolate* requireIsolate() const {
        if (isolate_ == nullptr || !isolate_->isValid()) {
            throw V8ContextError(
                "V8 context requires a valid isolate"
            );
        }

        return isolate_->get();
    }

    // ========================================================
    // Retrieve Persistent Context
    // ========================================================
    //
    // Must be called while the isolate is entered
    // and an active handle scope exists.
    //
    // ========================================================

    [[nodiscard]]
    ::v8::Local<::v8::Context> requireContext(
        ::v8::Isolate* isolate
    ) const {
        if (context_.IsEmpty()) {
            throw V8ContextError(
                "V8 context has already been disposed"
            );
        }

        return context_.Get(isolate);
    }

    // ========================================================
    // Internal State
    // ========================================================

    V8Isolate* isolate_ = nullptr;

    V8ContextConfig config_{};

    ::v8::Global<::v8::Context> context_;
};

} // namespace lamp::v8