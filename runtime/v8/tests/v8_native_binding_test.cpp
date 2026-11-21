// ============================================================
// Lamp Framework - V8 Native Binding Tests
// ============================================================
//
// File:
//   runtime/v8/tests/v8_native_binding_test.cpp
//
// Purpose:
//   Verify JavaScript-to-C++ native function bindings.
//
// Coverage:
//   - Function registration
//   - Function installation
//   - Primitive arguments and return values
//   - Arrays and objects
//   - Multiple native functions
//   - Repeated invocation
//   - Duplicate registration
//   - Missing functions
//   - Argument limits
//   - Conversion failures
//   - C++ exception propagation
//   - Registry isolation
//   - Disposal and callback safety
//
// Lifecycle:
//
//   V8Platform
//       |
//       +-- V8Isolate
//               |
//               +-- V8Context
//                       |
//                       +-- V8NativeBinding
//                       |
//                       +-- JavaScript execution
//                       |
//                       +-- Dispose bindings
//                       +-- Destroy context
//               +-- Destroy isolate
//       +-- Shutdown platform
//
// ============================================================

#include "v8/v8_platform.hpp"
#include "v8/v8_isolate.hpp"
#include "v8/v8_context.hpp"
#include "v8/v8_value.hpp"
#include "v8/v8_native_binding.hpp"

#include <v8.h>

#include <cstddef>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <variant>

namespace {

// ============================================================
// Test Infrastructure
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
            << " Lamp V8 Native Binding Tests\n"
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
// JavaScript Helpers
// ============================================================

::v8::Local<::v8::String> makeString(
    ::v8::Isolate* isolate,
    std::string_view source
) {
    ::v8::Local<::v8::String> result;

    require(
        ::v8::String::NewFromUtf8(
            isolate,
            source.data(),
            ::v8::NewStringType::kNormal,
            static_cast<int>(source.size())
        ).ToLocal(&result),
        "Failed to create V8 string"
    );

    return result;
}

::v8::Local<::v8::Value> evaluate(
    ::v8::Isolate* isolate,
    ::v8::Local<::v8::Context> context,
    std::string_view source
) {
    ::v8::TryCatch tryCatch(isolate);

    auto text = makeString(isolate, source);

    ::v8::Local<::v8::Script> script;

    if (!::v8::Script::Compile(
            context,
            text
        ).ToLocal(&script)) {

        throw TestFailure(
            "JavaScript compilation failed"
        );
    }

    ::v8::Local<::v8::Value> result;

    if (!script->Run(context).ToLocal(&result)) {
        std::string message =
            "JavaScript execution failed";

        if (tryCatch.HasCaught()) {
            ::v8::String::Utf8Value utf8(
                isolate,
                tryCatch.Exception()
            );

            if (*utf8 != nullptr) {
                message += ": ";
                message.append(
                    *utf8,
                    static_cast<std::size_t>(utf8.length())
                );
            }
        }

        throw TestFailure(std::move(message));
    }

    return result;
}

double evaluateNumber(
    ::v8::Isolate* isolate,
    ::v8::Local<::v8::Context> context,
    std::string_view source
) {
    auto value = evaluate(
        isolate,
        context,
        source
    );

    require(
        value->IsNumber(),
        "JavaScript result must be numeric"
    );

    return value.As<::v8::Number>()->Value();
}

bool evaluateBoolean(
    ::v8::Isolate* isolate,
    ::v8::Local<::v8::Context> context,
    std::string_view source
) {
    auto value = evaluate(
        isolate,
        context,
        source
    );

    require(
        value->IsBoolean(),
        "JavaScript result must be boolean"
    );

    return value->BooleanValue(isolate);
}

std::string evaluateString(
    ::v8::Isolate* isolate,
    ::v8::Local<::v8::Context> context,
    std::string_view source
) {
    auto value = evaluate(
        isolate,
        context,
        source
    );

    require(
        value->IsString(),
        "JavaScript result must be a string"
    );

    ::v8::String::Utf8Value utf8(
        isolate,
        value
    );

    require(
        *utf8 != nullptr,
        "String conversion failed"
    );

    return std::string(
        *utf8,
        static_cast<std::size_t>(utf8.length())
    );
}

// ============================================================
// Fixture
// ============================================================

class BindingFixture final {
public:
    explicit BindingFixture(
        lamp::v8::V8NativeBindingConfig config = {}
    )
        : isolate_(),
          context_(isolate_),
          bindings_(context_, std::move(config)) {}

    lamp::v8::V8NativeBinding& bindings() {
        return bindings_;
    }

    template <typename Callback>
    void withScope(Callback&& callback) {
        context_.withScope(
            std::forward<Callback>(callback)
        );
    }

    double number(std::string_view source) {
        double result = 0;

        withScope(
            [&](::v8::Isolate* isolate,
                ::v8::Local<::v8::Context> context) {
                result = evaluateNumber(
                    isolate,
                    context,
                    source
                );
            }
        );

        return result;
    }

    bool boolean(std::string_view source) {
        bool result = false;

        withScope(
            [&](::v8::Isolate* isolate,
                ::v8::Local<::v8::Context> context) {
                result = evaluateBoolean(
                    isolate,
                    context,
                    source
                );
            }
        );

        return result;
    }

    std::string string(std::string_view source) {
        std::string result;

        withScope(
            [&](::v8::Isolate* isolate,
                ::v8::Local<::v8::Context> context) {
                result = evaluateString(
                    isolate,
                    context,
                    source
                );
            }
        );

        return result;
    }

    void execute(std::string_view source) {
        withScope(
            [&](::v8::Isolate* isolate,
                ::v8::Local<::v8::Context> context) {
                (void)evaluate(
                    isolate,
                    context,
                    source
                );
            }
        );
    }

private:
    lamp::v8::V8Isolate isolate_;
    lamp::v8::V8Context context_;
    lamp::v8::V8NativeBinding bindings_;
};

// ============================================================
// Native Handler Helpers
// ============================================================

lamp::v8::V8NativeHandler makeAddHandler() {
    return [](
        const lamp::v8::V8NativeArguments& args
    ) -> lamp::v8::V8Value {
        if (args.size() != 2) {
            throw std::invalid_argument(
                "nativeAdd expects exactly two arguments"
            );
        }

        if (!args[0].isNumber() ||
            !args[1].isNumber()) {
            throw std::invalid_argument(
                "nativeAdd expects numeric arguments"
            );
        }

        const double a =
            std::get<double>(args[0].data);

        const double b =
            std::get<double>(args[1].data);

        return lamp::v8::V8Value(a + b);
    };
}

lamp::v8::V8NativeHandler makeMultiplyHandler() {
    return [](
        const lamp::v8::V8NativeArguments& args
    ) -> lamp::v8::V8Value {
        if (args.size() != 2) {
            throw std::invalid_argument(
                "nativeMultiply expects two arguments"
            );
        }

        return lamp::v8::V8Value(
            std::get<double>(args[0].data) *
            std::get<double>(args[1].data)
        );
    };
}

// ============================================================
// Test 1: Initial Registry State
// ============================================================

void testInitialRegistryState() {
    BindingFixture fixture;

    require(
        fixture.bindings().size() == 0,
        "New registry must be empty"
    );

    require(
        !fixture.bindings().contains("nativeAdd"),
        "Unregistered function must not exist"
    );
}

// ============================================================
// Test 2: Function Registration
// ============================================================

void testFunctionRegistration() {
    BindingFixture fixture;

    fixture.bindings().registerFunction(
        "nativeAdd",
        makeAddHandler()
    );

    require(
        fixture.bindings().contains("nativeAdd"),
        "Registered function must exist"
    );

    require(
        fixture.bindings().size() == 1,
        "Registry must contain one function"
    );
}

// ============================================================
// Test 3: Function Installation
// ============================================================

void testFunctionInstallation() {
    BindingFixture fixture;

    fixture.bindings().registerFunction(
        "nativeAdd",
        makeAddHandler()
    );

    require(
        !fixture.bindings().isInstalled("nativeAdd"),
        "Function must not initially be installed"
    );

    fixture.bindings().install("nativeAdd");

    require(
        fixture.bindings().isInstalled("nativeAdd"),
        "Function must be marked installed"
    );

    require(
        fixture.boolean(
            "typeof nativeAdd === 'function'"
        ),
        "Installed function must be visible in JavaScript"
    );
}

// ============================================================
// Test 4: Numeric Arguments
// ============================================================

void testNumericArguments() {
    BindingFixture fixture;

    fixture.bindings().registerFunction(
        "nativeAdd",
        makeAddHandler()
    );

    fixture.bindings().installAll();

    require(
        fixture.number("nativeAdd(20, 22)") == 42.0,
        "Native addition must return 42"
    );
}

// ============================================================
// Test 5: Negative Numbers
// ============================================================

void testNegativeNumbers() {
    BindingFixture fixture;

    fixture.bindings().registerFunction(
        "nativeAdd",
        makeAddHandler()
    );

    fixture.bindings().installAll();

    require(
        fixture.number("nativeAdd(-10, 5)") == -5.0,
        "Negative arguments must be preserved"
    );
}

// ============================================================
// Test 6: Boolean Return
// ============================================================

void testBooleanReturn() {
    BindingFixture fixture;

    fixture.bindings().registerFunction(
        "nativeEnabled",
        [](
            const lamp::v8::V8NativeArguments&
        ) -> lamp::v8::V8Value {
            return lamp::v8::V8Value(true);
        }
    );

    fixture.bindings().installAll();

    require(
        fixture.boolean("nativeEnabled()"),
        "Boolean return value must be true"
    );
}

// ============================================================
// Test 7: String Return
// ============================================================

void testStringReturn() {
    BindingFixture fixture;

    fixture.bindings().registerFunction(
        "nativeName",
        [](
            const lamp::v8::V8NativeArguments&
        ) -> lamp::v8::V8Value {
            return lamp::v8::V8Value(
                std::string("Lamp Framework")
            );
        }
    );

    fixture.bindings().installAll();

    require(
        fixture.string("nativeName()") ==
            "Lamp Framework",
        "String return value must be preserved"
    );
}

// ============================================================
// Test 8: String Argument
// ============================================================

void testStringArgument() {
    BindingFixture fixture;

    fixture.bindings().registerFunction(
        "nativeEcho",
        [](
            const lamp::v8::V8NativeArguments& args
        ) -> lamp::v8::V8Value {
            if (args.size() != 1 ||
                !args[0].isString()) {
                throw std::invalid_argument(
                    "Expected one string"
                );
            }

            return args[0];
        }
    );

    fixture.bindings().installAll();

    require(
        fixture.string("nativeEcho('hello lamp')") ==
            "hello lamp",
        "String argument must survive native round-trip"
    );
}

// ============================================================
// Test 9: Multiple Functions
// ============================================================

void testMultipleFunctions() {
    BindingFixture fixture;

    fixture.bindings().registerFunction(
        "nativeAdd",
        makeAddHandler()
    );

    fixture.bindings().registerFunction(
        "nativeMultiply",
        makeMultiplyHandler()
    );

    fixture.bindings().installAll();

    require(
        fixture.bindings().size() == 2,
        "Two functions must be registered"
    );

    require(
        fixture.number("nativeAdd(2, 3)") == 5.0,
        "Addition must work"
    );

    require(
        fixture.number("nativeMultiply(6, 7)") == 42.0,
        "Multiplication must work"
    );
}

// ============================================================
// Test 10: Repeated Invocation
// ============================================================

void testRepeatedInvocation() {
    BindingFixture fixture;

    std::size_t calls = 0;

    fixture.bindings().registerFunction(
        "nativeCounter",
        [&calls](
            const lamp::v8::V8NativeArguments&
        ) -> lamp::v8::V8Value {
            ++calls;

            return lamp::v8::V8Value(
                static_cast<double>(calls)
            );
        }
    );

    fixture.bindings().installAll();

    require(
        fixture.number("nativeCounter()") == 1.0,
        "First invocation must return 1"
    );

    require(
        fixture.number("nativeCounter()") == 2.0,
        "Second invocation must return 2"
    );

    require(
        calls == 2,
        "Handler must execute twice"
    );
}

// ============================================================
// Test 11: Array Argument
// ============================================================

void testArrayArgument() {
    BindingFixture fixture;

    fixture.bindings().registerFunction(
        "nativeSum",
        [](
            const lamp::v8::V8NativeArguments& args
        ) -> lamp::v8::V8Value {
            if (args.size() != 1 ||
                !args[0].isArray()) {
                throw std::invalid_argument(
                    "Expected one array"
                );
            }

            const auto& values =
                std::get<lamp::v8::V8ValueArray>(
                    args[0].data
                );

            double sum = 0;

            for (const auto& value : values) {
                if (!value || !value->isNumber()) {
                    throw std::invalid_argument(
                        "Array must contain numbers"
                    );
                }

                sum += std::get<double>(value->data);
            }

            return lamp::v8::V8Value(sum);
        }
    );

    fixture.bindings().installAll();

    require(
        fixture.number(
            "nativeSum([10, 20, 12])"
        ) == 42.0,
        "Array argument must be converted correctly"
    );
}

// ============================================================
// Test 12: Object Argument
// ============================================================

void testObjectArgument() {
    BindingFixture fixture;

    fixture.bindings().registerFunction(
        "nativeGetPort",
        [](
            const lamp::v8::V8NativeArguments& args
        ) -> lamp::v8::V8Value {
            if (args.size() != 1 ||
                !args[0].isObject()) {
                throw std::invalid_argument(
                    "Expected one object"
                );
            }

            const auto& object =
                std::get<lamp::v8::V8ValueObject>(
                    args[0].data
                );

            const auto it = object.find("port");

            if (it == object.end() ||
                !it->second ||
                !it->second->isNumber()) {
                throw std::invalid_argument(
                    "Missing numeric port"
                );
            }

            return *it->second;
        }
    );

    fixture.bindings().installAll();

    require(
        fixture.number(
            "nativeGetPort({ port: 3000 })"
        ) == 3000.0,
        "Object argument must be converted correctly"
    );
}

// ============================================================
// Test 13: Array Return
// ============================================================

void testArrayReturn() {
    BindingFixture fixture;

    fixture.bindings().registerFunction(
        "nativeNumbers",
        [](
            const lamp::v8::V8NativeArguments&
        ) -> lamp::v8::V8Value {
            lamp::v8::V8ValueArray values;

            values.push_back(
                std::make_shared<lamp::v8::V8Value>(
                    10.0
                )
            );

            values.push_back(
                std::make_shared<lamp::v8::V8Value>(
                    32.0
                )
            );

            return lamp::v8::V8Value(
                std::move(values)
            );
        }
    );

    fixture.bindings().installAll();

    require(
        fixture.number(
            "nativeNumbers()[0] + nativeNumbers()[1]"
        ) == 42.0,
        "Array return must be accessible in JavaScript"
    );
}

// ============================================================
// Test 14: Object Return
// ============================================================

void testObjectReturn() {
    BindingFixture fixture;

    fixture.bindings().registerFunction(
        "nativeConfig",
        [](
            const lamp::v8::V8NativeArguments&
        ) -> lamp::v8::V8Value {
            lamp::v8::V8ValueObject object;

            object.emplace(
                "port",
                std::make_shared<lamp::v8::V8Value>(
                    3000.0
                )
            );

            return lamp::v8::V8Value(
                std::move(object)
            );
        }
    );

    fixture.bindings().installAll();

    require(
        fixture.number(
            "nativeConfig().port"
        ) == 3000.0,
        "Object return must be accessible in JavaScript"
    );
}

// ============================================================
// Test 15: Duplicate Registration
// ============================================================

void testDuplicateRegistration() {
    BindingFixture fixture;

    fixture.bindings().registerFunction(
        "nativeAdd",
        makeAddHandler()
    );

    requireThrows<lamp::v8::V8NativeBindingError>(
        [&fixture]() {
            fixture.bindings().registerFunction(
                "nativeAdd",
                makeAddHandler()
            );
        },
        "Duplicate registration must be rejected"
    );

    require(
        fixture.bindings().size() == 1,
        "Duplicate registration must not add another entry"
    );
}

// ============================================================
// Test 16: Missing Function Installation
// ============================================================

void testMissingFunctionInstallation() {
    BindingFixture fixture;

    requireThrows<lamp::v8::V8NativeBindingError>(
        [&fixture]() {
            fixture.bindings().install(
                "missingFunction"
            );
        },
        "Installing an unknown function must fail"
    );
}

// ============================================================
// Test 17: Argument Count Limit
// ============================================================

void testArgumentCountLimit() {
    lamp::v8::V8NativeBindingConfig config{};

    config.maxArguments = 2;

    BindingFixture fixture(config);

    fixture.bindings().registerFunction(
        "nativeAdd",
        makeAddHandler()
    );

    fixture.bindings().installAll();

    require(
        fixture.boolean(
            "(() => {"
            "  try {"
            "    nativeAdd(1, 2, 3);"
            "    return false;"
            "  } catch (error) {"
            "    return error instanceof Error;"
            "  }"
            "})()"
        ),
        "Argument count limit must throw a JavaScript error"
    );
}

// ============================================================
// Test 18: Unsupported Argument
// ============================================================

void testUnsupportedArgument() {
    BindingFixture fixture;

    fixture.bindings().registerFunction(
        "nativeEcho",
        [](
            const lamp::v8::V8NativeArguments& args
        ) -> lamp::v8::V8Value {
            return args.at(0);
        }
    );

    fixture.bindings().installAll();

    require(
        fixture.boolean(
            "(() => {"
            "  try {"
            "    nativeEcho(Symbol('x'));"
            "    return false;"
            "  } catch (error) {"
            "    return error instanceof Error;"
            "  }"
            "})()"
        ),
        "Unsupported arguments must throw a JavaScript error"
    );
}

// ============================================================
// Test 19: C++ Exception Propagation
// ============================================================

void testCppExceptionPropagation() {
    BindingFixture fixture;

    fixture.bindings().registerFunction(
        "nativeFail",
        [](
            const lamp::v8::V8NativeArguments&
        ) -> lamp::v8::V8Value {
            throw std::runtime_error(
                "native operation failed"
            );
        }
    );

    fixture.bindings().installAll();

    require(
        fixture.boolean(
            "(() => {"
            "  try {"
            "    nativeFail();"
            "    return false;"
            "  } catch (error) {"
            "    return error.message.includes("
            "      'native operation failed'"
            "    );"
            "  }"
            "})()"
        ),
        "C++ exception must become a JavaScript exception"
    );
}

// ============================================================
// Test 20: Recovery After Handler Exception
// ============================================================

void testRecoveryAfterHandlerException() {
    BindingFixture fixture;

    fixture.bindings().registerFunction(
        "nativeFail",
        [](
            const lamp::v8::V8NativeArguments&
        ) -> lamp::v8::V8Value {
            throw std::runtime_error(
                "expected failure"
            );
        }
    );

    fixture.bindings().registerFunction(
        "nativeAdd",
        makeAddHandler()
    );

    fixture.bindings().installAll();

    fixture.execute(
        "try { nativeFail(); } catch (error) {}"
    );

    require(
        fixture.number(
            "nativeAdd(20, 22)"
        ) == 42.0,
        "Native bindings must remain usable after an exception"
    );
}

// ============================================================
// Test 21: Independent Registries
// ============================================================

void testIndependentRegistries() {
    BindingFixture first;
    BindingFixture second;

    first.bindings().registerFunction(
        "nativeFirst",
        [](
            const lamp::v8::V8NativeArguments&
        ) -> lamp::v8::V8Value {
            return lamp::v8::V8Value(1.0);
        }
    );

    second.bindings().registerFunction(
        "nativeSecond",
        [](
            const lamp::v8::V8NativeArguments&
        ) -> lamp::v8::V8Value {
            return lamp::v8::V8Value(2.0);
        }
    );

    first.bindings().installAll();
    second.bindings().installAll();

    require(
        first.number("nativeFirst()") == 1.0,
        "First registry must execute its handler"
    );

    require(
        second.number("nativeSecond()") == 2.0,
        "Second registry must execute its handler"
    );

    require(
        first.boolean(
            "typeof nativeSecond === 'undefined'"
        ),
        "Second registry must not leak into first context"
    );

    require(
        second.boolean(
            "typeof nativeFirst === 'undefined'"
        ),
        "First registry must not leak into second context"
    );
}

// ============================================================
// Test 22: Registry Disposal
// ============================================================

void testRegistryDisposal() {
    BindingFixture fixture;

    fixture.bindings().registerFunction(
        "nativeAdd",
        makeAddHandler()
    );

    fixture.bindings().installAll();

    require(
        fixture.number("nativeAdd(1, 2)") == 3.0,
        "Installed binding must initially work"
    );

    fixture.bindings().dispose();

    require(
        fixture.boolean(
            "(() => {"
            "  try {"
            "    nativeAdd(1, 2);"
            "    return false;"
            "  } catch (error) {"
            "    return error instanceof Error;"
            "  }"
            "})()"
        ),
        "Disposed registry must reject callback execution"
    );
}

// ============================================================
// Test 23: Repeated Disposal
// ============================================================

void testRepeatedDisposal() {
    BindingFixture fixture;

    fixture.bindings().registerFunction(
        "nativeAdd",
        makeAddHandler()
    );

    fixture.bindings().installAll();

    fixture.bindings().dispose();
    fixture.bindings().dispose();

    require(
        true,
        "Repeated registry disposal must be safe"
    );
}

// ============================================================
// Test 24: Repeated Installation
// ============================================================

void testRepeatedInstallation() {
    BindingFixture fixture;

    fixture.bindings().registerFunction(
        "nativeAdd",
        makeAddHandler()
    );

    fixture.bindings().install("nativeAdd");

    require(
        fixture.bindings().isInstalled("nativeAdd"),
        "First installation must succeed"
    );

    // A second install must not create duplicate registry
    // entries. The implementation may either treat this
    // as idempotent or reject the repeated installation.

    try {
        fixture.bindings().install("nativeAdd");

    } catch (const lamp::v8::V8NativeBindingError&) {
        // Explicit duplicate-install rejection is acceptable.
    }

    require(
        fixture.bindings().size() == 1,
        "Repeated installation must not duplicate registration"
    );

    require(
        fixture.number("nativeAdd(20, 22)") == 42.0,
        "Previously installed binding must remain functional"
    );
}

// ============================================================
// Test 25: Zero-Argument Function
// ============================================================

void testZeroArgumentFunction() {
    BindingFixture fixture;

    fixture.bindings().registerFunction(
        "nativeAnswer",
        [](
            const lamp::v8::V8NativeArguments& args
        ) -> lamp::v8::V8Value {
            if (!args.empty()) {
                throw std::invalid_argument(
                    "Expected zero arguments"
                );
            }

            return lamp::v8::V8Value(42.0);
        }
    );

    fixture.bindings().installAll();

    require(
        fixture.number("nativeAnswer()") == 42.0,
        "Zero-argument native function must work"
    );
}

// ============================================================
// Test 26: Undefined Return
// ============================================================

void testUndefinedReturn() {
    BindingFixture fixture;

    fixture.bindings().registerFunction(
        "nativeNoop",
        [](
            const lamp::v8::V8NativeArguments&
        ) -> lamp::v8::V8Value {
            return lamp::v8::V8Value{};
        }
    );

    fixture.bindings().installAll();

    require(
        fixture.boolean(
            "nativeNoop() === undefined"
        ),
        "Default native value must convert to undefined"
    );
}

// ============================================================
// Test 27: Null Return
// ============================================================

void testNullReturn() {
    BindingFixture fixture;

    fixture.bindings().registerFunction(
        "nativeNull",
        [](
            const lamp::v8::V8NativeArguments&
        ) -> lamp::v8::V8Value {
            return lamp::v8::V8Value(
                lamp::v8::V8Null{}
            );
        }
    );

    fixture.bindings().installAll();

    require(
        fixture.boolean(
            "nativeNull() === null"
        ),
        "Native null must convert to JavaScript null"
    );
}

// ============================================================
// Test 28: Callback Argument Ordering
// ============================================================

void testArgumentOrdering() {
    BindingFixture fixture;

    fixture.bindings().registerFunction(
        "nativeSubtract",
        [](
            const lamp::v8::V8NativeArguments& args
        ) -> lamp::v8::V8Value {
            if (args.size() != 2) {
                throw std::invalid_argument(
                    "Expected two arguments"
                );
            }

            const double a =
                std::get<double>(args[0].data);

            const double b =
                std::get<double>(args[1].data);

            return lamp::v8::V8Value(a - b);
        }
    );

    fixture.bindings().installAll();

    require(
        fixture.number(
            "nativeSubtract(50, 8)"
        ) == 42.0,
        "Native arguments must preserve their order"
    );
}

} // namespace

// ============================================================
// Main
// ============================================================

int main() {
    auto& platform =
        lamp::v8::V8Platform::instance();

    try {
        platform.initialize();

    } catch (const std::exception& error) {
        std::cerr
            << "[FATAL] V8 initialization failed: "
            << error.what()
            << '\n';

        return 1;
    }

    TestRunner runner;

    // --------------------------------------------------------
    // Registration And Installation
    // --------------------------------------------------------

    runner.add(
        "Initial registry state",
        testInitialRegistryState
    );

    runner.add(
        "Function registration",
        testFunctionRegistration
    );

    runner.add(
        "Function installation",
        testFunctionInstallation
    );

    // --------------------------------------------------------
    // Primitive Arguments And Returns
    // --------------------------------------------------------

    runner.add(
        "Numeric arguments",
        testNumericArguments
    );

    runner.add(
        "Negative numbers",
        testNegativeNumbers
    );

    runner.add(
        "Boolean return",
        testBooleanReturn
    );

    runner.add(
        "String return",
        testStringReturn
    );

    runner.add(
        "String argument",
        testStringArgument
    );

    // --------------------------------------------------------
    // Multiple Functions
    // --------------------------------------------------------

    runner.add(
        "Multiple functions",
        testMultipleFunctions
    );

    runner.add(
        "Repeated invocation",
        testRepeatedInvocation
    );

    // --------------------------------------------------------
    // Structured Values
    // --------------------------------------------------------

    runner.add(
        "Array argument",
        testArrayArgument
    );

    runner.add(
        "Object argument",
        testObjectArgument
    );

    runner.add(
        "Array return",
        testArrayReturn
    );

    runner.add(
        "Object return",
        testObjectReturn
    );

    // --------------------------------------------------------
    // Registration Errors
    // --------------------------------------------------------

    runner.add(
        "Duplicate registration",
        testDuplicateRegistration
    );

    runner.add(
        "Missing function installation",
        testMissingFunctionInstallation
    );

    // --------------------------------------------------------
    // Argument And Handler Errors
    // --------------------------------------------------------

    runner.add(
        "Argument count limit",
        testArgumentCountLimit
    );

    runner.add(
        "Unsupported argument",
        testUnsupportedArgument
    );

    runner.add(
        "C++ exception propagation",
        testCppExceptionPropagation
    );

    runner.add(
        "Recovery after handler exception",
        testRecoveryAfterHandlerException
    );

    // --------------------------------------------------------
    // Registry Isolation And Lifecycle
    // --------------------------------------------------------

    runner.add(
        "Independent registries",
        testIndependentRegistries
    );

    runner.add(
        "Registry disposal",
        testRegistryDisposal
    );

    runner.add(
        "Repeated disposal",
        testRepeatedDisposal
    );

    runner.add(
        "Repeated installation",
        testRepeatedInstallation
    );

    // --------------------------------------------------------
    // Edge Cases
    // --------------------------------------------------------

    runner.add(
        "Zero-argument function",
        testZeroArgumentFunction
    );

    runner.add(
        "Undefined return",
        testUndefinedReturn
    );

    runner.add(
        "Null return",
        testNullReturn
    );

    runner.add(
        "Callback argument ordering",
        testArgumentOrdering
    );

    const int result = runner.run();

    try {
        platform.shutdown();

    } catch (const std::exception& error) {
        std::cerr
            << "[FATAL] V8 shutdown failed: "
            << error.what()
            << '\n';

        return 1;
    }

    return result;
}