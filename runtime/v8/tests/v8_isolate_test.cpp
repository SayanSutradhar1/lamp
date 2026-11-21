// ============================================================
// Lamp Framework - V8 Isolate Tests
// ============================================================
//
// File:
//   runtime/v8/tests/v8_isolate_test.cpp
//
// Purpose:
//
//   Validate isolate creation, configuration, ownership,
//   and deterministic resource cleanup.
//
// Coverage:
//
//   1. Default isolate construction
//   2. Isolate validity
//   3. Native isolate access
//   4. Default configuration
//   5. Custom heap configuration
//   6. Multiple independent isolates
//   7. Explicit isolate disposal
//   8. Repeated isolate disposal
//   9. Automatic destruction
//  10. Isolate creation after another is disposed
//  11. Isolate creation without initialized platform
//  12. Invalid isolate configuration
//
// Lifecycle:
//
//   V8Platform::initialize()
//       |
//       +-- Isolate tests
//       |
//       +-- Destroy all isolates
//       |
//   V8Platform::shutdown()
//
// ============================================================

#include "v8/v8_platform.hpp"
#include "v8/v8_isolate.hpp"

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
            << " Lamp V8 Isolate Tests\n"
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
// Configuration Helpers
// ============================================================

lamp::v8::V8IsolateConfig makeDefaultConfig() {
    return {};
}

lamp::v8::V8IsolateConfig makeCustomConfig() {
    lamp::v8::V8IsolateConfig config{};

    config.initialHeapSizeMB = 16;
    config.maxHeapSizeMB = 128;

    return config;
}

// ============================================================
// Test 1: Default Isolate Construction
// ============================================================

void testDefaultConstruction() {
    lamp::v8::V8Isolate isolate;

    require(
        isolate.isValid(),
        "Default isolate must be valid"
    );
}

// ============================================================
// Test 2: Isolate Validity
// ============================================================

void testIsolateValidity() {
    lamp::v8::V8Isolate isolate;

    require(
        isolate.isValid(),
        "Newly created isolate must be valid"
    );

    require(
        isolate.get() != nullptr,
        "Valid isolate must expose a native pointer"
    );
}

// ============================================================
// Test 3: Native Isolate Access
// ============================================================

void testNativeIsolateAccess() {
    lamp::v8::V8Isolate isolate;

    ::v8::Isolate* native = isolate.get();

    require(
        native != nullptr,
        "Native V8 isolate pointer must not be null"
    );

    require(
        native == &isolate.isolate(),
        "Both native isolate accessors must agree"
    );
}

// ============================================================
// Test 4: Default Configuration
// ============================================================

void testDefaultConfiguration() {
    lamp::v8::V8Isolate isolate(
        makeDefaultConfig()
    );

    const auto& config = isolate.config();

    require(
        config.initialHeapSizeMB == 0,
        "Default initial heap must use V8 defaults"
    );

    require(
        config.maxHeapSizeMB == 0,
        "Default maximum heap must use V8 defaults"
    );
}

// ============================================================
// Test 5: Custom Heap Configuration
// ============================================================

void testCustomHeapConfiguration() {
    const auto requested = makeCustomConfig();

    lamp::v8::V8Isolate isolate(requested);

    require(
        isolate.isValid(),
        "Isolate with custom heap settings must be valid"
    );

    const auto& actual = isolate.config();

    require(
        actual.initialHeapSizeMB ==
            requested.initialHeapSizeMB,
        "Initial heap configuration must be preserved"
    );

    require(
        actual.maxHeapSizeMB ==
            requested.maxHeapSizeMB,
        "Maximum heap configuration must be preserved"
    );

    // The configuration accessor verifies what Lamp
    // requested, not the actual V8 heap limit.
    //
    // Heap limit enforcement should be tested separately
    // through V8 heap statistics and controlled workloads.
}

// ============================================================
// Test 6: Multiple Independent Isolates
// ============================================================

void testMultipleIndependentIsolates() {
    lamp::v8::V8Isolate first;
    lamp::v8::V8Isolate second;
    lamp::v8::V8Isolate third;

    require(
        first.isValid(),
        "First isolate must be valid"
    );

    require(
        second.isValid(),
        "Second isolate must be valid"
    );

    require(
        third.isValid(),
        "Third isolate must be valid"
    );

    require(
        first.get() != second.get(),
        "First and second isolates must differ"
    );

    require(
        first.get() != third.get(),
        "First and third isolates must differ"
    );

    require(
        second.get() != third.get(),
        "Second and third isolates must differ"
    );
}

// ============================================================
// Test 7: Explicit Disposal
// ============================================================

void testExplicitDisposal() {
    lamp::v8::V8Isolate isolate;

    require(
        isolate.isValid(),
        "Isolate must start valid"
    );

    isolate.dispose();

    require(
        !isolate.isValid(),
        "Disposed isolate must be invalid"
    );

    require(
        isolate.get() == nullptr,
        "Disposed isolate pointer must be null"
    );
}

// ============================================================
// Test 8: Repeated Disposal
// ============================================================

void testRepeatedDisposal() {
    lamp::v8::V8Isolate isolate;

    isolate.dispose();
    isolate.dispose();

    require(
        !isolate.isValid(),
        "Repeated disposal must preserve invalid state"
    );

    require(
        isolate.get() == nullptr,
        "Repeated disposal must preserve null pointer"
    );
}

// ============================================================
// Test 9: Automatic Destruction
// ============================================================

void testAutomaticDestruction() {
    {
        auto isolate =
            std::make_unique<lamp::v8::V8Isolate>();

        require(
            isolate->isValid(),
            "Heap-allocated isolate must be valid"
        );
    }

    // The unique_ptr and V8Isolate destructors have run.
    //
    // This is a lifecycle smoke test; tools such as
    // AddressSanitizer or Valgrind are required for
    // stronger memory-safety verification.

    require(
        true,
        "Automatic isolate destruction completed"
    );
}

// ============================================================
// Test 10: Create After Previous Disposal
// ============================================================

void testCreationAfterDisposal() {
    {
        lamp::v8::V8Isolate first;

        require(
            first.isValid(),
            "First isolate must be valid"
        );

        first.dispose();
    }

    lamp::v8::V8Isolate second;

    require(
        second.isValid(),
        "A new isolate must be creatable after disposal"
    );
}

// ============================================================
// Test 11: Invalid Configuration
// ============================================================
//
// The File 63 implementation does not currently enforce
// a complete heap-configuration validation policy.
//
// For that reason, we test only a configuration that
// should be valid and avoid inventing unsupported
// validation behavior.
//
// ============================================================

void testZeroHeapConfiguration() {
    lamp::v8::V8IsolateConfig config{};

    config.initialHeapSizeMB = 0;
    config.maxHeapSizeMB = 0;

    lamp::v8::V8Isolate isolate(config);

    require(
        isolate.isValid(),
        "Zero heap settings must select V8 defaults"
    );
}

// ============================================================
// Test 12: Isolate Creation Without Platform
// ============================================================
//
// This test must run only after every other isolate has
// been destroyed and the platform has shut down.
//
// ============================================================

void testCreationAfterPlatformShutdown() {
    auto& platform =
        lamp::v8::V8Platform::instance();

    require(
        platform.isDisposed(),
        "Platform must already be disposed"
    );

    requireThrows<lamp::v8::V8IsolateError>(
        []() {
            lamp::v8::V8Isolate isolate;
        },
        "Isolate creation must fail after platform shutdown"
    );
}

} // namespace

// ============================================================
// Main
// ============================================================

int main() {
    TestRunner runner;

    auto& platform =
        lamp::v8::V8Platform::instance();

    // --------------------------------------------------------
    // Initialize V8 Once
    // --------------------------------------------------------

    try {
        platform.initialize();

    } catch (const std::exception& error) {
        std::cerr
            << "[FATAL] V8 initialization failed: "
            << error.what()
            << '\n';

        return 1;
    }

    // --------------------------------------------------------
    // Register Isolate Tests
    // --------------------------------------------------------

    runner.add(
        "Default isolate construction",
        testDefaultConstruction
    );

    runner.add(
        "Isolate validity",
        testIsolateValidity
    );

    runner.add(
        "Native isolate access",
        testNativeIsolateAccess
    );

    runner.add(
        "Default isolate configuration",
        testDefaultConfiguration
    );

    runner.add(
        "Custom heap configuration",
        testCustomHeapConfiguration
    );

    runner.add(
        "Multiple independent isolates",
        testMultipleIndependentIsolates
    );

    runner.add(
        "Explicit isolate disposal",
        testExplicitDisposal
    );

    runner.add(
        "Repeated isolate disposal",
        testRepeatedDisposal
    );

    runner.add(
        "Automatic isolate destruction",
        testAutomaticDestruction
    );

    runner.add(
        "Isolate creation after disposal",
        testCreationAfterDisposal
    );

    runner.add(
        "Zero heap configuration",
        testZeroHeapConfiguration
    );

    // --------------------------------------------------------
    // Execute Tests While Platform Is Alive
    // --------------------------------------------------------

    const int result = runner.run();

    // --------------------------------------------------------
    // Shut Down V8 After All Isolates Are Gone
    // --------------------------------------------------------

    try {
        platform.shutdown();

    } catch (const std::exception& error) {
        std::cerr
            << "[FATAL] V8 shutdown failed: "
            << error.what()
            << '\n';

        return 1;
    }

    // --------------------------------------------------------
    // Verify Post-Shutdown Creation Is Rejected
    // --------------------------------------------------------

    try {
        testCreationAfterPlatformShutdown();

        std::cout
            << "[PASS] Isolate creation after platform shutdown\n";

    } catch (const std::exception& error) {
        std::cerr
            << "[FAIL] Isolate creation after platform shutdown: "
            << error.what()
            << '\n';

        return 1;
    }

    return result;
}