// ============================================================
// Lamp Framework - V8 Runtime Integration Tests
// ============================================================
//
// File:
//   runtime/v8/tests/v8_runtime_test.cpp
//
// Purpose:
//   Validate the integrated Lamp V8 runtime.
//
// Coverage:
//   - Runtime construction and configuration
//   - Initialization and lifecycle states
//   - JavaScript execution
//   - Persistent JavaScript state
//   - Script error propagation
//   - Native function registration
//   - Native argument and return conversion
//   - ES module execution
//   - Relative imports and module caching
//   - Runtime context access
//   - Shutdown and invalid-state handling
//   - Resource cleanup
//
// Lifecycle:
//
//   V8Platform (process-wide)
//       |
//       +-- V8Runtime
//               |
//               +-- V8Isolate
//               +-- V8Context
//               +-- V8ScriptEngine
//               +-- V8NativeBinding
//               +-- V8ModuleLoader
//       |
//       +-- Destroy runtime
//       |
//   V8Platform::shutdown()
//
// Important:
//
//   This test executable initializes the V8 platform
//   once and shuts it down once.
//
//   Individual runtime instances do not own the
//   process-wide platform shutdown.
//
// ============================================================

#include "v8/v8_platform.hpp"
#include "v8/v8_runtime.hpp"

#include <v8.h>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace fs = std::filesystem;

using lamp::v8::V8Runtime;
using lamp::v8::V8RuntimeConfig;
using lamp::v8::V8RuntimeError;
using lamp::v8::V8RuntimeState;
using lamp::v8::V8ScriptError;
using lamp::v8::V8ScriptErrorType;
using lamp::v8::V8ScriptResult;
using lamp::v8::V8NativeArguments;
using lamp::v8::V8Value;

// ============================================================
// Test Framework
// ============================================================

class TestFailure final : public std::runtime_error {
public:
    explicit TestFailure(std::string message)
        : std::runtime_error(std::move(message)) {}
};

void require(
    bool condition,
    std::string_view message
) {
    if (!condition) {
        throw TestFailure(std::string(message));
    }
}

void requireEqual(
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
ExpectedException requireThrows(
    Callback&& callback,
    std::string_view message
) {
    try {
        std::forward<Callback>(callback)();

    } catch (const ExpectedException& error) {
        return error;
    }

    throw TestFailure(std::string(message));
}

struct TestCase final {
    std::string name;
    std::function<void()> execute;
};

class TestRunner final {
public:
    void add(
        std::string name,
        std::function<void()> callback
    ) {
        tests_.push_back({
            std::move(name),
            std::move(callback)
        });
    }

    [[nodiscard]]
    int run() const {
        std::size_t passed = 0;
        std::size_t failed = 0;

        std::cout
            << "\n"
            << "========================================\n"
            << " Lamp V8 Runtime Integration Tests\n"
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
// Temporary Directory
// ============================================================

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        const auto base = fs::temp_directory_path();

        const auto timestamp =
            std::chrono::steady_clock::now()
                .time_since_epoch()
                .count();

        for (std::size_t attempt = 0;
             attempt < 1024;
             ++attempt) {

            const auto candidate =
                base /
                (
                    "lamp-v8-runtime-" +
                    std::to_string(timestamp) +
                    "-" +
                    std::to_string(attempt)
                );

            std::error_code error;

            if (fs::create_directory(candidate, error)) {
                root_ = candidate;
                return;
            }

            if (error &&
                error != std::errc::file_exists) {
                throw TestFailure(
                    "Temporary directory creation failed: " +
                    error.message()
                );
            }
        }

        throw TestFailure(
            "Unable to create unique temporary directory"
        );
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(
        const TemporaryDirectory&
    ) = delete;

    ~TemporaryDirectory() {
        if (!root_.empty()) {
            std::error_code error;
            fs::remove_all(root_, error);
        }
    }

    [[nodiscard]]
    const fs::path& root() const noexcept {
        return root_;
    }

    [[nodiscard]]
    fs::path write(
        const fs::path& relativePath,
        std::string_view content
    ) const {
        require(
            !relativePath.is_absolute(),
            "Fixture module path must be relative"
        );

        const auto destination =
            root_ / relativePath;

        fs::create_directories(
            destination.parent_path()
        );

        std::ofstream output(
            destination,
            std::ios::binary | std::ios::trunc
        );

        require(
            output.is_open(),
            "Unable to create module fixture"
        );

        output.write(
            content.data(),
            static_cast<std::streamsize>(
                content.size()
            )
        );

        output.flush();

        require(
            output.good(),
            "Unable to write module fixture"
        );

        return destination;
    }

private:
    fs::path root_;
};

// ============================================================
// Runtime Fixture
// ============================================================
//
// The temporary directory outlives V8Runtime.
//
// ============================================================

class RuntimeFixture final {
public:
    explicit RuntimeFixture(
        std::function<void(V8RuntimeConfig&)> configure = {}
    )
        : directory_() {

        V8RuntimeConfig config{};

        config.modules.rootDirectory =
            directory_.root();

        config.modules.restrictToRoot = true;

        if (configure) {
            configure(config);
        }

        runtime_ =
            std::make_unique<V8Runtime>(
                std::move(config)
            );
    }

    V8Runtime& runtime() {
        return *runtime_;
    }

    [[nodiscard]]
    fs::path write(
        const fs::path& relativePath,
        std::string_view content
    ) const {
        return directory_.write(
            relativePath,
            content
        );
    }

    [[nodiscard]]
    const fs::path& root() const noexcept {
        return directory_.root();
    }

    void initialize() {
        runtime_->initialize();
    }

    [[nodiscard]]
    V8ScriptResult execute(
        std::string_view source
    ) {
        return runtime_->execute(source);
    }

    void executeVoid(
        std::string_view source
    ) {
        runtime_->executeVoid(source);
    }

    void executeModule(
        const fs::path& entry
    ) {
        runtime_->executeModule(entry);
    }

private:
    TemporaryDirectory directory_;
    std::unique_ptr<V8Runtime> runtime_;
};

// ============================================================
// Script Result Assertions
// ============================================================

void requireNumber(
    const V8ScriptResult& result,
    double expected
) {
    require(
        result.isNumber(),
        "Expected numeric script result"
    );

    requireEqual(
        result.numberValue,
        expected,
        "Numeric script result mismatch"
    );
}

void requireBoolean(
    const V8ScriptResult& result,
    bool expected
) {
    require(
        result.isBoolean(),
        "Expected boolean script result"
    );

    require(
        result.booleanValue == expected,
        "Boolean script result mismatch"
    );
}

void requireString(
    const V8ScriptResult& result,
    std::string_view expected
) {
    require(
        result.isString(),
        "Expected string script result"
    );

    require(
        result.stringValue == expected,
        "String script result mismatch"
    );
}

// ============================================================
// Native Handler Helpers
// ============================================================

lamp::v8::V8NativeHandler makeAddHandler() {
    return [](
        const V8NativeArguments& args
    ) -> V8Value {
        if (args.size() != 2 ||
            !args[0].isNumber() ||
            !args[1].isNumber()) {
            throw std::invalid_argument(
                "nativeAdd expects two numbers"
            );
        }

        return V8Value(
            std::get<double>(args[0].data) +
            std::get<double>(args[1].data)
        );
    };
}

// ============================================================
// Test 1: Initial Runtime State
// ============================================================

void testInitialRuntimeState() {
    RuntimeFixture fixture;

    require(
        fixture.runtime().state() ==
            V8RuntimeState::Created,
        "New runtime must start in Created state"
    );

    require(
        !fixture.runtime().isRunning(),
        "New runtime must not be running"
    );
}

// ============================================================
// Test 2: Default Configuration
// ============================================================

void testDefaultConfiguration() {
    RuntimeFixture fixture;

    const auto& config =
        fixture.runtime().config();

    require(
        config.context.name == "lamp",
        "Default context name must be lamp"
    );

    require(
        config.modules.restrictToRoot,
        "Fixture must enable root restriction"
    );
}

// ============================================================
// Test 3: Custom Configuration
// ============================================================

void testCustomConfiguration() {
    RuntimeFixture fixture(
        [](V8RuntimeConfig& config) {
            config.context.name =
                "lamp-runtime-test";

            config.isolate.initialHeapSizeMB = 16;
            config.isolate.maxHeapSizeMB = 128;
        }
    );

    const auto& config =
        fixture.runtime().config();

    require(
        config.context.name == "lamp-runtime-test",
        "Custom context name must be preserved"
    );

    require(
        config.isolate.initialHeapSizeMB == 16,
        "Initial heap configuration must be preserved"
    );

    require(
        config.isolate.maxHeapSizeMB == 128,
        "Maximum heap configuration must be preserved"
    );
}

// ============================================================
// Test 4: Runtime Initialization
// ============================================================

void testRuntimeInitialization() {
    RuntimeFixture fixture;

    fixture.initialize();

    require(
        fixture.runtime().state() ==
            V8RuntimeState::Running,
        "Initialized runtime must enter Running state"
    );

    require(
        fixture.runtime().isRunning(),
        "Initialized runtime must report running"
    );
}

// ============================================================
// Test 5: Repeated Initialization
// ============================================================

void testRepeatedInitialization() {
    RuntimeFixture fixture;

    fixture.initialize();

    // Repeated initialization should not create
    // duplicate runtime components.

    fixture.initialize();

    require(
        fixture.runtime().isRunning(),
        "Repeated initialization must preserve Running state"
    );
}

// ============================================================
// Test 6: Integer Arithmetic
// ============================================================

void testIntegerArithmetic() {
    RuntimeFixture fixture;

    fixture.initialize();

    requireNumber(
        fixture.execute("20 + 22"),
        42.0
    );
}

// ============================================================
// Test 7: Floating-Point Arithmetic
// ============================================================

void testFloatingPointArithmetic() {
    RuntimeFixture fixture;

    fixture.initialize();

    requireNumber(
        fixture.execute("1.5 + 2.25"),
        3.75
    );
}

// ============================================================
// Test 8: Boolean Results
// ============================================================

void testBooleanResults() {
    RuntimeFixture fixture;

    fixture.initialize();

    requireBoolean(
        fixture.execute("10 > 5"),
        true
    );

    requireBoolean(
        fixture.execute("10 < 5"),
        false
    );
}

// ============================================================
// Test 9: String Result
// ============================================================

void testStringResult() {
    RuntimeFixture fixture;

    fixture.initialize();

    requireString(
        fixture.execute("'Lamp Framework'"),
        "Lamp Framework"
    );
}

// ============================================================
// Test 10: Undefined Result
// ============================================================

void testUndefinedResult() {
    RuntimeFixture fixture;

    fixture.initialize();

    require(
        fixture.execute("undefined").isUndefined(),
        "Undefined result must be preserved"
    );
}

// ============================================================
// Test 11: Null Result
// ============================================================

void testNullResult() {
    RuntimeFixture fixture;

    fixture.initialize();

    require(
        fixture.execute("null").isNull(),
        "Null result must be preserved"
    );
}

// ============================================================
// Test 12: Persistent JavaScript State
// ============================================================

void testPersistentState() {
    RuntimeFixture fixture;

    fixture.initialize();

    fixture.executeVoid(
        "globalThis.counter = 10;"
    );

    requireNumber(
        fixture.execute(
            "globalThis.counter += 5;"
            "globalThis.counter;"
        ),
        15.0
    );
}

// ============================================================
// Test 13: Function Declarations
// ============================================================

void testFunctionDeclarations() {
    RuntimeFixture fixture;

    fixture.initialize();

    fixture.executeVoid(
        "function add(a, b) {"
        "  return a + b;"
        "}"
    );

    requireNumber(
        fixture.execute("add(20, 22)"),
        42.0
    );
}

// ============================================================
// Test 14: executeVoid
// ============================================================

void testExecuteVoid() {
    RuntimeFixture fixture;

    fixture.initialize();

    fixture.executeVoid(
        "globalThis.answer = 42;"
    );

    requireNumber(
        fixture.execute("globalThis.answer"),
        42.0
    );
}

// ============================================================
// Test 15: Syntax Error Propagation
// ============================================================

void testSyntaxError() {
    RuntimeFixture fixture;

    fixture.initialize();

    const auto error =
        requireThrows<V8ScriptError>(
            [&]() {
                (void)fixture.execute(
                    "const = ;"
                );
            },
            "Syntax error must propagate"
        );

    require(
        error.type() ==
            V8ScriptErrorType::Compilation,
        "Syntax error must be classified as compilation"
    );
}

// ============================================================
// Test 16: Runtime Error Propagation
// ============================================================

void testRuntimeError() {
    RuntimeFixture fixture;

    fixture.initialize();

    const auto error =
        requireThrows<V8ScriptError>(
            [&]() {
                (void)fixture.execute(
                    "throw new Error('runtime failure');"
                );
            },
            "Runtime error must propagate"
        );

    require(
        error.type() ==
            V8ScriptErrorType::Runtime,
        "Runtime exception must be classified correctly"
    );
}

// ============================================================
// Test 17: Recovery After Script Error
// ============================================================

void testRecoveryAfterScriptError() {
    RuntimeFixture fixture;

    fixture.initialize();

    (void)requireThrows<V8ScriptError>(
        [&]() {
            (void)fixture.execute(
                "throw new Error('temporary failure');"
            );
        },
        "Runtime exception must propagate"
    );

    require(
        fixture.runtime().isRunning(),
        "Runtime must remain running after script exception"
    );

    requireNumber(
        fixture.execute("6 * 7"),
        42.0
    );
}

// ============================================================
// Test 18: Native Function Registration
// ============================================================

void testNativeRegistration() {
    RuntimeFixture fixture;

    fixture.initialize();

    fixture.runtime().registerNativeFunction(
        "nativeAdd",
        makeAddHandler()
    );

    fixture.runtime().installNativeFunctions();

    requireNumber(
        fixture.execute("nativeAdd(20, 22)"),
        42.0
    );
}

// ============================================================
// Test 19: Multiple Native Functions
// ============================================================

void testMultipleNativeFunctions() {
    RuntimeFixture fixture;

    fixture.initialize();

    fixture.runtime().registerNativeFunction(
        "nativeAdd",
        makeAddHandler()
    );

    fixture.runtime().registerNativeFunction(
        "nativeMultiply",
        [](
            const V8NativeArguments& args
        ) -> V8Value {
            if (args.size() != 2 ||
                !args[0].isNumber() ||
                !args[1].isNumber()) {
                throw std::invalid_argument(
                    "Expected two numbers"
                );
            }

            return V8Value(
                std::get<double>(args[0].data) *
                std::get<double>(args[1].data)
            );
        }
    );

    fixture.runtime().installNativeFunctions();

    requireNumber(
        fixture.execute(
            "nativeAdd(2, 3) + nativeMultiply(6, 7)"
        ),
        47.0
    );
}

// ============================================================
// Test 20: Native String Return
// ============================================================

void testNativeStringReturn() {
    RuntimeFixture fixture;

    fixture.initialize();

    fixture.runtime().registerNativeFunction(
        "nativeName",
        [](
            const V8NativeArguments&
        ) -> V8Value {
            return V8Value(
                std::string("Lamp")
            );
        }
    );

    fixture.runtime().installNativeFunctions();

    requireString(
        fixture.execute("nativeName()"),
        "Lamp"
    );
}

// ============================================================
// Test 21: Native Exception Translation
// ============================================================

void testNativeExceptionTranslation() {
    RuntimeFixture fixture;

    fixture.initialize();

    fixture.runtime().registerNativeFunction(
        "nativeFail",
        [](
            const V8NativeArguments&
        ) -> V8Value {
            throw std::runtime_error(
                "native failure"
            );
        }
    );

    fixture.runtime().installNativeFunctions();

    requireBoolean(
        fixture.execute(
            "(() => {"
            "  try {"
            "    nativeFail();"
            "    return false;"
            "  } catch (error) {"
            "    return error.message.includes('native failure');"
            "  }"
            "})()"
        ),
        true
    );
}

// ============================================================
// Test 22: Simple ES Module
// ============================================================

void testSimpleModule() {
    RuntimeFixture fixture;

    fixture.write(
        "main.js",
        "globalThis.moduleResult = 42;"
    );

    fixture.initialize();

    fixture.executeModule("main.js");

    requireNumber(
        fixture.execute("globalThis.moduleResult"),
        42.0
    );
}

// ============================================================
// Test 23: Relative Module Import
// ============================================================

void testRelativeModuleImport() {
    RuntimeFixture fixture;

    fixture.write(
        "math.js",
        "export const answer = 42;"
    );

    fixture.write(
        "main.js",
        "import { answer } from './math.js';"
        "globalThis.moduleResult = answer;"
    );

    fixture.initialize();

    fixture.executeModule("main.js");

    requireNumber(
        fixture.execute("globalThis.moduleResult"),
        42.0
    );
}

// ============================================================
// Test 24: Module Cache
// ============================================================

void testModuleCache() {
    RuntimeFixture fixture;

    fixture.write(
        "main.js",
        "globalThis.moduleExecutions = "
        "(globalThis.moduleExecutions || 0) + 1;"
    );

    fixture.initialize();

    fixture.executeModule("main.js");
    fixture.executeModule("main.js");

    requireNumber(
        fixture.execute("globalThis.moduleExecutions"),
        1.0
    );
}

// ============================================================
// Test 25: Missing Module
// ============================================================

void testMissingModule() {
    RuntimeFixture fixture;

    fixture.initialize();

    requireThrows<lamp::v8::V8ModuleError>(
        [&]() {
            fixture.executeModule("missing.js");
        },
        "Missing module must throw V8ModuleError"
    );
}

// ============================================================
// Test 26: Recovery After Module Failure
// ============================================================

void testRecoveryAfterModuleFailure() {
    RuntimeFixture fixture;

    fixture.initialize();

    requireThrows<lamp::v8::V8ModuleError>(
        [&]() {
            fixture.executeModule("missing.js");
        },
        "Missing module must fail"
    );

    fixture.write(
        "valid.js",
        "globalThis.moduleResult = 42;"
    );

    fixture.executeModule("valid.js");

    requireNumber(
        fixture.execute("globalThis.moduleResult"),
        42.0
    );
}

// ============================================================
// Test 27: Context Access
// ============================================================

void testContextAccess() {
    RuntimeFixture fixture;

    fixture.initialize();

    bool invoked = false;

    fixture.runtime().withContext(
        [&invoked](
            ::v8::Isolate* isolate,
            ::v8::Local<::v8::Context> context
        ) {
            invoked = true;

            require(
                isolate != nullptr,
                "Context callback must receive isolate"
            );

            require(
                !context.IsEmpty(),
                "Context callback must receive context"
            );

            require(
                isolate->GetCurrentContext() == context,
                "Context must be entered during callback"
            );
        }
    );

    require(
        invoked,
        "withContext callback must execute"
    );
}

// ============================================================
// Test 28: Execution Before Initialization
// ============================================================

void testExecutionBeforeInitialization() {
    RuntimeFixture fixture;

    requireThrows<V8RuntimeError>(
        [&]() {
            (void)fixture.execute("1 + 1");
        },
        "Execution before initialization must fail"
    );
}

// ============================================================
// Test 29: Native Registration Before Initialization
// ============================================================

void testRegistrationBeforeInitialization() {
    RuntimeFixture fixture;

    requireThrows<V8RuntimeError>(
        [&]() {
            fixture.runtime().registerNativeFunction(
                "nativeAdd",
                makeAddHandler()
            );
        },
        "Native registration before initialization must fail"
    );
}

// ============================================================
// Test 30: Module Execution Before Initialization
// ============================================================

void testModuleBeforeInitialization() {
    RuntimeFixture fixture;

    requireThrows<V8RuntimeError>(
        [&]() {
            fixture.executeModule("main.js");
        },
        "Module execution before initialization must fail"
    );
}

// ============================================================
// Test 31: Runtime Shutdown
// ============================================================

void testRuntimeShutdown() {
    RuntimeFixture fixture;

    fixture.initialize();

    fixture.runtime().shutdown();

    require(
        fixture.runtime().state() ==
            V8RuntimeState::Stopped,
        "Shutdown must transition runtime to Stopped"
    );

    require(
        !fixture.runtime().isRunning(),
        "Stopped runtime must not report running"
    );
}

// ============================================================
// Test 32: Repeated Shutdown
// ============================================================

void testRepeatedShutdown() {
    RuntimeFixture fixture;

    fixture.initialize();

    fixture.runtime().shutdown();
    fixture.runtime().shutdown();

    require(
        fixture.runtime().state() ==
            V8RuntimeState::Stopped,
        "Repeated shutdown must preserve Stopped state"
    );
}

// ============================================================
// Test 33: Execution After Shutdown
// ============================================================

void testExecutionAfterShutdown() {
    RuntimeFixture fixture;

    fixture.initialize();
    fixture.runtime().shutdown();

    requireThrows<V8RuntimeError>(
        [&]() {
            (void)fixture.execute("1 + 1");
        },
        "Execution after shutdown must fail"
    );
}

// ============================================================
// Test 34: Module Execution After Shutdown
// ============================================================

void testModuleAfterShutdown() {
    RuntimeFixture fixture;

    fixture.initialize();
    fixture.runtime().shutdown();

    requireThrows<V8RuntimeError>(
        [&]() {
            fixture.executeModule("main.js");
        },
        "Module execution after shutdown must fail"
    );
}

// ============================================================
// Test 35: Context Access After Shutdown
// ============================================================

void testContextAfterShutdown() {
    RuntimeFixture fixture;

    fixture.initialize();
    fixture.runtime().shutdown();

    requireThrows<V8RuntimeError>(
        [&]() {
            fixture.runtime().withContext(
                [](
                    ::v8::Isolate*,
                    ::v8::Local<::v8::Context>
                ) {}
            );
        },
        "Context access after shutdown must fail"
    );
}

// ============================================================
// Test 36: Runtime Reinitialization Policy
// ============================================================

void testReinitializationPolicy() {
    RuntimeFixture fixture;

    fixture.initialize();
    fixture.runtime().shutdown();

    requireThrows<V8RuntimeError>(
        [&]() {
            fixture.initialize();
        },
        "Stopped runtime must reject reinitialization"
    );
}

// ============================================================
// Test 37: Multiple Runtime Instances
// ============================================================

void testMultipleRuntimeInstances() {
    RuntimeFixture first;
    RuntimeFixture second;

    first.initialize();
    second.initialize();

    first.executeVoid(
        "globalThis.runtimeValue = 10;"
    );

    second.executeVoid(
        "globalThis.runtimeValue = 20;"
    );

    requireNumber(
        first.execute("globalThis.runtimeValue"),
        10.0
    );

    requireNumber(
        second.execute("globalThis.runtimeValue"),
        20.0
    );
}

// ============================================================
// Test 38: Runtime Independence After Shutdown
// ============================================================

void testRuntimeIndependence() {
    RuntimeFixture first;
    RuntimeFixture second;

    first.initialize();
    second.initialize();

    first.runtime().shutdown();

    require(
        second.runtime().isRunning(),
        "Stopping one runtime must not stop another"
    );

    requireNumber(
        second.execute("6 * 7"),
        42.0
    );
}

// ============================================================
// Test 39: Automatic Runtime Destruction
// ============================================================

void testAutomaticDestruction() {
    {
        RuntimeFixture fixture;

        fixture.initialize();

        requireNumber(
            fixture.execute("20 + 22"),
            42.0
        );
    }

    // RuntimeFixture and V8Runtime destructors
    // have completed without an explicit shutdown.
    //
    // This is a lifecycle smoke test, not a memory
    // leak detector. Use sanitizers for deeper checks.
}

// ============================================================
// Test 40: Custom Module Root
// ============================================================

void testCustomModuleRoot() {
    RuntimeFixture fixture;

    fixture.write(
        "src/main.js",
        "globalThis.moduleResult = 42;"
    );

    fixture.initialize();

    fixture.executeModule("src/main.js");

    requireNumber(
        fixture.execute("globalThis.moduleResult"),
        42.0
    );
}

// ============================================================
// Test 41: Script Resource Configuration
// ============================================================

void testScriptResourceConfiguration() {
    RuntimeFixture fixture;

    fixture.initialize();

    lamp::v8::V8ScriptConfig config{};

    config.resourceName =
        "lamp-runtime-integration.js";

    requireNumber(
        fixture.runtime().execute(
            "20 + 22",
            config
        ),
        42.0
    );
}

// ============================================================
// Test 42: Runtime Remains Healthy After Native Error
// ============================================================

void testRecoveryAfterNativeError() {
    RuntimeFixture fixture;

    fixture.initialize();

    fixture.runtime().registerNativeFunction(
        "nativeFail",
        [](
            const V8NativeArguments&
        ) -> V8Value {
            throw std::runtime_error(
                "expected native failure"
            );
        }
    );

    fixture.runtime().installNativeFunctions();

    fixture.executeVoid(
        "try { nativeFail(); } catch (error) {}"
    );

    require(
        fixture.runtime().isRunning(),
        "Native callback failure must not stop runtime"
    );

    requireNumber(
        fixture.execute("6 * 7"),
        42.0
    );
}

// ============================================================
// Test 43: Runtime Configuration Persistence
// ============================================================

void testConfigurationPersistence() {
    RuntimeFixture fixture(
        [](V8RuntimeConfig& config) {
            config.context.name =
                "persistent-runtime";

            config.bindings.maxArguments = 8;
        }
    );

    fixture.initialize();

    const auto& config =
        fixture.runtime().config();

    require(
        config.context.name == "persistent-runtime",
        "Context configuration must persist"
    );

    require(
        config.bindings.maxArguments == 8,
        "Binding configuration must persist"
    );
}

// ============================================================
// Test 44: Shutdown Before Initialization
// ============================================================

void testShutdownBeforeInitialization() {
    RuntimeFixture fixture;

    fixture.runtime().shutdown();

    require(
        !fixture.runtime().isRunning(),
        "Shutdown before initialization must leave runtime inactive"
    );

    require(
        fixture.runtime().state() ==
            V8RuntimeState::Stopped,
        "Shutdown before initialization must enter Stopped state"
    );
}

} // namespace

// ============================================================
// Main
// ============================================================

int main() {
    TestRunner runner;

    // --------------------------------------------------------
    // Construction And Configuration
    // --------------------------------------------------------

    runner.add(
        "Initial runtime state",
        testInitialRuntimeState
    );

    runner.add(
        "Default configuration",
        testDefaultConfiguration
    );

    runner.add(
        "Custom configuration",
        testCustomConfiguration
    );

    // --------------------------------------------------------
    // Runtime Initialization
    // --------------------------------------------------------

    runner.add(
        "Runtime initialization",
        testRuntimeInitialization
    );

    runner.add(
        "Repeated initialization",
        testRepeatedInitialization
    );

    // --------------------------------------------------------
    // JavaScript Execution
    // --------------------------------------------------------

    runner.add(
        "Integer arithmetic",
        testIntegerArithmetic
    );

    runner.add(
        "Floating-point arithmetic",
        testFloatingPointArithmetic
    );

    runner.add(
        "Boolean results",
        testBooleanResults
    );

    runner.add(
        "String result",
        testStringResult
    );

    runner.add(
        "Undefined result",
        testUndefinedResult
    );

    runner.add(
        "Null result",
        testNullResult
    );

    runner.add(
        "Persistent JavaScript state",
        testPersistentState
    );

    runner.add(
        "Function declarations",
        testFunctionDeclarations
    );

    runner.add(
        "executeVoid",
        testExecuteVoid
    );

    // --------------------------------------------------------
    // JavaScript Errors
    // --------------------------------------------------------

    runner.add(
        "Syntax error propagation",
        testSyntaxError
    );

    runner.add(
        "Runtime error propagation",
        testRuntimeError
    );

    runner.add(
        "Recovery after script error",
        testRecoveryAfterScriptError
    );

    // --------------------------------------------------------
    // Native Bindings
    // --------------------------------------------------------

    runner.add(
        "Native function registration",
        testNativeRegistration
    );

    runner.add(
        "Multiple native functions",
        testMultipleNativeFunctions
    );

    runner.add(
        "Native string return",
        testNativeStringReturn
    );

    runner.add(
        "Native exception translation",
        testNativeExceptionTranslation
    );

    // --------------------------------------------------------
    // ES Modules
    // --------------------------------------------------------

    runner.add(
        "Simple ES module",
        testSimpleModule
    );

    runner.add(
        "Relative module import",
        testRelativeModuleImport
    );

    runner.add(
        "Module caching",
        testModuleCache
    );

    runner.add(
        "Missing module",
        testMissingModule
    );

    runner.add(
        "Recovery after module failure",
        testRecoveryAfterModuleFailure
    );

    // --------------------------------------------------------
    // Context Access
    // --------------------------------------------------------

    runner.add(
        "Context access",
        testContextAccess
    );

    // --------------------------------------------------------
    // Invalid State Handling
    // --------------------------------------------------------

    runner.add(
        "Execution before initialization",
        testExecutionBeforeInitialization
    );

    runner.add(
        "Native registration before initialization",
        testRegistrationBeforeInitialization
    );

    runner.add(
        "Module execution before initialization",
        testModuleBeforeInitialization
    );

    // --------------------------------------------------------
    // Shutdown
    // --------------------------------------------------------

    runner.add(
        "Runtime shutdown",
        testRuntimeShutdown
    );

    runner.add(
        "Repeated shutdown",
        testRepeatedShutdown
    );

    runner.add(
        "Execution after shutdown",
        testExecutionAfterShutdown
    );

    runner.add(
        "Module execution after shutdown",
        testModuleAfterShutdown
    );

    runner.add(
        "Context access after shutdown",
        testContextAfterShutdown
    );

    runner.add(
        "Runtime reinitialization policy",
        testReinitializationPolicy
    );

    // --------------------------------------------------------
    // Multiple Runtime Instances
    // --------------------------------------------------------

    runner.add(
        "Multiple runtime instances",
        testMultipleRuntimeInstances
    );

    runner.add(
        "Runtime independence after shutdown",
        testRuntimeIndependence
    );

    // --------------------------------------------------------
    // Resource Ownership
    // --------------------------------------------------------

    runner.add(
        "Automatic runtime destruction",
        testAutomaticDestruction
    );

    // --------------------------------------------------------
    // Additional Integration
    // --------------------------------------------------------

    runner.add(
        "Custom module root",
        testCustomModuleRoot
    );

    runner.add(
        "Script resource configuration",
        testScriptResourceConfiguration
    );

    runner.add(
        "Recovery after native error",
        testRecoveryAfterNativeError
    );

    runner.add(
        "Runtime configuration persistence",
        testConfigurationPersistence
    );

    runner.add(
        "Shutdown before initialization",
        testShutdownBeforeInitialization
    );

    // --------------------------------------------------------
    // Execute All Runtime Tests
    // --------------------------------------------------------

    const int result = runner.run();

    // --------------------------------------------------------
    // Shutdown Process-Wide V8 Platform
    // --------------------------------------------------------
    //
    // All RuntimeFixture instances have been destroyed.
    // No isolates or contexts should remain alive.
    //
    // --------------------------------------------------------

    try {
        auto& platform =
            lamp::v8::V8Platform::instance();

        if (platform.isInitialized()) {
            platform.shutdown();
        }

    } catch (const std::exception& error) {
        std::cerr
            << "[FATAL] V8 platform shutdown failed: "
            << error.what()
            << '\n';

        return 1;
    }

    return result;
}