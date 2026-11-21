// ============================================================
// Lamp Framework - V8 Platform Tests
// ============================================================
//
// File:
//   runtime/v8/tests/v8_platform_test.cpp
//
// Purpose:
//
//   Validate the lifecycle of the process-wide V8 platform.
//
// Coverage:
//
//   1. Initial platform state
//   2. Platform initialization
//   3. Configuration preservation
//   4. Platform availability
//   5. Repeated initialization
//   6. Singleton identity
//   7. Platform shutdown
//   8. Repeated shutdown
//   9. Reinitialization rejection
//
// IMPORTANT:
//
//   V8 is initialized once and disposed once per process.
//
//   These tests intentionally execute in a fixed order.
//
// ============================================================

#include "v8/v8_platform.hpp"

#include <exception>
#include <functional>
#include <iostream>
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
            << " Lamp V8 Platform Tests\n"
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
    bool caughtExpectedException = false;

    try {
        std::forward<Callback>(callback)();

    } catch (const Exception&) {
        caughtExpectedException = true;
    }

    require(
        caughtExpectedException,
        message
    );
}

// ============================================================
// Platform Configuration
// ============================================================

lamp::v8::V8PlatformConfig makeTestConfig() {
    lamp::v8::V8PlatformConfig config{};

    config.threadPoolSize = 2;
    config.enableIdleTasks = true;

    return config;
}

// ============================================================
// Test 1: Initial Platform State
// ============================================================

void testInitialState() {
    auto& platform =
        lamp::v8::V8Platform::instance();

    require(
        !platform.isInitialized(),
        "Platform must not be initialized initially"
    );

    require(
        !platform.isDisposed(),
        "Platform must not be disposed initially"
    );
}

// ============================================================
// Test 2: Platform Initialization
// ============================================================

void testInitialization() {
    auto& platform =
        lamp::v8::V8Platform::instance();

    platform.initialize(makeTestConfig());

    require(
        platform.isInitialized(),
        "Platform must be initialized"
    );

    require(
        !platform.isDisposed(),
        "Initialized platform must not be disposed"
    );
}

// ============================================================
// Test 3: Configuration Preservation
// ============================================================

void testConfiguration() {
    auto& platform =
        lamp::v8::V8Platform::instance();

    const auto config = platform.config();

    require(
        config.threadPoolSize == 2,
        "Thread pool size must be preserved"
    );

    require(
        config.enableIdleTasks,
        "Idle task configuration must be preserved"
    );
}

// ============================================================
// Test 4: Platform Availability
// ============================================================

void testPlatformAvailability() {
    auto& platform =
        lamp::v8::V8Platform::instance();

    require(
        platform.isInitialized(),
        "V8 platform must be initialized and available"
    );
}

// ============================================================
// Test 5: Repeated Initialization
// ============================================================

void testRepeatedInitialization() {
    auto& platform =
        lamp::v8::V8Platform::instance();

    // Reinitializing an already initialized platform
    // should be idempotent.

    platform.initialize(makeTestConfig());

    require(
        platform.isInitialized(),
        "Repeated initialization must preserve state"
    );
}

// ============================================================
// Test 6: Singleton Identity
// ============================================================

void testSingletonIdentity() {
    auto& first =
        lamp::v8::V8Platform::instance();

    auto& second =
        lamp::v8::V8Platform::instance();

    require(
        &first == &second,
        "Platform must be a singleton"
    );
}

// ============================================================
// Test 7: Platform Shutdown
// ============================================================

void testShutdown() {
    auto& platform =
        lamp::v8::V8Platform::instance();

    platform.shutdown();

    require(
        !platform.isInitialized(),
        "Platform must not remain initialized"
    );

    require(
        platform.isDisposed(),
        "Platform must be marked disposed"
    );
}

// ============================================================
// Test 8: Repeated Shutdown
// ============================================================

void testRepeatedShutdown() {
    auto& platform =
        lamp::v8::V8Platform::instance();

    // Repeated shutdown should be safe.

    platform.shutdown();

    require(
        platform.isDisposed(),
        "Repeated shutdown must preserve disposed state"
    );
}

// ============================================================
// Test 9: Reinitialization Rejection
// ============================================================

void testReinitializationAfterShutdown() {
    auto& platform =
        lamp::v8::V8Platform::instance();

    requireThrows<std::exception>(
        [&platform]() {
            platform.initialize(makeTestConfig());
        },
        "Platform must reject initialization after shutdown"
    );

    require(
        platform.isDisposed(),
        "Rejected initialization must preserve disposed state"
    );
}

} // namespace

// ============================================================
// Main
// ============================================================

int main() {
    TestRunner runner;

    // --------------------------------------------------------
    // Pre-Initialization Tests
    // --------------------------------------------------------

    runner.add(
        "Initial platform state",
        testInitialState
    );

    // --------------------------------------------------------
    // Initialization Tests
    // --------------------------------------------------------

    runner.add(
        "Platform initialization",
        testInitialization
    );

    runner.add(
        "Configuration preservation",
        testConfiguration
    );

    runner.add(
        "Platform availability",
        testPlatformAvailability
    );

    runner.add(
        "Repeated initialization",
        testRepeatedInitialization
    );

    runner.add(
        "Singleton identity",
        testSingletonIdentity
    );

    // --------------------------------------------------------
    // Shutdown Tests
    // --------------------------------------------------------

    runner.add(
        "Platform shutdown",
        testShutdown
    );

    runner.add(
        "Repeated shutdown",
        testRepeatedShutdown
    );

    runner.add(
        "Reinitialization after shutdown",
        testReinitializationAfterShutdown
    );

    return runner.run();
}