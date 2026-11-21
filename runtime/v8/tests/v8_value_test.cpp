// ============================================================
// Lamp Framework - V8 Value Conversion Tests
// ============================================================
//
// File:
//   runtime/v8/tests/v8_value_test.cpp
//
// Purpose:
//   Validate JavaScript <-> C++ value conversion.
//
// Coverage:
//   - Primitive values
//   - Arrays and objects
//   - Nested structures
//   - Unicode strings
//   - Unsupported JavaScript values
//   - Circular references
//   - Conversion depth limits
//   - Container size limits
//   - String size limits
//   - Node count limits
//   - Round-trip conversion
//   - Converter recovery after errors
//
// Lifecycle:
//   V8Platform -> V8Isolate -> V8Context
//
// ============================================================

#include "v8/v8_platform.hpp"
#include "v8/v8_isolate.hpp"
#include "v8/v8_context.hpp"
#include "v8/v8_value.hpp"

#include <v8.h>

#include <cmath>
#include <cstddef>
#include <exception>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
            << " Lamp V8 Value Conversion Tests\n"
            << "========================================\n";

        for (const auto& test : tests_) {
            try {
                test.execute();
                ++passed;

                std::cout
                    << "[PASS] " << test.name << '\n';

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

void requireNumber(
    double actual,
    double expected,
    std::string_view message
) {
    if (std::isnan(expected)) {
        require(std::isnan(actual), message);
        return;
    }

    if (std::isinf(expected)) {
        require(
            std::isinf(actual) &&
                std::signbit(actual) == std::signbit(expected),
            message
        );
        return;
    }

    require(
        std::abs(actual - expected) <= 1e-9,
        message
    );
}

template <typename ExpectedException, typename Callback>
void requireThrows(
    Callback&& callback,
    std::string_view message
) {
    bool caught = false;

    try {
        std::forward<Callback>(callback)();

    } catch (const ExpectedException&) {
        caught = true;
    }

    require(caught, message);
}

// ============================================================
// Fixture
// ============================================================

class ValueFixture final {
public:
    ValueFixture()
        : isolate_(),
          context_(isolate_) {}

    template <typename Callback>
    void withScope(Callback&& callback) {
        context_.withScope(
            std::forward<Callback>(callback)
        );
    }

private:
    lamp::v8::V8Isolate isolate_;
    lamp::v8::V8Context context_;
};

// ============================================================
// JavaScript Helpers
// ============================================================

::v8::Local<::v8::String> makeString(
    ::v8::Isolate* isolate,
    std::string_view text
) {
    ::v8::Local<::v8::String> result;

    require(
        ::v8::String::NewFromUtf8(
            isolate,
            text.data(),
            ::v8::NewStringType::kNormal,
            static_cast<int>(text.size())
        ).ToLocal(&result),
        "Failed to create JavaScript string"
    );

    return result;
}

::v8::Local<::v8::Value> evaluate(
    ::v8::Isolate* isolate,
    ::v8::Local<::v8::Context> context,
    std::string_view source
) {
    auto scriptSource = makeString(isolate, source);

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

lamp::v8::V8Value convertFromSource(
    ::v8::Isolate* isolate,
    ::v8::Local<::v8::Context> context,
    lamp::v8::V8ValueConverter& converter,
    std::string_view source
) {
    return converter.fromJavaScript(
        isolate,
        context,
        evaluate(isolate, context, source)
    );
}

::v8::Local<::v8::Value> convertToJavaScript(
    ::v8::Isolate* isolate,
    ::v8::Local<::v8::Context> context,
    lamp::v8::V8ValueConverter& converter,
    const lamp::v8::V8Value& value
) {
    return converter.toJavaScript(
        isolate,
        context,
        value
    );
}

// ============================================================
// C++ Value Helpers
// ============================================================

lamp::v8::V8ValuePtr makeValue(
    lamp::v8::V8Value value
) {
    return std::make_shared<lamp::v8::V8Value>(
        std::move(value)
    );
}

const lamp::v8::V8Value& arrayElement(
    const lamp::v8::V8Value& value,
    std::size_t index
) {
    require(
        value.isArray(),
        "Expected array value"
    );

    const auto& array =
        std::get<lamp::v8::V8ValueArray>(value.data);

    require(
        index < array.size(),
        "Array index out of bounds"
    );

    require(
        array[index] != nullptr,
        "Array element pointer must not be null"
    );

    return *array[index];
}

const lamp::v8::V8Value& objectProperty(
    const lamp::v8::V8Value& value,
    const std::string& key
) {
    require(
        value.isObject(),
        "Expected object value"
    );

    const auto& object =
        std::get<lamp::v8::V8ValueObject>(value.data);

    const auto it = object.find(key);

    require(
        it != object.end(),
        "Object property not found"
    );

    require(
        it->second != nullptr,
        "Object property pointer must not be null"
    );

    return *it->second;
}

void requireValueNumber(
    const lamp::v8::V8Value& value,
    double expected
) {
    require(
        value.isNumber(),
        "Expected numeric V8Value"
    );

    requireNumber(
        std::get<double>(value.data),
        expected,
        "Numeric value mismatch"
    );
}

void requireValueString(
    const lamp::v8::V8Value& value,
    std::string_view expected
) {
    require(
        value.isString(),
        "Expected string V8Value"
    );

    require(
        std::get<std::string>(value.data) == expected,
        "String value mismatch"
    );
}

// ============================================================
// Test 1: Undefined
// ============================================================

void testUndefined() {
    ValueFixture fixture;

    fixture.withScope(
        [](::v8::Isolate* isolate,
           ::v8::Local<::v8::Context> context) {
            lamp::v8::V8ValueConverter converter;

            auto value = convertFromSource(
                isolate, context, converter, "undefined"
            );

            require(
                value.isUndefined(),
                "Undefined must be preserved"
            );

            auto js = convertToJavaScript(
                isolate, context, converter, value
            );

            require(
                js->IsUndefined(),
                "Round-trip result must be undefined"
            );
        }
    );
}

// ============================================================
// Test 2: Null
// ============================================================

void testNull() {
    ValueFixture fixture;

    fixture.withScope(
        [](::v8::Isolate* isolate,
           ::v8::Local<::v8::Context> context) {
            lamp::v8::V8ValueConverter converter;

            auto value = convertFromSource(
                isolate, context, converter, "null"
            );

            require(value.isNull(), "Expected null");

            auto js = convertToJavaScript(
                isolate, context, converter, value
            );

            require(js->IsNull(), "Round-trip must be null");
        }
    );
}

// ============================================================
// Test 3: Boolean True
// ============================================================

void testBooleanTrue() {
    ValueFixture fixture;

    fixture.withScope(
        [](::v8::Isolate* isolate,
           ::v8::Local<::v8::Context> context) {
            lamp::v8::V8ValueConverter converter;

            auto value = convertFromSource(
                isolate, context, converter, "true"
            );

            require(value.isBoolean(), "Expected boolean");

            require(
                std::get<bool>(value.data),
                "Expected true"
            );

            auto js = convertToJavaScript(
                isolate, context, converter, value
            );

            require(
                js->IsTrue(),
                "Round-trip must be true"
            );
        }
    );
}

// ============================================================
// Test 4: Boolean False
// ============================================================

void testBooleanFalse() {
    ValueFixture fixture;

    fixture.withScope(
        [](::v8::Isolate* isolate,
           ::v8::Local<::v8::Context> context) {
            lamp::v8::V8ValueConverter converter;

            auto value = convertFromSource(
                isolate, context, converter, "false"
            );

            require(value.isBoolean(), "Expected boolean");

            require(
                !std::get<bool>(value.data),
                "Expected false"
            );

            auto js = convertToJavaScript(
                isolate, context, converter, value
            );

            require(
                js->IsFalse(),
                "Round-trip must be false"
            );
        }
    );
}

// ============================================================
// Test 5: Numbers
// ============================================================

void testNumbers() {
    ValueFixture fixture;

    fixture.withScope(
        [](::v8::Isolate* isolate,
           ::v8::Local<::v8::Context> context) {
            lamp::v8::V8ValueConverter converter;

            const auto positive = convertFromSource(
                isolate, context, converter, "42"
            );

            const auto negative = convertFromSource(
                isolate, context, converter, "-17.5"
            );

            const auto zero = convertFromSource(
                isolate, context, converter, "0"
            );

            requireValueNumber(positive, 42.0);
            requireValueNumber(negative, -17.5);
            requireValueNumber(zero, 0.0);
        }
    );
}

// ============================================================
// Test 6: Non-Finite Numbers
// ============================================================

void testNonFiniteNumbers() {
    ValueFixture fixture;

    fixture.withScope(
        [](::v8::Isolate* isolate,
           ::v8::Local<::v8::Context> context) {
            lamp::v8::V8ValueConverter converter;

            const auto nan = convertFromSource(
                isolate, context, converter, "NaN"
            );

            const auto infinity = convertFromSource(
                isolate, context, converter, "Infinity"
            );

            requireValueNumber(
                nan,
                std::numeric_limits<double>::quiet_NaN()
            );

            requireValueNumber(
                infinity,
                std::numeric_limits<double>::infinity()
            );
        }
    );
}

// ============================================================
// Test 7: Strings
// ============================================================

void testStrings() {
    ValueFixture fixture;

    fixture.withScope(
        [](::v8::Isolate* isolate,
           ::v8::Local<::v8::Context> context) {
            lamp::v8::V8ValueConverter converter;

            auto value = convertFromSource(
                isolate,
                context,
                converter,
                "'Lamp Framework'"
            );

            requireValueString(
                value,
                "Lamp Framework"
            );
        }
    );
}

// ============================================================
// Test 8: Empty String
// ============================================================

void testEmptyString() {
    ValueFixture fixture;

    fixture.withScope(
        [](::v8::Isolate* isolate,
           ::v8::Local<::v8::Context> context) {
            lamp::v8::V8ValueConverter converter;

            auto value = convertFromSource(
                isolate, context, converter, "''"
            );

            requireValueString(value, "");
        }
    );
}

// ============================================================
// Test 9: Unicode String
// ============================================================

void testUnicodeString() {
    ValueFixture fixture;

    fixture.withScope(
        [](::v8::Isolate* isolate,
           ::v8::Local<::v8::Context> context) {
            lamp::v8::V8ValueConverter converter;

            constexpr std::string_view expected =
                "Lamp \xE2\x9C\xA8 \xE4\xB8\x96\xE7\x95\x8C";

            auto jsString = makeString(
                isolate,
                expected
            );

            auto value = converter.fromJavaScript(
                isolate,
                context,
                jsString
            );

            requireValueString(value, expected);

            auto roundTrip = converter.toJavaScript(
                isolate,
                context,
                value
            );

            ::v8::String::Utf8Value utf8(
                isolate,
                roundTrip
            );

            require(
                *utf8 != nullptr,
                "UTF-8 conversion must succeed"
            );

            require(
                std::string(*utf8, utf8.length()) == expected,
                "Unicode round-trip must preserve UTF-8"
            );
        }
    );
}

// ============================================================
// Test 10: Empty Array
// ============================================================

void testEmptyArray() {
    ValueFixture fixture;

    fixture.withScope(
        [](::v8::Isolate* isolate,
           ::v8::Local<::v8::Context> context) {
            lamp::v8::V8ValueConverter converter;

            auto value = convertFromSource(
                isolate, context, converter, "[]"
            );

            require(value.isArray(), "Expected array");

            require(
                std::get<lamp::v8::V8ValueArray>(
                    value.data
                ).empty(),
                "Array must be empty"
            );
        }
    );
}

// ============================================================
// Test 11: Primitive Array
// ============================================================

void testPrimitiveArray() {
    ValueFixture fixture;

    fixture.withScope(
        [](::v8::Isolate* isolate,
           ::v8::Local<::v8::Context> context) {
            lamp::v8::V8ValueConverter converter;

            auto value = convertFromSource(
                isolate,
                context,
                converter,
                "[1, 2, 3]"
            );

            requireValueNumber(
                arrayElement(value, 0), 1.0
            );

            requireValueNumber(
                arrayElement(value, 1), 2.0
            );

            requireValueNumber(
                arrayElement(value, 2), 3.0
            );
        }
    );
}

// ============================================================
// Test 12: Mixed Array
// ============================================================

void testMixedArray() {
    ValueFixture fixture;

    fixture.withScope(
        [](::v8::Isolate* isolate,
           ::v8::Local<::v8::Context> context) {
            lamp::v8::V8ValueConverter converter;

            auto value = convertFromSource(
                isolate,
                context,
                converter,
                "[42, 'lamp', true, null]"
            );

            requireValueNumber(
                arrayElement(value, 0), 42.0
            );

            requireValueString(
                arrayElement(value, 1), "lamp"
            );

            require(
                arrayElement(value, 2).isBoolean(),
                "Third element must be boolean"
            );

            require(
                arrayElement(value, 3).isNull(),
                "Fourth element must be null"
            );
        }
    );
}

// ============================================================
// Test 13: Empty Object
// ============================================================

void testEmptyObject() {
    ValueFixture fixture;

    fixture.withScope(
        [](::v8::Isolate* isolate,
           ::v8::Local<::v8::Context> context) {
            lamp::v8::V8ValueConverter converter;

            auto value = convertFromSource(
                isolate, context, converter, "({})"
            );

            require(value.isObject(), "Expected object");

            require(
                std::get<lamp::v8::V8ValueObject>(
                    value.data
                ).empty(),
                "Object must be empty"
            );
        }
    );
}

// ============================================================
// Test 14: Plain Object
// ============================================================

void testPlainObject() {
    ValueFixture fixture;

    fixture.withScope(
        [](::v8::Isolate* isolate,
           ::v8::Local<::v8::Context> context) {
            lamp::v8::V8ValueConverter converter;

            auto value = convertFromSource(
                isolate,
                context,
                converter,
                "({ name: 'lamp', version: 1 })"
            );

            requireValueString(
                objectProperty(value, "name"),
                "lamp"
            );

            requireValueNumber(
                objectProperty(value, "version"),
                1.0
            );
        }
    );
}

// ============================================================
// Test 15: Nested Structures
// ============================================================

void testNestedStructures() {
    ValueFixture fixture;

    fixture.withScope(
        [](::v8::Isolate* isolate,
           ::v8::Local<::v8::Context> context) {
            lamp::v8::V8ValueConverter converter;

            auto value = convertFromSource(
                isolate,
                context,
                converter,
                "({"
                "  app: { name: 'lamp' },"
                "  ports: [3000, 3001]"
                "})"
            );

            const auto& app =
                objectProperty(value, "app");

            const auto& ports =
                objectProperty(value, "ports");

            requireValueString(
                objectProperty(app, "name"),
                "lamp"
            );

            requireValueNumber(
                arrayElement(ports, 0),
                3000.0
            );

            requireValueNumber(
                arrayElement(ports, 1),
                3001.0
            );
        }
    );
}

// ============================================================
// Test 16: C++ to JavaScript Array
// ============================================================

void testCppToJavaScriptArray() {
    ValueFixture fixture;

    fixture.withScope(
        [](::v8::Isolate* isolate,
           ::v8::Local<::v8::Context> context) {
            lamp::v8::V8ValueArray array;

            array.push_back(
                makeValue(lamp::v8::V8Value(10.0))
            );

            array.push_back(
                makeValue(lamp::v8::V8Value(20.0))
            );

            lamp::v8::V8Value input(
                std::move(array)
            );

            lamp::v8::V8ValueConverter converter;

            auto js = converter.toJavaScript(
                isolate,
                context,
                input
            );

            require(
                js->IsArray(),
                "C++ array must become JavaScript array"
            );

            auto output = converter.fromJavaScript(
                isolate,
                context,
                js
            );

            requireValueNumber(
                arrayElement(output, 0),
                10.0
            );

            requireValueNumber(
                arrayElement(output, 1),
                20.0
            );
        }
    );
}

// ============================================================
// Test 17: C++ to JavaScript Object
// ============================================================

void testCppToJavaScriptObject() {
    ValueFixture fixture;

    fixture.withScope(
        [](::v8::Isolate* isolate,
           ::v8::Local<::v8::Context> context) {
            lamp::v8::V8ValueObject object;

            object.emplace(
                "framework",
                makeValue(
                    lamp::v8::V8Value(
                        std::string("lamp")
                    )
                )
            );

            object.emplace(
                "version",
                makeValue(
                    lamp::v8::V8Value(1.0)
                )
            );

            lamp::v8::V8Value input(
                std::move(object)
            );

            lamp::v8::V8ValueConverter converter;

            auto js = converter.toJavaScript(
                isolate,
                context,
                input
            );

            require(
                js->IsObject(),
                "C++ object must become JavaScript object"
            );

            auto output = converter.fromJavaScript(
                isolate,
                context,
                js
            );

            requireValueString(
                objectProperty(output, "framework"),
                "lamp"
            );

            requireValueNumber(
                objectProperty(output, "version"),
                1.0
            );
        }
    );
}

// ============================================================
// Test 18: Unsupported Function
// ============================================================

void testUnsupportedFunction() {
    ValueFixture fixture;

    fixture.withScope(
        [](::v8::Isolate* isolate,
           ::v8::Local<::v8::Context> context) {
            lamp::v8::V8ValueConverter converter;

            requireThrows<lamp::v8::V8ValueError>(
                [&]() {
                    (void)convertFromSource(
                        isolate,
                        context,
                        converter,
                        "(function () {})"
                    );
                },
                "Function conversion must be rejected"
            );
        }
    );
}

// ============================================================
// Test 19: Unsupported Symbol
// ============================================================

void testUnsupportedSymbol() {
    ValueFixture fixture;

    fixture.withScope(
        [](::v8::Isolate* isolate,
           ::v8::Local<::v8::Context> context) {
            lamp::v8::V8ValueConverter converter;

            requireThrows<lamp::v8::V8ValueError>(
                [&]() {
                    (void)convertFromSource(
                        isolate,
                        context,
                        converter,
                        "Symbol('lamp')"
                    );
                },
                "Symbol conversion must be rejected"
            );
        }
    );
}

// ============================================================
// Test 20: Unsupported BigInt
// ============================================================

void testUnsupportedBigInt() {
    ValueFixture fixture;

    fixture.withScope(
        [](::v8::Isolate* isolate,
           ::v8::Local<::v8::Context> context) {
            lamp::v8::V8ValueConverter converter;

            requireThrows<lamp::v8::V8ValueError>(
                [&]() {
                    (void)convertFromSource(
                        isolate,
                        context,
                        converter,
                        "12345678901234567890n"
                    );
                },
                "BigInt conversion must be rejected"
            );
        }
    );
}

// ============================================================
// Test 21: Unsupported Date
// ============================================================

void testUnsupportedDate() {
    ValueFixture fixture;

    fixture.withScope(
        [](::v8::Isolate* isolate,
           ::v8::Local<::v8::Context> context) {
            lamp::v8::V8ValueConverter converter;

            requireThrows<lamp::v8::V8ValueError>(
                [&]() {
                    (void)convertFromSource(
                        isolate,
                        context,
                        converter,
                        "new Date()"
                    );
                },
                "Date conversion must be rejected"
            );
        }
    );
}

// ============================================================
// Test 22: Circular JavaScript Object
// ============================================================

void testCircularJavaScriptObject() {
    ValueFixture fixture;

    fixture.withScope(
        [](::v8::Isolate* isolate,
           ::v8::Local<::v8::Context> context) {
            lamp::v8::V8ValueConverter converter;

            requireThrows<lamp::v8::V8ValueError>(
                [&]() {
                    (void)convertFromSource(
                        isolate,
                        context,
                        converter,
                        "(() => {"
                        "  const x = {};"
                        "  x.self = x;"
                        "  return x;"
                        "})()"
                    );
                },
                "Circular JavaScript object must be rejected"
            );
        }
    );
}

// ============================================================
// Test 23: Circular C++ Array
// ============================================================

void testCircularCppArray() {
    ValueFixture fixture;

    fixture.withScope(
        [](::v8::Isolate* isolate,
           ::v8::Local<::v8::Context> context) {
            lamp::v8::V8ValueConverter converter;

            auto root = std::make_shared<
                lamp::v8::V8Value
            >();

            lamp::v8::V8ValueArray array;
            array.push_back(root);

            root->data = std::move(array);

            requireThrows<lamp::v8::V8ValueError>(
                [&]() {
                    (void)converter.toJavaScript(
                        isolate,
                        context,
                        *root
                    );
                },
                "Circular C++ array must be rejected"
            );

            // Break the shared_ptr cycle before leaving
            // the test to avoid leaking the test fixture.

            root->data = lamp::v8::V8Undefined{};
        }
    );
}

// ============================================================
// Test 24: Maximum Depth
// ============================================================

void testMaximumDepth() {
    ValueFixture fixture;

    fixture.withScope(
        [](::v8::Isolate* isolate,
           ::v8::Local<::v8::Context> context) {
            lamp::v8::V8ValueConfig config{};
            config.maxDepth = 2;

            lamp::v8::V8ValueConverter converter(config);

            requireThrows<lamp::v8::V8ValueError>(
                [&]() {
                    (void)convertFromSource(
                        isolate,
                        context,
                        converter,
                        "({ a: { b: { c: 1 } } })"
                    );
                },
                "Excessive nesting must be rejected"
            );
        }
    );
}

// ============================================================
// Test 25: Maximum Container Entries
// ============================================================

void testMaximumContainerEntries() {
    ValueFixture fixture;

    fixture.withScope(
        [](::v8::Isolate* isolate,
           ::v8::Local<::v8::Context> context) {
            lamp::v8::V8ValueConfig config{};
            config.maxContainerEntries = 2;

            lamp::v8::V8ValueConverter converter(config);

            requireThrows<lamp::v8::V8ValueError>(
                [&]() {
                    (void)convertFromSource(
                        isolate,
                        context,
                        converter,
                        "[1, 2, 3]"
                    );
                },
                "Oversized array must be rejected"
            );
        }
    );
}

// ============================================================
// Test 26: Maximum String Bytes
// ============================================================

void testMaximumStringBytes() {
    ValueFixture fixture;

    fixture.withScope(
        [](::v8::Isolate* isolate,
           ::v8::Local<::v8::Context> context) {
            lamp::v8::V8ValueConfig config{};
            config.maxStringBytes = 4;

            lamp::v8::V8ValueConverter converter(config);

            requireThrows<lamp::v8::V8ValueError>(
                [&]() {
                    (void)convertFromSource(
                        isolate,
                        context,
                        converter,
                        "'lamp-framework'"
                    );
                },
                "Oversized string must be rejected"
            );
        }
    );
}

// ============================================================
// Test 27: Maximum Node Count
// ============================================================

void testMaximumNodeCount() {
    ValueFixture fixture;

    fixture.withScope(
        [](::v8::Isolate* isolate,
           ::v8::Local<::v8::Context> context) {
            lamp::v8::V8ValueConfig config{};
            config.maxNodes = 2;

            lamp::v8::V8ValueConverter converter(config);

            requireThrows<lamp::v8::V8ValueError>(
                [&]() {
                    (void)convertFromSource(
                        isolate,
                        context,
                        converter,
                        "[1, 2, 3]"
                    );
                },
                "Node limit must be enforced"
            );
        }
    );
}

// ============================================================
// Test 28: Converter Recovery
// ============================================================

void testConverterRecovery() {
    ValueFixture fixture;

    fixture.withScope(
        [](::v8::Isolate* isolate,
           ::v8::Local<::v8::Context> context) {
            lamp::v8::V8ValueConverter converter;

            requireThrows<lamp::v8::V8ValueError>(
                [&]() {
                    (void)convertFromSource(
                        isolate,
                        context,
                        converter,
                        "Symbol('unsupported')"
                    );
                },
                "Initial unsupported conversion must fail"
            );

            auto result = convertFromSource(
                isolate,
                context,
                converter,
                "42"
            );

            requireValueNumber(result, 42.0);
        }
    );
}

// ============================================================
// Test 29: Shared Child Without Cycle
// ============================================================

void testSharedChildWithoutCycle() {
    ValueFixture fixture;

    fixture.withScope(
        [](::v8::Isolate* isolate,
           ::v8::Local<::v8::Context> context) {
            auto shared = makeValue(
                lamp::v8::V8Value(42.0)
            );

            lamp::v8::V8ValueArray array;

            array.push_back(shared);
            array.push_back(shared);

            lamp::v8::V8Value input(
                std::move(array)
            );

            lamp::v8::V8ValueConverter converter;

            auto js = converter.toJavaScript(
                isolate,
                context,
                input
            );

            auto output = converter.fromJavaScript(
                isolate,
                context,
                js
            );

            requireValueNumber(
                arrayElement(output, 0),
                42.0
            );

            requireValueNumber(
                arrayElement(output, 1),
                42.0
            );
        }
    );
}

// ============================================================
// Test 30: Deep Round-Trip
// ============================================================

void testDeepRoundTrip() {
    ValueFixture fixture;

    fixture.withScope(
        [](::v8::Isolate* isolate,
           ::v8::Local<::v8::Context> context) {
            lamp::v8::V8ValueConverter converter;

            auto original = convertFromSource(
                isolate,
                context,
                converter,
                "({"
                "  framework: 'lamp',"
                "  enabled: true,"
                "  settings: {"
                "    port: 3000,"
                "    hosts: ['localhost', '127.0.0.1']"
                "  }"
                "})"
            );

            auto js = converter.toJavaScript(
                isolate,
                context,
                original
            );

            auto restored = converter.fromJavaScript(
                isolate,
                context,
                js
            );

            requireValueString(
                objectProperty(restored, "framework"),
                "lamp"
            );

            const auto& settings =
                objectProperty(restored, "settings");

            requireValueNumber(
                objectProperty(settings, "port"),
                3000.0
            );

            const auto& hosts =
                objectProperty(settings, "hosts");

            requireValueString(
                arrayElement(hosts, 0),
                "localhost"
            );

            requireValueString(
                arrayElement(hosts, 1),
                "127.0.0.1"
            );
        }
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
    // Primitive Values
    // --------------------------------------------------------

    runner.add("Undefined", testUndefined);
    runner.add("Null", testNull);
    runner.add("Boolean true", testBooleanTrue);
    runner.add("Boolean false", testBooleanFalse);
    runner.add("Numbers", testNumbers);
    runner.add("Non-finite numbers", testNonFiniteNumbers);
    runner.add("Strings", testStrings);
    runner.add("Empty string", testEmptyString);
    runner.add("Unicode string", testUnicodeString);

    // --------------------------------------------------------
    // Arrays And Objects
    // --------------------------------------------------------

    runner.add("Empty array", testEmptyArray);
    runner.add("Primitive array", testPrimitiveArray);
    runner.add("Mixed array", testMixedArray);
    runner.add("Empty object", testEmptyObject);
    runner.add("Plain object", testPlainObject);
    runner.add("Nested structures", testNestedStructures);

    // --------------------------------------------------------
    // C++ To JavaScript
    // --------------------------------------------------------

    runner.add(
        "C++ to JavaScript array",
        testCppToJavaScriptArray
    );

    runner.add(
        "C++ to JavaScript object",
        testCppToJavaScriptObject
    );

    // --------------------------------------------------------
    // Unsupported Types
    // --------------------------------------------------------

    runner.add(
        "Unsupported function",
        testUnsupportedFunction
    );

    runner.add(
        "Unsupported symbol",
        testUnsupportedSymbol
    );

    runner.add(
        "Unsupported BigInt",
        testUnsupportedBigInt
    );

    runner.add(
        "Unsupported Date",
        testUnsupportedDate
    );

    // --------------------------------------------------------
    // Circular References
    // --------------------------------------------------------

    runner.add(
        "Circular JavaScript object",
        testCircularJavaScriptObject
    );

    runner.add(
        "Circular C++ array",
        testCircularCppArray
    );

    // --------------------------------------------------------
    // Conversion Limits
    // --------------------------------------------------------

    runner.add(
        "Maximum depth",
        testMaximumDepth
    );

    runner.add(
        "Maximum container entries",
        testMaximumContainerEntries
    );

    runner.add(
        "Maximum string bytes",
        testMaximumStringBytes
    );

    runner.add(
        "Maximum node count",
        testMaximumNodeCount
    );

    // --------------------------------------------------------
    // Recovery And Round Trips
    // --------------------------------------------------------

    runner.add(
        "Converter recovery",
        testConverterRecovery
    );

    runner.add(
        "Shared child without cycle",
        testSharedChildWithoutCycle
    );

    runner.add(
        "Deep round-trip",
        testDeepRoundTrip
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