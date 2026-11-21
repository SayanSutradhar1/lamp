// ============================================================
// Lamp Framework - V8 ES Module Loader Tests
// ============================================================
//
// File:
//   runtime/v8/tests/v8_module_loader_test.cpp
//
// Purpose:
//   Validate the native ES module loader.
//
// Coverage:
//   - Entry module execution
//   - Relative imports
//   - Named and default exports
//   - Dependency graphs
//   - Module caching
//   - Circular imports
//   - Missing modules
//   - Syntax errors
//   - Runtime exceptions
//   - Filesystem sandboxing
//   - Module size and count limits
//   - Async evaluation rejection
//   - Loader disposal
//   - Recovery after failures
//
// ============================================================

#include "v8/v8_platform.hpp"
#include "v8/v8_isolate.hpp"
#include "v8/v8_context.hpp"
#include "v8/v8_module_loader.hpp"

#include <v8.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
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
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;

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

template <typename Exception, typename Callback>
void requireThrows(
    Callback&& callback,
    std::string_view message
) {
    try {
        std::forward<Callback>(callback)();
    } catch (const Exception&) {
        return;
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
            << " Lamp V8 Module Loader Tests\n"
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
// Temporary Filesystem Fixture
// ============================================================
//
// Every test receives a separate temporary directory.
//
// This prevents test modules from interfering with one
// another and avoids relying on the current working
// directory.
//
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
                    "lamp-v8-test-" +
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
                    "Unable to create temporary directory: " +
                    error.message()
                );
            }
        }

        throw TestFailure(
            "Unable to allocate unique temporary directory"
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
            "Fixture path must be relative"
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
            "Unable to open module fixture file"
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
// V8 Module Fixture
// ============================================================
//
// Member destruction order:
//
//   loader_
//   context_
//   isolate_
//
// The process-wide V8 platform remains initialized
// until every test fixture has been destroyed.
//
// ============================================================

class ModuleFixture final {
public:
    explicit ModuleFixture(
        std::function<void(
            lamp::v8::V8ModuleLoaderConfig&
        )> configure = {}
    )
        : directory_(),
          isolate_(),
          context_(isolate_) {

        lamp::v8::V8ModuleLoaderConfig config{};

        config.rootDirectory = directory_.root();
        config.restrictToRoot = true;

        if (configure) {
            configure(config);
        }

        loader_ =
            std::make_unique<lamp::v8::V8ModuleLoader>(
                context_,
                std::move(config)
            );
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

    lamp::v8::V8ModuleLoader& loader() {
        return *loader_;
    }

    void execute(
        const fs::path& entry
    ) {
        loader_->executeEntry(entry);
    }

    [[nodiscard]]
    std::size_t cachedModules() const {
        return loader_->cachedModuleCount();
    }

    template <typename Callback>
    void withScope(Callback&& callback) {
        context_.withScope(
            std::forward<Callback>(callback)
        );
    }

private:
    TemporaryDirectory directory_;
    lamp::v8::V8Isolate isolate_;
    lamp::v8::V8Context context_;

    std::unique_ptr<
        lamp::v8::V8ModuleLoader
    > loader_;
};

// ============================================================
// JavaScript Inspection Helpers
// ============================================================
//
// Module tests inspect globalThis values through the
// existing V8Context rather than depending on the
// V8ScriptEngine.
//
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
        "Unable to create V8 string"
    );

    return result;
}

::v8::Local<::v8::Value> evaluate(
    ::v8::Isolate* isolate,
    ::v8::Local<::v8::Context> context,
    std::string_view source
) {
    ::v8::TryCatch tryCatch(isolate);

    ::v8::Local<::v8::Script> script;

    require(
        ::v8::Script::Compile(
            context,
            makeString(isolate, source)
        ).ToLocal(&script),
        "Inspection script compilation failed"
    );

    ::v8::Local<::v8::Value> result;

    require(
        script->Run(context).ToLocal(&result),
        "Inspection script execution failed"
    );

    return result;
}

double readNumber(
    ModuleFixture& fixture,
    std::string_view expression
) {
    double result = 0.0;

    fixture.withScope(
        [&](::v8::Isolate* isolate,
            ::v8::Local<::v8::Context> context) {

            auto value = evaluate(
                isolate,
                context,
                expression
            );

            require(
                value->IsNumber(),
                "Expected numeric JavaScript value"
            );

            result =
                value.As<::v8::Number>()->Value();
        }
    );

    return result;
}

bool readBoolean(
    ModuleFixture& fixture,
    std::string_view expression
) {
    bool result = false;

    fixture.withScope(
        [&](::v8::Isolate* isolate,
            ::v8::Local<::v8::Context> context) {

            auto value = evaluate(
                isolate,
                context,
                expression
            );

            require(
                value->IsBoolean(),
                "Expected boolean JavaScript value"
            );

            result = value->BooleanValue(isolate);
        }
    );

    return result;
}

std::string readString(
    ModuleFixture& fixture,
    std::string_view expression
) {
    std::string result;

    fixture.withScope(
        [&](::v8::Isolate* isolate,
            ::v8::Local<::v8::Context> context) {

            auto value = evaluate(
                isolate,
                context,
                expression
            );

            require(
                value->IsString(),
                "Expected string JavaScript value"
            );

            ::v8::String::Utf8Value utf8(
                isolate,
                value
            );

            require(
                *utf8 != nullptr,
                "Unable to decode JavaScript string"
            );

            result.assign(
                *utf8,
                static_cast<std::size_t>(
                    utf8.length()
                )
            );
        }
    );

    return result;
}

// ============================================================
// Test 1: Initial Cache
// ============================================================

void testInitialCache() {
    ModuleFixture fixture;

    require(
        fixture.cachedModules() == 0,
        "New module loader must have an empty cache"
    );
}

// ============================================================
// Test 2: Root Directory
// ============================================================

void testRootDirectory() {
    ModuleFixture fixture;

    require(
        fs::equivalent(
            fixture.loader().rootDirectory(),
            fixture.root()
        ),
        "Loader root must match fixture directory"
    );
}

// ============================================================
// Test 3: Simple Entry Module
// ============================================================

void testSimpleEntryModule() {
    ModuleFixture fixture;

    fixture.write(
        "main.js",
        "globalThis.lampResult = 42;"
    );

    fixture.execute("main.js");

    require(
        readNumber(
            fixture,
            "globalThis.lampResult"
        ) == 42.0,
        "Entry module must execute"
    );
}

// ============================================================
// Test 4: Named Export
// ============================================================

void testNamedExport() {
    ModuleFixture fixture;

    fixture.write(
        "math.js",
        "export const answer = 42;"
    );

    fixture.write(
        "main.js",
        "import { answer } from './math.js';"
        "globalThis.lampResult = answer;"
    );

    fixture.execute("main.js");

    require(
        readNumber(
            fixture,
            "globalThis.lampResult"
        ) == 42.0,
        "Named export must be imported correctly"
    );
}

// ============================================================
// Test 5: Default Export
// ============================================================

void testDefaultExport() {
    ModuleFixture fixture;

    fixture.write(
        "config.js",
        "export default { port: 3000 };"
    );

    fixture.write(
        "main.js",
        "import config from './config.js';"
        "globalThis.lampPort = config.port;"
    );

    fixture.execute("main.js");

    require(
        readNumber(
            fixture,
            "globalThis.lampPort"
        ) == 3000.0,
        "Default export must be imported correctly"
    );
}

// ============================================================
// Test 6: Relative Import
// ============================================================

void testRelativeImport() {
    ModuleFixture fixture;

    fixture.write(
        "lib/math.js",
        "export function add(a, b) {"
        "  return a + b;"
        "}"
    );

    fixture.write(
        "main.js",
        "import { add } from './lib/math.js';"
        "globalThis.lampResult = add(20, 22);"
    );

    fixture.execute("main.js");

    require(
        readNumber(
            fixture,
            "globalThis.lampResult"
        ) == 42.0,
        "Relative import must resolve"
    );
}

// ============================================================
// Test 7: Parent Directory Import Within Root
// ============================================================

void testParentDirectoryImport() {
    ModuleFixture fixture;

    fixture.write(
        "shared/constants.js",
        "export const value = 42;"
    );

    fixture.write(
        "features/main.js",
        "import { value } from '../shared/constants.js';"
        "globalThis.lampResult = value;"
    );

    fixture.execute("features/main.js");

    require(
        readNumber(
            fixture,
            "globalThis.lampResult"
        ) == 42.0,
        "Parent import within root must succeed"
    );
}

// ============================================================
// Test 8: Nested Dependency Graph
// ============================================================

void testNestedDependencyGraph() {
    ModuleFixture fixture;

    fixture.write(
        "math/base.js",
        "export const base = 40;"
    );

    fixture.write(
        "math/add.js",
        "import { base } from './base.js';"
        "export const result = base + 2;"
    );

    fixture.write(
        "main.js",
        "import { result } from './math/add.js';"
        "globalThis.lampResult = result;"
    );

    fixture.execute("main.js");

    require(
        readNumber(
            fixture,
            "globalThis.lampResult"
        ) == 42.0,
        "Nested dependency graph must execute"
    );

    require(
        fixture.cachedModules() == 3,
        "All three modules must be cached"
    );
}

// ============================================================
// Test 9: Shared Dependency
// ============================================================

void testSharedDependency() {
    ModuleFixture fixture;

    fixture.write(
        "shared.js",
        "globalThis.sharedExecutions = "
        "(globalThis.sharedExecutions || 0) + 1;"
        "export const value = 21;"
    );

    fixture.write(
        "left.js",
        "import { value } from './shared.js';"
        "export const left = value;"
    );

    fixture.write(
        "right.js",
        "import { value } from './shared.js';"
        "export const right = value;"
    );

    fixture.write(
        "main.js",
        "import { left } from './left.js';"
        "import { right } from './right.js';"
        "globalThis.lampResult = left + right;"
    );

    fixture.execute("main.js");

    require(
        readNumber(
            fixture,
            "globalThis.lampResult"
        ) == 42.0,
        "Shared dependency exports must work"
    );

    require(
        readNumber(
            fixture,
            "globalThis.sharedExecutions"
        ) == 1.0,
        "Shared module must execute once"
    );

    require(
        fixture.cachedModules() == 4,
        "Shared dependency must have one cache entry"
    );
}

// ============================================================
// Test 10: Repeated Entry Execution
// ============================================================

void testRepeatedEntryExecution() {
    ModuleFixture fixture;

    fixture.write(
        "main.js",
        "globalThis.executionCount = "
        "(globalThis.executionCount || 0) + 1;"
    );

    fixture.execute("main.js");
    fixture.execute("main.js");

    require(
        readNumber(
            fixture,
            "globalThis.executionCount"
        ) == 1.0,
        "Cached ES module must not evaluate twice"
    );

    require(
        fixture.cachedModules() == 1,
        "Repeated entry must not duplicate cache entries"
    );
}

// ============================================================
// Test 11: Namespace Import
// ============================================================

void testNamespaceImport() {
    ModuleFixture fixture;

    fixture.write(
        "math.js",
        "export const a = 20;"
        "export const b = 22;"
    );

    fixture.write(
        "main.js",
        "import * as math from './math.js';"
        "globalThis.lampResult = math.a + math.b;"
    );

    fixture.execute("main.js");

    require(
        readNumber(
            fixture,
            "globalThis.lampResult"
        ) == 42.0,
        "Namespace import must work"
    );
}

// ============================================================
// Test 12: Re-Export
// ============================================================

void testReExport() {
    ModuleFixture fixture;

    fixture.write(
        "math.js",
        "export const answer = 42;"
    );

    fixture.write(
        "index.js",
        "export { answer } from './math.js';"
    );

    fixture.write(
        "main.js",
        "import { answer } from './index.js';"
        "globalThis.lampResult = answer;"
    );

    fixture.execute("main.js");

    require(
        readNumber(
            fixture,
            "globalThis.lampResult"
        ) == 42.0,
        "Re-export must resolve"
    );
}

// ============================================================
// Test 13: Circular Imports
// ============================================================
//
// The cycle is intentionally structured so that neither
// module reads an uninitialized binding.
//
// ============================================================

void testCircularImports() {
    ModuleFixture fixture;

    fixture.write(
        "a.js",
        "import { getB } from './b.js';"
        "export function getA() { return 20; }"
        "export function result() {"
        "  return getA() + getB();"
        "}"
    );

    fixture.write(
        "b.js",
        "import { getA } from './a.js';"
        "export function getB() {"
        "  return getA() + 2;"
        "}"
    );

    fixture.write(
        "main.js",
        "import { result } from './a.js';"
        "globalThis.lampResult = result();"
    );

    fixture.execute("main.js");

    require(
        readNumber(
            fixture,
            "globalThis.lampResult"
        ) == 42.0,
        "Circular import graph must execute"
    );

    require(
        fixture.cachedModules() == 3,
        "Circular graph must not duplicate modules"
    );
}

// ============================================================
// Test 14: Missing Entry
// ============================================================

void testMissingEntry() {
    ModuleFixture fixture;

    requireThrows<lamp::v8::V8ModuleError>(
        [&]() {
            fixture.execute("missing.js");
        },
        "Missing entry module must fail"
    );
}

// ============================================================
// Test 15: Missing Dependency
// ============================================================

void testMissingDependency() {
    ModuleFixture fixture;

    fixture.write(
        "main.js",
        "import './missing.js';"
    );

    requireThrows<lamp::v8::V8ModuleError>(
        [&]() {
            fixture.execute("main.js");
        },
        "Missing dependency must fail"
    );
}

// ============================================================
// Test 16: Syntax Error
// ============================================================

void testSyntaxError() {
    ModuleFixture fixture;

    fixture.write(
        "main.js",
        "export const = ;"
    );

    requireThrows<lamp::v8::V8ModuleError>(
        [&]() {
            fixture.execute("main.js");
        },
        "Invalid module syntax must fail"
    );
}

// ============================================================
// Test 17: Dependency Syntax Error
// ============================================================

void testDependencySyntaxError() {
    ModuleFixture fixture;

    fixture.write(
        "broken.js",
        "export function ("
    );

    fixture.write(
        "main.js",
        "import './broken.js';"
    );

    requireThrows<lamp::v8::V8ModuleError>(
        [&]() {
            fixture.execute("main.js");
        },
        "Dependency syntax errors must propagate"
    );
}

// ============================================================
// Test 18: Runtime Exception
// ============================================================

void testRuntimeException() {
    ModuleFixture fixture;

    fixture.write(
        "main.js",
        "throw new Error('module failure');"
    );

    requireThrows<lamp::v8::V8ModuleError>(
        [&]() {
            fixture.execute("main.js");
        },
        "Module runtime exceptions must propagate"
    );
}

// ============================================================
// Test 19: Missing Named Export
// ============================================================

void testMissingNamedExport() {
    ModuleFixture fixture;

    fixture.write(
        "module.js",
        "export const existing = 42;"
    );

    fixture.write(
        "main.js",
        "import { missing } from './module.js';"
        "globalThis.lampResult = missing;"
    );

    requireThrows<lamp::v8::V8ModuleError>(
        [&]() {
            fixture.execute("main.js");
        },
        "Missing named export must fail instantiation"
    );
}

// ============================================================
// Test 20: Recovery After Missing Entry
// ============================================================

void testRecoveryAfterMissingEntry() {
    ModuleFixture fixture;

    requireThrows<lamp::v8::V8ModuleError>(
        [&]() {
            fixture.execute("missing.js");
        },
        "Missing entry must fail"
    );

    fixture.write(
        "valid.js",
        "globalThis.lampResult = 42;"
    );

    fixture.execute("valid.js");

    require(
        readNumber(
            fixture,
            "globalThis.lampResult"
        ) == 42.0,
        "Loader must recover after missing entry"
    );
}

// ============================================================
// Test 21: Recovery After Syntax Error
// ============================================================

void testRecoveryAfterSyntaxError() {
    ModuleFixture fixture;

    fixture.write(
        "broken.js",
        "export const = ;"
    );

    requireThrows<lamp::v8::V8ModuleError>(
        [&]() {
            fixture.execute("broken.js");
        },
        "Broken module must fail"
    );

    fixture.write(
        "valid.js",
        "globalThis.lampResult = 42;"
    );

    fixture.execute("valid.js");

    require(
        readNumber(
            fixture,
            "globalThis.lampResult"
        ) == 42.0,
        "Loader must recover after compilation failure"
    );
}

// ============================================================
// Test 22: Reject Parent Traversal Outside Root
// ============================================================

void testRejectParentTraversal() {
    ModuleFixture fixture;

    fixture.write(
        "main.js",
        "import '../outside.js';"
    );

    requireThrows<lamp::v8::V8ModuleError>(
        [&]() {
            fixture.execute("main.js");
        },
        "Imports escaping root must be rejected"
    );
}

// ============================================================
// Test 23: Reject Absolute Entry Outside Root
// ============================================================

void testRejectAbsoluteEntryOutsideRoot() {
    TemporaryDirectory outside;
    ModuleFixture fixture;

    const auto outsideFile = outside.write(
        "outside.js",
        "globalThis.escaped = true;"
    );

    requireThrows<lamp::v8::V8ModuleError>(
        [&]() {
            fixture.execute(outsideFile);
        },
        "Absolute entry outside root must be rejected"
    );
}

// ============================================================
// Test 24: Reject Symlink Escape
// ============================================================
//
// Symlink creation can require elevated privileges on
// Windows. If unsupported, this specific test is skipped.
//
// ============================================================

void testRejectSymlinkEscape() {
    TemporaryDirectory outside;
    ModuleFixture fixture;

    const auto outsideFile = outside.write(
        "outside.js",
        "globalThis.escaped = true;"
    );

    const auto link =
        fixture.root() / "linked.js";

    std::error_code error;

    fs::create_symlink(
        outsideFile,
        link,
        error
    );

    if (error) {
        std::cout
            << "[INFO] Symlink escape check unavailable: "
            << error.message()
            << '\n';

        return;
    }

    requireThrows<lamp::v8::V8ModuleError>(
        [&]() {
            fixture.execute("linked.js");
        },
        "Symlink escape must be rejected"
    );
}

// ============================================================
// Test 25: Maximum Module Size
// ============================================================

void testMaximumModuleSize() {
    ModuleFixture fixture(
        [](lamp::v8::V8ModuleLoaderConfig& config) {
            config.maxModuleBytes = 16;
        }
    );

    fixture.write(
        "main.js",
        "globalThis.lampResult = 42;"
    );

    requireThrows<lamp::v8::V8ModuleError>(
        [&]() {
            fixture.execute("main.js");
        },
        "Oversized module must be rejected"
    );
}

// ============================================================
// Test 26: Maximum Module Count
// ============================================================

void testMaximumModuleCount() {
    ModuleFixture fixture(
        [](lamp::v8::V8ModuleLoaderConfig& config) {
            config.maxModules = 2;
        }
    );

    fixture.write(
        "first.js",
        "export const first = 1;"
    );

    fixture.write(
        "second.js",
        "export const second = 2;"
    );

    fixture.write(
        "main.js",
        "import { first } from './first.js';"
        "import { second } from './second.js';"
        "globalThis.lampResult = first + second;"
    );

    requireThrows<lamp::v8::V8ModuleError>(
        [&]() {
            fixture.execute("main.js");
        },
        "Module count limit must be enforced"
    );
}

// ============================================================
// Test 27: Module Without Imports
// ============================================================

void testModuleWithoutImports() {
    ModuleFixture fixture;

    fixture.write(
        "main.js",
        "const value = 40 + 2;"
        "globalThis.lampResult = value;"
    );

    fixture.execute("main.js");

    require(
        readNumber(
            fixture,
            "globalThis.lampResult"
        ) == 42.0,
        "Module without imports must execute"
    );
}

// ============================================================
// Test 28: Module String Result
// ============================================================

void testModuleStringResult() {
    ModuleFixture fixture;

    fixture.write(
        "main.js",
        "globalThis.frameworkName = 'Lamp';"
    );

    fixture.execute("main.js");

    require(
        readString(
            fixture,
            "globalThis.frameworkName"
        ) == "Lamp",
        "Module must update string global"
    );
}

// ============================================================
// Test 29: Module Boolean Result
// ============================================================

void testModuleBooleanResult() {
    ModuleFixture fixture;

    fixture.write(
        "main.js",
        "globalThis.frameworkReady = true;"
    );

    fixture.execute("main.js");

    require(
        readBoolean(
            fixture,
            "globalThis.frameworkReady"
        ),
        "Module must update boolean global"
    );
}

// ============================================================
// Test 30: Loader Disposal
// ============================================================

void testLoaderDisposal() {
    ModuleFixture fixture;

    fixture.write(
        "main.js",
        "globalThis.lampResult = 42;"
    );

    fixture.execute("main.js");

    require(
        fixture.cachedModules() == 1,
        "Module must be cached before disposal"
    );

    fixture.loader().dispose();

    requireThrows<lamp::v8::V8ModuleError>(
        [&]() {
            fixture.execute("main.js");
        },
        "Disposed loader must reject execution"
    );
}

// ============================================================
// Test 31: Repeated Disposal
// ============================================================

void testRepeatedDisposal() {
    ModuleFixture fixture;

    fixture.loader().dispose();
    fixture.loader().dispose();

    requireThrows<lamp::v8::V8ModuleError>(
        [&]() {
            fixture.execute("missing.js");
        },
        "Repeated disposal must preserve disabled state"
    );
}

// ============================================================
// Test 32: Module Cache Identity
// ============================================================

void testModuleCacheIdentity() {
    ModuleFixture fixture;

    fixture.write(
        "shared.js",
        "globalThis.sharedCount = "
        "(globalThis.sharedCount || 0) + 1;"
        "export const value = 42;"
    );

    fixture.write(
        "first.js",
        "import { value } from './shared.js';"
        "globalThis.firstResult = value;"
    );

    fixture.write(
        "second.js",
        "import { value } from './shared.js';"
        "globalThis.secondResult = value;"
    );

    fixture.execute("first.js");
    fixture.execute("second.js");

    require(
        readNumber(
            fixture,
            "globalThis.sharedCount"
        ) == 1.0,
        "Shared module must retain evaluation identity"
    );

    require(
        fixture.cachedModules() == 3,
        "Cache must contain three unique modules"
    );
}

// ============================================================
// Test 33: Top-Level Await Policy
// ============================================================
//
// File 68 currently implements synchronous module
// evaluation and rejects pending evaluation promises.
//
// ============================================================

void testTopLevelAwaitPolicy() {
    ModuleFixture fixture;

    fixture.write(
        "main.js",
        "await new Promise(() => {});"
    );

    requireThrows<lamp::v8::V8ModuleError>(
        [&]() {
            fixture.execute("main.js");
        },
        "Pending top-level await must be rejected"
    );
}

// ============================================================
// Test 34: Dependency Import Alias
// ============================================================

void testImportAlias() {
    ModuleFixture fixture;

    fixture.write(
        "math.js",
        "export const answer = 42;"
    );

    fixture.write(
        "main.js",
        "import { answer as result } from './math.js';"
        "globalThis.lampResult = result;"
    );

    fixture.execute("main.js");

    require(
        readNumber(
            fixture,
            "globalThis.lampResult"
        ) == 42.0,
        "Import aliases must work"
    );
}

// ============================================================
// Test 35: Nested Module Directory
// ============================================================

void testNestedModuleDirectory() {
    ModuleFixture fixture;

    fixture.write(
        "src/features/math/answer.js",
        "export const answer = 42;"
    );

    fixture.write(
        "src/main.js",
        "import { answer } "
        "from './features/math/answer.js';"
        "globalThis.lampResult = answer;"
    );

    fixture.execute("src/main.js");

    require(
        readNumber(
            fixture,
            "globalThis.lampResult"
        ) == 42.0,
        "Nested module paths must resolve correctly"
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
            << "[FATAL] V8 platform initialization failed: "
            << error.what()
            << '\n';

        return 1;
    }

    TestRunner runner;

    // --------------------------------------------------------
    // Loader Initialization
    // --------------------------------------------------------

    runner.add(
        "Initial module cache",
        testInitialCache
    );

    runner.add(
        "Module root directory",
        testRootDirectory
    );

    // --------------------------------------------------------
    // Basic Module Execution
    // --------------------------------------------------------

    runner.add(
        "Simple entry module",
        testSimpleEntryModule
    );

    runner.add(
        "Named export",
        testNamedExport
    );

    runner.add(
        "Default export",
        testDefaultExport
    );

    runner.add(
        "Relative import",
        testRelativeImport
    );

    runner.add(
        "Parent directory import",
        testParentDirectoryImport
    );

    runner.add(
        "Nested dependency graph",
        testNestedDependencyGraph
    );

    runner.add(
        "Shared dependency",
        testSharedDependency
    );

    runner.add(
        "Repeated entry execution",
        testRepeatedEntryExecution
    );

    runner.add(
        "Namespace import",
        testNamespaceImport
    );

    runner.add(
        "Re-export",
        testReExport
    );

    runner.add(
        "Circular imports",
        testCircularImports
    );

    // --------------------------------------------------------
    // Error Handling
    // --------------------------------------------------------

    runner.add(
        "Missing entry module",
        testMissingEntry
    );

    runner.add(
        "Missing dependency",
        testMissingDependency
    );

    runner.add(
        "Syntax error",
        testSyntaxError
    );

    runner.add(
        "Dependency syntax error",
        testDependencySyntaxError
    );

    runner.add(
        "Runtime exception",
        testRuntimeException
    );

    runner.add(
        "Missing named export",
        testMissingNamedExport
    );

    runner.add(
        "Recovery after missing entry",
        testRecoveryAfterMissingEntry
    );

    runner.add(
        "Recovery after syntax error",
        testRecoveryAfterSyntaxError
    );

    // --------------------------------------------------------
    // Filesystem Security
    // --------------------------------------------------------

    runner.add(
        "Reject parent traversal",
        testRejectParentTraversal
    );

    runner.add(
        "Reject absolute entry outside root",
        testRejectAbsoluteEntryOutsideRoot
    );

    runner.add(
        "Reject symlink escape",
        testRejectSymlinkEscape
    );

    // --------------------------------------------------------
    // Resource Limits
    // --------------------------------------------------------

    runner.add(
        "Maximum module size",
        testMaximumModuleSize
    );

    runner.add(
        "Maximum module count",
        testMaximumModuleCount
    );

    // --------------------------------------------------------
    // JavaScript Semantics
    // --------------------------------------------------------

    runner.add(
        "Module without imports",
        testModuleWithoutImports
    );

    runner.add(
        "Module string result",
        testModuleStringResult
    );

    runner.add(
        "Module boolean result",
        testModuleBooleanResult
    );

    runner.add(
        "Module cache identity",
        testModuleCacheIdentity
    );

    runner.add(
        "Top-level await policy",
        testTopLevelAwaitPolicy
    );

    runner.add(
        "Import alias",
        testImportAlias
    );

    runner.add(
        "Nested module directory",
        testNestedModuleDirectory
    );

    // --------------------------------------------------------
    // Loader Lifecycle
    // --------------------------------------------------------

    runner.add(
        "Loader disposal",
        testLoaderDisposal
    );

    runner.add(
        "Repeated disposal",
        testRepeatedDisposal
    );

    const int result = runner.run();

    // All module fixtures have been destroyed here.

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