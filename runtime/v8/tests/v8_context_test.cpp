// ============================================================
// Lamp Framework - V8 Context Tests
// ============================================================
//
// File:
//   runtime/v8/tests/v8_context_test.cpp
//
// Purpose:
//
//   Validate V8Context lifecycle, scope management,
//   JavaScript execution, and context isolation.
//
// Coverage:
//
//   1. Default context construction
//   2. Context validity
//   3. Default configuration
//   4. Custom configuration
//   5. Scoped context access
//   6. Context identity
//   7. JavaScript execution
//   8. Global variable persistence
//   9. Independent global objects
//  10. Multiple contexts sharing one isolate
//  11. Context global-object mutation
//  12. Nested context scopes
//  13. withScope callback
//  14. Explicit disposal
//  15. Repeated disposal
//  16. Automatic destruction
//  17. Context recreation after disposal
//  18. Code-generation restriction
//  19. Construction with disposed isolate
//
// Lifecycle:
//
//   V8Platform
//       |
//       +-- V8Isolate
//               |
//               +-- V8Context A
//               |
//               +-- V8Context B
//               |
//       +-- Destroy contexts
//       +-- Destroy isolate
//       |
//   V8Platform shutdown
//
// ============================================================

#include "v8/v8_platform.hpp"
#include "v8/v8_isolate.hpp"
#include "v8/v8_context.hpp"

#include <v8.h>

#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

// ============================================================
// Test Framework
// ============================================================

class TestFailure final : public std::runtime_error {
public:
    explicit TestFailure(std::string message)
        : std::runtime_error(std::move(message)) {}
};

struct TestCase final {
    std::string name;
    std::function<void()> execute;
};

class TestRunner final {
public:
    void add(
        std::string name,
        std::function<void()> test
    ) {
        tests_.push_back({
            std::move(name),
            std::move(test)
        });
    }

    [[nodiscard]]
    int run() const {
        std::size_t passed = 0;
        std::size_t failed = 0;

        std::cout
            << "\n"
            << "========================================\n"
            << " Lamp V8 Context Tests\n"
            << "========================================\n";

        for (const auto& test : tests_) {
            try {
                test.execute();
                ++passed;

                std::cout
                    << "[PASS] "
                    << test.name
                    << '\n';

            } catch (const std::exception& error) {
                ++failed;

                std::cerr
                    << "[FAIL] "
                    << test.name
                    << ": "
                    << error.what()
                    << '\n';

            } catch (...) {
                ++failed;

                std::cerr
                    << "[FAIL] "
                    << test.name
                    << ": unknown exception\n";
            }
        }

        std::cout
            << "----------------------------------------\n"
            << " Passed : " << passed << '\n'
            << " Failed : " << failed << '\n'
            << " Total  : " << tests_.size() << '\n'
            << "========================================\n";

        return failed == 0 ? 0 : 1;
    }

private:
    std::vector<TestCase> tests_;
};

// ============================================================
// Assertions
// ============================================================

void require(
    bool condition,
    std::string_view message
) {
    if (!condition) {
        throw TestFailure(std::string(message));
    }
}

template <typename Exception, typename Callback>
void requireThrows(
    Callback&& callback,
    std::string_view message
) {
    bool caught = false;

    try {
        std::forward<Callback>(callback)();

    } catch (const Exception&) {
        caught = true;
    }

    require(caught, message);
}

// ============================================================
// JavaScript Evaluation Helpers
// ============================================================
//
// These helpers must be called while the appropriate
// isolate, handle scope, and context scope are active.
//
// They intentionally use V8 directly so this suite tests
// V8Context independently of V8ScriptEngine.
//
// ============================================================

::v8::Local<::v8::String> makeString(
    ::v8::Isolate* isolate,
    std::string_view text
) {
    ::v8::Local<::v8::String> result;

    const auto status = ::v8::String::NewFromUtf8(
        isolate,
        text.data(),
        ::v8::NewStringType::kNormal,
        static_cast<int>(text.size())
    );

    require(
        status.ToLocal(&result),
        "Failed to create V8 string"
    );

    return result;
}

::v8::Local<::v8::Value> evaluate(
    ::v8::Isolate* isolate,
    ::v8::Local<::v8::Context> context,
    std::string_view source
) {
    auto scriptSource = makeString(
        isolate,
        source
    );

    ::v8::Local<::v8::Script> script;

    require(
        ::v8::Script::Compile(
            context,
            scriptSource
        ).ToLocal(&script),
        "JavaScript compilation failed"
    );

    ::v8::Local<::v8::Value> result;

    require(
        script->Run(context).ToLocal(&result),
        "JavaScript execution failed"
    );

    return result;
}

double evaluateNumber(
    ::v8::Isolate* isolate,
    ::v8::Local<::v8::Context> context,
    std::string_view source
) {
    auto result = evaluate(
        isolate,
        context,
        source
    );

    require(
        result->IsNumber(),
        "JavaScript result must be numeric"
    );

    return result.As<::v8::Number>()->Value();
}

bool evaluateBoolean(
    ::v8::Isolate* isolate,
    ::v8::Local<::v8::Context> context,
    std::string_view source
) {
    auto result = evaluate(
        isolate,
        context,
        source
    );

    require(
        result->IsBoolean(),
        "JavaScript result must be boolean"
    );

    return result->BooleanValue(isolate);
}

// ============================================================
// Test 1: Default Context Construction
// ============================================================

void testDefaultConstruction() {
    lamp::v8::V8Isolate isolate;
    lamp::v8::V8Context context(isolate);

    require(
        context.isValid(),
        "Default context must be valid"
    );
}

// ============================================================
// Test 2: Context Validity
// ============================================================

void testContextValidity() {
    lamp::v8::V8Isolate isolate;
    lamp::v8::V8Context context(isolate);

    require(
        context.isValid(),
        "Newly created context must be valid"
    );

    context.withScope(
        [](
            ::v8::Isolate* native,
            ::v8::Local<::v8::Context> local
        ) {
            require(
                native != nullptr,
                "Native isolate must not be null"
            );

            require(
                !local.IsEmpty(),
                "Local context handle must not be empty"
            );
        }
    );
}

// ============================================================
// Test 3: Default Configuration
// ============================================================

void testDefaultConfiguration() {
    lamp::v8::V8Isolate isolate;
    lamp::v8::V8Context context(isolate);

    const auto& config = context.config();

    require(
        config.name == "lamp",
        "Default context name must be lamp"
    );

    require(
        config.allowCodeGenerationFromStrings,
        "String-based code generation must be enabled by default"
    );
}

// ============================================================
// Test 4: Custom Configuration
// ============================================================

void testCustomConfiguration() {
    lamp::v8::V8Isolate isolate;

    lamp::v8::V8ContextConfig config{};

    config.name = "lamp-test-context";
    config.allowCodeGenerationFromStrings = false;

    lamp::v8::V8Context context(
        isolate,
        config
    );

    require(
        context.isValid(),
        "Custom context must be valid"
    );

    require(
        context.config().name == "lamp-test-context",
        "Custom context name must be preserved"
    );

    require(
        !context.config().allowCodeGenerationFromStrings,
        "Custom code-generation restriction must be preserved"
    );
}

// ============================================================
// Test 5: Scoped Context Access
// ============================================================

void testScopedContextAccess() {
    lamp::v8::V8Isolate isolate;
    lamp::v8::V8Context context(isolate);

    lamp::v8::V8Context::Scope scope(context);

    auto* native = scope.isolate();
    auto local = scope.context();

    require(
        native == isolate.get(),
        "Scope must expose the owning isolate"
    );

    require(
        !local.IsEmpty(),
        "Scope must expose a valid local context"
    );

    require(
        native->GetCurrentContext() == local,
        "Scope must enter its context"
    );
}

// ============================================================
// Test 6: Context Identity
// ============================================================

void testContextIdentity() {
    lamp::v8::V8Isolate isolate;
    lamp::v8::V8Context context(isolate);

    context.withScope(
        [&context](
            ::v8::Isolate* isolate,
            ::v8::Local<::v8::Context> first
        ) {
            (void)isolate;

            lamp::v8::V8Context::Scope nested(context);

            auto second = nested.context();

            require(
                first == second,
                "Repeated scopes must reference the same context"
            );
        }
    );
}

// ============================================================
// Test 7: JavaScript Execution
// ============================================================

void testJavaScriptExecution() {
    lamp::v8::V8Isolate isolate;
    lamp::v8::V8Context context(isolate);

    context.withScope(
        [](
            ::v8::Isolate* native,
            ::v8::Local<::v8::Context> local
        ) {
            const auto result = evaluateNumber(
                native,
                local,
                "20 + 22"
            );

            require(
                result == 42.0,
                "JavaScript arithmetic must return 42"
            );
        }
    );
}

// ============================================================
// Test 8: Global Variable Persistence
// ============================================================

void testGlobalVariablePersistence() {
    lamp::v8::V8Isolate isolate;
    lamp::v8::V8Context context(isolate);

    context.withScope(
        [](
            ::v8::Isolate* native,
            ::v8::Local<::v8::Context> local
        ) {
            const auto result = evaluateNumber(
                native,
                local,
                "globalThis.counter = 10; counter;"
            );

            require(
                result == 10.0,
                "Initial global value must be 10"
            );
        }
    );

    context.withScope(
        [](
            ::v8::Isolate* native,
            ::v8::Local<::v8::Context> local
        ) {
            const auto result = evaluateNumber(
                native,
                local,
                "globalThis.counter += 5; counter;"
            );

            require(
                result == 15.0,
                "Global state must persist across scopes"
            );
        }
    );
}

// ============================================================
// Test 9: Independent Global Objects
// ============================================================

void testIndependentGlobalObjects() {
    lamp::v8::V8Isolate isolate;

    lamp::v8::V8Context first(isolate);
    lamp::v8::V8Context second(isolate);

    first.withScope(
        [](
            ::v8::Isolate* native,
            ::v8::Local<::v8::Context> local
        ) {
            evaluate(
                native,
                local,
                "globalThis.value = 100;"
            );
        }
    );

    second.withScope(
        [](
            ::v8::Isolate* native,
            ::v8::Local<::v8::Context> local
        ) {
            require(
                evaluateBoolean(
                    native,
                    local,
                    "typeof globalThis.value === 'undefined'"
                ),
                "Second context must not inherit first context globals"
            );
        }
    );
}

// ============================================================
// Test 10: Multiple Contexts Sharing One Isolate
// ============================================================

void testMultipleContextsOneIsolate() {
    lamp::v8::V8Isolate isolate;

    lamp::v8::V8Context first(isolate);
    lamp::v8::V8Context second(isolate);
    lamp::v8::V8Context third(isolate);

    require(
        first.isValid() &&
        second.isValid() &&
        third.isValid(),
        "All contexts must be valid"
    );

    first.withScope(
        [](
            ::v8::Isolate* native,
            ::v8::Local<::v8::Context> local
        ) {
            require(
                evaluateNumber(
                    native,
                    local,
                    "1 + 1"
                ) == 2.0,
                "First context must execute JavaScript"
            );
        }
    );

    second.withScope(
        [](
            ::v8::Isolate* native,
            ::v8::Local<::v8::Context> local
        ) {
            require(
                evaluateNumber(
                    native,
                    local,
                    "2 + 2"
                ) == 4.0,
                "Second context must execute JavaScript"
            );
        }
    );

    third.withScope(
        [](
            ::v8::Isolate* native,
            ::v8::Local<::v8::Context> local
        ) {
            require(
                evaluateNumber(
                    native,
                    local,
                    "3 + 3"
                ) == 6.0,
                "Third context must execute JavaScript"
            );
        }
    );
}

// ============================================================
// Test 11: Global Object Mutation
// ============================================================

void testGlobalObjectMutation() {
    lamp::v8::V8Isolate isolate;
    lamp::v8::V8Context context(isolate);

    context.withScope(
        [](
            ::v8::Isolate* native,
            ::v8::Local<::v8::Context> local
        ) {
            auto global = local->Global();

            auto key = makeString(
                native,
                "nativeValue"
            );

            require(
                global->Set(
                    local,
                    key,
                    ::v8::Number::New(native, 123)
                ).FromMaybe(false),
                "Setting global property must succeed"
            );

            require(
                evaluateNumber(
                    native,
                    local,
                    "globalThis.nativeValue"
                ) == 123.0,
                "JavaScript must read native global property"
            );
        }
    );
}

// ============================================================
// Test 12: Nested Context Scopes
// ============================================================

void testNestedContextScopes() {
    lamp::v8::V8Isolate isolate;

    lamp::v8::V8Context first(isolate);
    lamp::v8::V8Context second(isolate);

    lamp::v8::V8Context::Scope firstScope(first);

    auto* native = firstScope.isolate();

    require(
        native->GetCurrentContext() ==
            firstScope.context(),
        "First context must be current"
    );

    {
        lamp::v8::V8Context::Scope secondScope(second);

        require(
            native->GetCurrentContext() ==
                secondScope.context(),
            "Nested context must become current"
        );
    }

    require(
        native->GetCurrentContext() ==
            firstScope.context(),
        "Outer context must be restored"
    );
}

// ============================================================
// Test 13: withScope Callback
// ============================================================

void testWithScopeCallback() {
    lamp::v8::V8Isolate isolate;
    lamp::v8::V8Context context(isolate);

    bool invoked = false;

    context.withScope(
        [&invoked](
            ::v8::Isolate* native,
            ::v8::Local<::v8::Context> local
        ) {
            invoked = true;

            require(
                native != nullptr,
                "Callback must receive an isolate"
            );

            require(
                !local.IsEmpty(),
                "Callback must receive a context"
            );
        }
    );

    require(
        invoked,
        "withScope callback must execute"
    );
}

// ============================================================
// Test 14: Explicit Disposal
// ============================================================

void testExplicitDisposal() {
    lamp::v8::V8Isolate isolate;
    lamp::v8::V8Context context(isolate);

    require(
        context.isValid(),
        "Context must initially be valid"
    );

    context.dispose();

    require(
        !context.isValid(),
        "Disposed context must be invalid"
    );
}

// ============================================================
// Test 15: Repeated Disposal
// ============================================================

void testRepeatedDisposal() {
    lamp::v8::V8Isolate isolate;
    lamp::v8::V8Context context(isolate);

    context.dispose();
    context.dispose();

    require(
        !context.isValid(),
        "Repeated disposal must preserve invalid state"
    );
}

// ============================================================
// Test 16: Automatic Destruction
// ============================================================

void testAutomaticDestruction() {
    lamp::v8::V8Isolate isolate;

    {
        auto context =
            std::make_unique<lamp::v8::V8Context>(
                isolate
            );

        require(
            context->isValid(),
            "Heap-allocated context must be valid"
        );
    }

    // Context destructor has completed.
    // The isolate remains valid.

    require(
        isolate.isValid(),
        "Destroying a context must not dispose its isolate"
    );
}

// ============================================================
// Test 17: Context Recreation
// ============================================================

void testContextRecreation() {
    lamp::v8::V8Isolate isolate;

    {
        lamp::v8::V8Context first(isolate);

        require(
            first.isValid(),
            "First context must be valid"
        );

        first.dispose();
    }

    lamp::v8::V8Context second(isolate);

    require(
        second.isValid(),
        "A new context must be creatable after disposal"
    );
}

// ============================================================
// Test 18: Code Generation Restriction
// ============================================================

void testCodeGenerationRestriction() {
    lamp::v8::V8Isolate isolate;

    lamp::v8::V8ContextConfig config{};

    config.allowCodeGenerationFromStrings = false;

    lamp::v8::V8Context context(
        isolate,
        config
    );

    context.withScope(
        [](
            ::v8::Isolate* native,
            ::v8::Local<::v8::Context> local
        ) {
            ::v8::TryCatch tryCatch(native);

            auto source = makeString(
                native,
                "eval('1 + 1')"
            );

            ::v8::Local<::v8::Script> script;

            require(
                ::v8::Script::Compile(
                    local,
                    source
                ).ToLocal(&script),
                "Outer script must compile"
            );

            ::v8::Local<::v8::Value> result;

            const bool succeeded =
                script->Run(local).ToLocal(&result);

            require(
                !succeeded,
                "String-based eval must be rejected"
            );

            require(
                tryCatch.HasCaught(),
                "Rejected eval must produce a JS exception"
            );
        }
    );
}

// ============================================================
// Test 19: Construction With Disposed Isolate
// ============================================================

void testDisposedIsolateRejection() {
    lamp::v8::V8Isolate isolate;

    isolate.dispose();

    requireThrows<lamp::v8::V8ContextError>(
        [&isolate]() {
            lamp::v8::V8Context context(isolate);
        },
        "Context construction must reject disposed isolate"
    );
}

} // namespace

// ============================================================
// Main
// ============================================================

int main() {
    auto& platform =
        lamp::v8::V8Platform::instance();

    // --------------------------------------------------------
    // Initialize Process-Wide V8 Platform
    // --------------------------------------------------------

    try {
        platform.initialize();

    } catch (const std::exception& error) {
        std::cerr
            << "[FATAL] V8 platform initialization failed: "
            << error.what()
            << '\n';

        return 1;
    }

    TestRunner runner;

    // --------------------------------------------------------
    // Register Context Tests
    // --------------------------------------------------------

    runner.add(
        "Default context construction",
        testDefaultConstruction
    );

    runner.add(
        "Context validity",
        testContextValidity
    );

    runner.add(
        "Default context configuration",
        testDefaultConfiguration
    );

    runner.add(
        "Custom context configuration",
        testCustomConfiguration
    );

    runner.add(
        "Scoped context access",
        testScopedContextAccess
    );

    runner.add(
        "Context identity",
        testContextIdentity
    );

    runner.add(
        "JavaScript execution",
        testJavaScriptExecution
    );

    runner.add(
        "Global variable persistence",
        testGlobalVariablePersistence
    );

    runner.add(
        "Independent global objects",
        testIndependentGlobalObjects
    );

    runner.add(
        "Multiple contexts sharing one isolate",
        testMultipleContextsOneIsolate
    );

    runner.add(
        "Global object mutation",
        testGlobalObjectMutation
    );

    runner.add(
        "Nested context scopes",
        testNestedContextScopes
    );

    runner.add(
        "withScope callback",
        testWithScopeCallback
    );

    runner.add(
        "Explicit context disposal",
        testExplicitDisposal
    );

    runner.add(
        "Repeated context disposal",
        testRepeatedDisposal
    );

    runner.add(
        "Automatic context destruction",
        testAutomaticDestruction
    );

    runner.add(
        "Context recreation after disposal",
        testContextRecreation
    );

    runner.add(
        "Code-generation restriction",
        testCodeGenerationRestriction
    );

    runner.add(
        "Disposed isolate rejection",
        testDisposedIsolateRejection
    );

    // --------------------------------------------------------
    // Execute Tests
    // --------------------------------------------------------

    const int result = runner.run();

    // --------------------------------------------------------
    // Shutdown V8 After All Contexts And Isolates
    // Have Been Destroyed
    // --------------------------------------------------------

    try {
        platform.shutdown();

    } catch (const std::exception& error) {
        std::cerr
            << "[FATAL] V8 platform shutdown failed: "
            << error.what()
            << '\n';

        return 1;
    }

    return result;
}