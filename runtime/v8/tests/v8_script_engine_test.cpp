// ============================================================
// Lamp Framework - V8 Script Engine Tests
// ============================================================
//
// File:
//   runtime/v8/tests/v8_script_engine_test.cpp
//
// Purpose:
//
//   Validate classic JavaScript execution through
//   Lamp's V8ScriptEngine.
//
// Coverage:
//
//   1. Integer arithmetic
//   2. Floating-point arithmetic
//   3. Boolean true
//   4. Boolean false
//   5. String result
//   6. Empty string
//   7. Undefined result
//   8. Null result
//   9. Negative numbers
//  10. Zero
//  11. JavaScript expressions
//  12. Multiple statements
//  13. Variable declarations
//  14. Function declarations
//  15. Persistent global state
//  16. Independent contexts
//  17. Syntax error classification
//  18. Runtime error classification
//  19. Error message availability
//  20. Resource name preservation
//  21. Error line information
//  22. Error stack information
//  23. Recovery after syntax error
//  24. Recovery after runtime error
//  25. Unsupported object result
//  26. Unsupported array result
//  27. executeVoid success
//  28. executeVoid error propagation
//  29. Empty source
//  30. Invalid context rejection
//
// ============================================================

#include "v8/v8_platform.hpp"
#include "v8/v8_isolate.hpp"
#include "v8/v8_context.hpp"
#include "v8/v8_script_engine.hpp"

#include <cmath>
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

namespace
{

    // ============================================================
    // Test Infrastructure
    // ============================================================

    class TestFailure final : public std::runtime_error
    {
    public:
        explicit TestFailure(std::string message)
            : std::runtime_error(std::move(message)) {}
    };

    struct TestCase final
    {
        std::string name;
        std::function<void()> execute;
    };

    class TestRunner final
    {
    public:
        void add(
            std::string name,
            std::function<void()> test)
        {
            tests_.push_back({std::move(name),
                              std::move(test)});
        }

        [[nodiscard]]
        int run() const
        {
            std::size_t passed = 0;
            std::size_t failed = 0;

            std::cout
                << "\n"
                << "========================================\n"
                << " Lamp V8 Script Engine Tests\n"
                << "========================================\n";

            for (const auto &test : tests_)
            {
                try
                {
                    test.execute();

                    ++passed;

                    std::cout
                        << "[PASS] "
                        << test.name
                        << '\n';
                }
                catch (const std::exception &error)
                {
                    ++failed;

                    std::cerr
                        << "[FAIL] "
                        << test.name
                        << ": "
                        << error.what()
                        << '\n';
                }
                catch (...)
                {
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
        std::string_view message)
    {
        if (!condition)
        {
            throw TestFailure(std::string(message));
        }
    }

    void requireEqual(
        double actual,
        double expected,
        std::string_view message)
    {
        constexpr double epsilon = 1e-9;

        if (std::isnan(expected))
        {
            require(
                std::isnan(actual),
                message);

            return;
        }

        require(
            std::abs(actual - expected) <= epsilon,
            message);
    }

    template <typename ExpectedException, typename Callback>
    ExpectedException requireThrows(
        Callback &&callback,
        std::string_view message)
    {
        try
        {
            std::forward<Callback>(callback)();
        }
        catch (const ExpectedException &error)
        {
            return error;
        }

        throw TestFailure(std::string(message));
    }

    // ============================================================
    // Test Fixture
    // ============================================================
    //
    // Each fixture owns:
    //
    //   V8Isolate
    //       |
    //       +-- V8Context
    //               |
    //               +-- V8ScriptEngine
    //
    // Destruction occurs in reverse order.
    //
    // ============================================================

    class ScriptFixture final
    {
    public:
        ScriptFixture()
            : isolate_(),
              context_(isolate_),
              engine_(context_) {}

        ScriptFixture(const ScriptFixture &) = delete;
        ScriptFixture &operator=(const ScriptFixture &) = delete;

        [[nodiscard]]
        lamp::v8::V8ScriptResult execute(
            std::string_view source,
            const lamp::v8::V8ScriptConfig &config = {})
        {
            return engine_.execute(source, config);
        }

        void executeVoid(
            std::string_view source,
            const lamp::v8::V8ScriptConfig &config = {})
        {
            engine_.executeVoid(source, config);
        }

    private:
        lamp::v8::V8Isolate isolate_;
        lamp::v8::V8Context context_;
        lamp::v8::V8ScriptEngine engine_;
    };

    // ============================================================
    // Result Assertions
    // ============================================================

    void requireNumber(
        const lamp::v8::V8ScriptResult &result,
        double expected)
    {
        require(
            result.isNumber(),
            "Expected JavaScript number result");

        requireEqual(
            result.numberValue,
            expected,
            "JavaScript number does not match expected value");
    }

    void requireBoolean(
        const lamp::v8::V8ScriptResult &result,
        bool expected)
    {
        require(
            result.isBoolean(),
            "Expected JavaScript boolean result");

        require(
            result.booleanValue == expected,
            "JavaScript boolean does not match expected value");
    }

    void requireString(
        const lamp::v8::V8ScriptResult &result,
        std::string_view expected)
    {
        require(
            result.isString(),
            "Expected JavaScript string result");

        require(
            result.stringValue == expected,
            "JavaScript string does not match expected value");
    }

    // ============================================================
    // Test 1: Integer Arithmetic
    // ============================================================

    void testIntegerArithmetic()
    {
        ScriptFixture fixture;

        requireNumber(
            fixture.execute("20 + 22"),
            42.0);
    }

    // ============================================================
    // Test 2: Floating-Point Arithmetic
    // ============================================================

    void testFloatingPointArithmetic()
    {
        ScriptFixture fixture;

        requireNumber(
            fixture.execute("1.5 + 2.25"),
            3.75);
    }

    // ============================================================
    // Test 3: Boolean True
    // ============================================================

    void testBooleanTrue()
    {
        ScriptFixture fixture;

        requireBoolean(
            fixture.execute("10 > 5"),
            true);
    }

    // ============================================================
    // Test 4: Boolean False
    // ============================================================

    void testBooleanFalse()
    {
        ScriptFixture fixture;

        requireBoolean(
            fixture.execute("10 < 5"),
            false);
    }

    // ============================================================
    // Test 5: String Result
    // ============================================================

    void testStringResult()
    {
        ScriptFixture fixture;

        requireString(
            fixture.execute("'Lamp Framework'"),
            "Lamp Framework");
    }

    // ============================================================
    // Test 6: Empty String
    // ============================================================

    void testEmptyString()
    {
        ScriptFixture fixture;

        requireString(
            fixture.execute("''"),
            "");
    }

    // ============================================================
    // Test 7: Undefined Result
    // ============================================================

    void testUndefinedResult()
    {
        ScriptFixture fixture;

        const auto result = fixture.execute(
            "undefined");

        require(
            result.isUndefined(),
            "Expected undefined result");
    }

    // ============================================================
    // Test 8: Null Result
    // ============================================================

    void testNullResult()
    {
        ScriptFixture fixture;

        const auto result = fixture.execute(
            "null");

        require(
            result.isNull(),
            "Expected null result");
    }

    // ============================================================
    // Test 9: Negative Numbers
    // ============================================================

    void testNegativeNumbers()
    {
        ScriptFixture fixture;

        requireNumber(
            fixture.execute("-100 + 25"),
            -75.0);
    }

    // ============================================================
    // Test 10: Zero
    // ============================================================

    void testZero()
    {
        ScriptFixture fixture;

        requireNumber(
            fixture.execute("100 - 100"),
            0.0);
    }

    // ============================================================
    // Test 11: JavaScript Expressions
    // ============================================================

    void testExpressions()
    {
        ScriptFixture fixture;

        requireNumber(
            fixture.execute(
                "(10 + 5) * 2 / 3"),
            10.0);
    }

    // ============================================================
    // Test 12: Multiple Statements
    // ============================================================

    void testMultipleStatements()
    {
        ScriptFixture fixture;

        requireNumber(
            fixture.execute(
                "let a = 10;"
                "let b = 20;"
                "a + b;"),
            30.0);
    }

    // ============================================================
    // Test 13: Variable Declarations
    // ============================================================

    void testVariableDeclarations()
    {
        ScriptFixture fixture;

        requireNumber(
            fixture.execute(
                "const value = 42;"
                "value;"),
            42.0);
    }

    // ============================================================
    // Test 14: Function Declarations
    // ============================================================

    void testFunctionDeclarations()
    {
        ScriptFixture fixture;

        requireNumber(
            fixture.execute(
                "function add(a, b) {"
                "    return a + b;"
                "}"
                "add(20, 22);"),
            42.0);
    }

    // ============================================================
    // Test 15: Persistent Global State
    // ============================================================

    void testPersistentGlobalState()
    {
        ScriptFixture fixture;

        requireNumber(
            fixture.execute(
                "globalThis.counter = 10;"
                "counter;"),
            10.0);

        requireNumber(
            fixture.execute(
                "globalThis.counter += 5;"
                "counter;"),
            15.0);
    }

    // ============================================================
    // Test 16: Independent Contexts
    // ============================================================

    void testIndependentContexts()
    {
        ScriptFixture first;
        ScriptFixture second;

        requireNumber(
            first.execute(
                "globalThis.value = 100;"
                "value;"),
            100.0);

        requireBoolean(
            second.execute(
                "typeof globalThis.value === 'undefined'"),
            true);
    }

    // ============================================================
    // Test 17: Syntax Error Classification
    // ============================================================

    void testSyntaxErrorClassification()
    {
        ScriptFixture fixture;

        const auto error =
            requireThrows<lamp::v8::V8ScriptError>(
                [&fixture]()
                {
                    (void)fixture.execute(
                        "const = ;");
                },
                "Invalid syntax must throw V8ScriptError");

        require(
            error.type() ==
                lamp::v8::V8ScriptErrorType::Compilation,
            "Syntax error must be classified as compilation error");
    }

    // ============================================================
    // Test 18: Runtime Error Classification
    // ============================================================

    void testRuntimeErrorClassification()
    {
        ScriptFixture fixture;

        const auto error =
            requireThrows<lamp::v8::V8ScriptError>(
                [&fixture]()
                {
                    (void)fixture.execute(
                        "throw new Error('runtime failure');");
                },
                "Thrown JavaScript error must be reported");

        require(
            error.type() ==
                lamp::v8::V8ScriptErrorType::Runtime,
            "Thrown error must be classified as runtime error");
    }

    // ============================================================
    // Test 19: Error Message Availability
    // ============================================================

    void testErrorMessage()
    {
        ScriptFixture fixture;

        const auto error =
            requireThrows<lamp::v8::V8ScriptError>(
                [&fixture]()
                {
                    (void)fixture.execute(
                        "throw new Error('lamp failure');");
                },
                "JavaScript exception must be reported");

        require(
            std::string(error.what()).find("lamp failure") !=
                std::string::npos,
            "Error message must contain JavaScript exception text");
    }

    // ============================================================
    // Test 20: Resource Name Preservation
    // ============================================================

    void testResourceName()
    {
        ScriptFixture fixture;

        lamp::v8::V8ScriptConfig config{};

        config.resourceName = "lamp-test-script.js";

        const auto error =
            requireThrows<lamp::v8::V8ScriptError>(
                [&fixture, &config]()
                {
                    (void)fixture.execute(
                        "throw new Error('resource test');",
                        config);
                },
                "Script must report runtime exception");

        require(
            error.resourceName() == "lamp-test-script.js",
            "Error must preserve script resource name");
    }

    // ============================================================
    // Test 21: Error Line Information
    // ============================================================

    void testErrorLineInformation()
    {
        ScriptFixture fixture;

        const auto error =
            requireThrows<lamp::v8::V8ScriptError>(
                [&fixture]()
                {
                    (void)fixture.execute(
                        "const value = 10;\n"
                        "throw new Error('line test');");
                },
                "Runtime error must be reported");

        require(
            error.line() == 2,
            "Error must identify the second source line");
    }

    // ============================================================
    // Test 22: Error Stack Information
    // ============================================================

    void testErrorStackInformation()
    {
        ScriptFixture fixture;

        const auto error =
            requireThrows<lamp::v8::V8ScriptError>(
                [&fixture]()
                {
                    (void)fixture.execute(
                        "function fail() {\n"
                        "    throw new Error('stack test');\n"
                        "}\n"
                        "fail();");
                },
                "Runtime error must be reported");

        require(
            !error.stackTrace().empty(),
            "JavaScript error must include stack information");
    }

    // ============================================================
    // Test 23: Recovery After Syntax Error
    // ============================================================

    void testRecoveryAfterSyntaxError()
    {
        ScriptFixture fixture;

        (void)requireThrows<lamp::v8::V8ScriptError>(
            [&fixture]()
            {
                (void)fixture.execute(
                    "function (");
            },
            "Syntax error must be reported");

        requireNumber(
            fixture.execute("20 + 22"),
            42.0);
    }

    // ============================================================
    // Test 24: Recovery After Runtime Error
    // ============================================================

    void testRecoveryAfterRuntimeError()
    {
        ScriptFixture fixture;

        (void)requireThrows<lamp::v8::V8ScriptError>(
            [&fixture]()
            {
                (void)fixture.execute(
                    "throw new Error('temporary failure');");
            },
            "Runtime error must be reported");

        requireNumber(
            fixture.execute("6 * 7"),
            42.0);
    }

    // ============================================================
    // Test 25: Unsupported Object Result
    // ============================================================

    void testUnsupportedObjectResult()
    {
        ScriptFixture fixture;

        const auto result = fixture.execute(
            "({ value: 42 })");

        require(
            !result.isSupported(),
            "Structured object result is not supported "
            "by the primitive script result API");
    }

    // ============================================================
    // Test 26: Unsupported Array Result
    // ============================================================

    void testUnsupportedArrayResult()
    {
        ScriptFixture fixture;

        const auto result = fixture.execute(
            "[1, 2, 3]");

        require(
            !result.isSupported(),
            "Array result is not supported "
            "by the primitive script result API");
    }

    // ============================================================
    // Test 27: executeVoid Success
    // ============================================================

    void testExecuteVoidSuccess()
    {
        ScriptFixture fixture;

        fixture.executeVoid(
            "globalThis.answer = 42;");

        requireNumber(
            fixture.execute("globalThis.answer"),
            42.0);
    }

    // ============================================================
    // Test 28: executeVoid Error Propagation
    // ============================================================

    void testExecuteVoidErrorPropagation()
    {
        ScriptFixture fixture;

        const auto error =
            requireThrows<lamp::v8::V8ScriptError>(
                [&fixture]()
                {
                    fixture.executeVoid(
                        "throw new Error('void failure');");
                },
                "executeVoid must propagate script errors");

        require(
            error.type() ==
                lamp::v8::V8ScriptErrorType::Runtime,
            "executeVoid must preserve runtime error classification");
    }

    // ============================================================
    // Test 29: Empty Source
    // ============================================================

    void testEmptySource()
    {
        ScriptFixture fixture;

        const auto result = fixture.execute("");

        require(
            result.isUndefined(),
            "Empty script must evaluate to undefined");
    }

    // ============================================================
    // Test 30: Invalid Context Rejection
    // ============================================================

    void testInvalidContextRejection()
    {
        lamp::v8::V8Isolate isolate;
        lamp::v8::V8Context context(isolate);

        context.dispose();

        require(
            !context.isValid(),
            "Context must be disposed before testing rejection");

        // The script engine should reject a disposed context.
        //
        // The exact failure may occur during engine construction
        // or during script execution, depending on implementation.

        bool rejected = false;

        try
        {
            lamp::v8::V8ScriptEngine engine(context);

            (void)engine.execute("1 + 1");
        }
        catch (const lamp::v8::V8ScriptError &)
        {
            rejected = true;
        }
        catch (const lamp::v8::V8ContextError &)
        {
            rejected = true;
        }

        require(
            rejected,
            "Script engine must reject a disposed context");
    }

} // namespace

// ============================================================
// Main
// ============================================================

int main()
{
    auto &platform =
        lamp::v8::V8Platform::instance();

    try
    {
        platform.initialize();
    }
    catch (const std::exception &error)
    {
        std::cerr
            << "[FATAL] V8 platform initialization failed: "
            << error.what()
            << '\n';

        return 1;
    }

    TestRunner runner;

    // --------------------------------------------------------
    // Primitive Results
    // --------------------------------------------------------

    runner.add(
        "Integer arithmetic",
        testIntegerArithmetic);

    runner.add(
        "Floating-point arithmetic",
        testFloatingPointArithmetic);

    runner.add(
        "Boolean true",
        testBooleanTrue);

    runner.add(
        "Boolean false",
        testBooleanFalse);

    runner.add(
        "String result",
        testStringResult);

    runner.add(
        "Empty string",
        testEmptyString);

    runner.add(
        "Undefined result",
        testUndefinedResult);

    runner.add(
        "Null result",
        testNullResult);

    runner.add(
        "Negative numbers",
        testNegativeNumbers);

    runner.add(
        "Zero",
        testZero);

    // --------------------------------------------------------
    // JavaScript Execution
    // --------------------------------------------------------

    runner.add(
        "JavaScript expressions",
        testExpressions);

    runner.add(
        "Multiple statements",
        testMultipleStatements);

    runner.add(
        "Variable declarations",
        testVariableDeclarations);

    runner.add(
        "Function declarations",
        testFunctionDeclarations);

    runner.add(
        "Persistent global state",
        testPersistentGlobalState);

    runner.add(
        "Independent contexts",
        testIndependentContexts);

    // --------------------------------------------------------
    // Error Handling
    // --------------------------------------------------------

    runner.add(
        "Syntax error classification",
        testSyntaxErrorClassification);

    runner.add(
        "Runtime error classification",
        testRuntimeErrorClassification);

    runner.add(
        "Error message availability",
        testErrorMessage);

    runner.add(
        "Resource name preservation",
        testResourceName);

    runner.add(
        "Error line information",
        testErrorLineInformation);

    runner.add(
        "Error stack information",
        testErrorStackInformation);

    runner.add(
        "Recovery after syntax error",
        testRecoveryAfterSyntaxError);

    runner.add(
        "Recovery after runtime error",
        testRecoveryAfterRuntimeError);

    // --------------------------------------------------------
    // Result Conversion
    // --------------------------------------------------------

    runner.add(
        "Unsupported object result",
        testUnsupportedObjectResult);

    runner.add(
        "Unsupported array result",
        testUnsupportedArrayResult);

    // --------------------------------------------------------
    // Void Execution
    // --------------------------------------------------------

    runner.add(
        "executeVoid success",
        testExecuteVoidSuccess);

    runner.add(
        "executeVoid error propagation",
        testExecuteVoidErrorPropagation);

    // --------------------------------------------------------
    // Edge Cases
    // --------------------------------------------------------

    runner.add(
        "Empty source",
        testEmptySource);

    runner.add(
        "Invalid context rejection",
        testInvalidContextRejection);

    const int result = runner.run();

    try
    {
        platform.shutdown();
    }
    catch (const std::exception &error)
    {
        std::cerr
            << "[FATAL] V8 platform shutdown failed: "
            << error.what()
            << '\n';

        return 1;
    }

    return result;
}