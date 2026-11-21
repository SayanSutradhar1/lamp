
#include "core/service_registry.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace lamp::core;

// ============================================================
// Minimal Test Framework
// ============================================================

class TestFailure final : public std::runtime_error {
public:
    explicit TestFailure(std::string message)
        : std::runtime_error(std::move(message)) {}
};

#define LAMP_ASSERT(condition)                              \
    do {                                                    \
        if (!(condition)) {                                 \
            throw TestFailure(                              \
                std::string("Assertion failed: ") +         \
                #condition + " at line " +                  \
                std::to_string(__LINE__)                    \
            );                                              \
        }                                                   \
    } while (false)

#define LAMP_ASSERT_EQ(actual, expected)                    \
    do {                                                    \
        const auto lamp_actual = (actual);                   \
        const auto lamp_expected = (expected);               \
        LAMP_ASSERT(lamp_actual == lamp_expected);           \
    } while (false)

template <typename Function>
void expectRegistryError(
    Function&& function,
    ServiceRegistryErrorCode expected
) {
    bool caught = false;

    try {
        std::forward<Function>(function)();
    } catch (const ServiceRegistryError& error) {
        caught = true;
        LAMP_ASSERT_EQ(error.code(), expected);
    }

    LAMP_ASSERT(caught);
}

// ============================================================
// Test Services
// ============================================================

struct CounterService {
    std::atomic<int> value{0};

    void increment() noexcept {
        value.fetch_add(1, std::memory_order_relaxed);
    }

    [[nodiscard]]
    int get() const noexcept {
        return value.load(std::memory_order_relaxed);
    }
};

struct ConfigService {
    explicit ConfigService(int initial)
        : value(initial) {}

    int value;
};

struct NameService {
    explicit NameService(std::string initial)
        : name(std::move(initial)) {}

    std::string name;
};

struct ILogger {
    virtual ~ILogger() = default;
    virtual std::string message() const = 0;
};

struct ConsoleLogger final : ILogger {
    std::string message() const override {
        return "console";
    }
};

struct FileLogger final : ILogger {
    std::string message() const override {
        return "file";
    }
};

struct DestructionTracker {
    explicit DestructionTracker(std::atomic<int>& count)
        : count_(count) {}

    ~DestructionTracker() {
        count_.fetch_add(1);
    }

private:
    std::atomic<int>& count_;
};

struct ReentrantDestructionService {
    ServiceRegistry* registry = nullptr;
    std::atomic<bool>* callbackExecuted = nullptr;

    ~ReentrantDestructionService() {
        if (registry && callbackExecuted) {
            (void)registry->size();
            callbackExecuted->store(true);
        }
    }
};

struct ServiceA {
    int value = 1;
};

struct ServiceB {
    int value = 2;
};

struct ServiceC {
    int value = 3;
};

// ============================================================
// Test 1: Empty Registry
// ============================================================

void testEmptyRegistry() {
    ServiceRegistry registry;

    LAMP_ASSERT(registry.empty());
    LAMP_ASSERT_EQ(registry.size(), 0U);
    LAMP_ASSERT(!registry.isClosed());
}

// ============================================================
// Test 2: Register Existing Service
// ============================================================

void testRegisterExistingService() {
    ServiceRegistry registry;

    auto service = std::make_shared<ConfigService>(42);

    registry.registerService(service);

    LAMP_ASSERT_EQ(registry.size(), 1U);
    LAMP_ASSERT(registry.contains<ConfigService>());
}

// ============================================================
// Test 3: Resolve Registered Service
// ============================================================

void testResolveRegisteredService() {
    ServiceRegistry registry;

    auto original = std::make_shared<ConfigService>(100);

    registry.registerService(original);

    auto resolved = registry.resolve<ConfigService>();

    LAMP_ASSERT(resolved == original);
    LAMP_ASSERT_EQ(resolved->value, 100);
}

// ============================================================
// Test 4: Resolve Missing Service
// ============================================================

void testResolveMissingService() {
    ServiceRegistry registry;

    expectRegistryError(
        [&]() {
            (void)registry.resolve<ConfigService>();
        },
        ServiceRegistryErrorCode::NotRegistered
    );
}

// ============================================================
// Test 5: Optional Resolution
// ============================================================

void testOptionalResolution() {
    ServiceRegistry registry;

    LAMP_ASSERT(!registry.tryResolve<ConfigService>());

    auto service = std::make_shared<ConfigService>(25);

    registry.registerService(service);

    LAMP_ASSERT(
        registry.tryResolve<ConfigService>() == service
    );
}

// ============================================================
// Test 6: Duplicate Registration
// ============================================================

void testDuplicateRegistration() {
    ServiceRegistry registry;

    registry.registerService(
        std::make_shared<ConfigService>(1)
    );

    expectRegistryError(
        [&]() {
            registry.registerService(
                std::make_shared<ConfigService>(2)
            );
        },
        ServiceRegistryErrorCode::AlreadyRegistered
    );

    LAMP_ASSERT_EQ(
        registry.resolve<ConfigService>()->value,
        1
    );
}

// ============================================================
// Test 7: Null Registration
// ============================================================

void testNullRegistration() {
    ServiceRegistry registry;

    std::shared_ptr<ConfigService> service;

    expectRegistryError(
        [&]() {
            registry.registerService(service);
        },
        ServiceRegistryErrorCode::NullService
    );

    LAMP_ASSERT(registry.empty());
}

// ============================================================
// Test 8: Emplace Service
// ============================================================

void testEmplaceService() {
    ServiceRegistry registry;

    auto service = registry.emplace<ConfigService>(77);

    LAMP_ASSERT_EQ(service->value, 77);
    LAMP_ASSERT(
        registry.resolve<ConfigService>() == service
    );
}

// ============================================================
// Test 9: Multiple Service Types
// ============================================================

void testMultipleServiceTypes() {
    ServiceRegistry registry;

    (void)registry.emplace<ServiceA>();
    (void)registry.emplace<ServiceB>();
    (void)registry.emplace<ServiceC>();

    LAMP_ASSERT_EQ(registry.size(), 3U);
    LAMP_ASSERT_EQ(registry.resolve<ServiceA>()->value, 1);
    LAMP_ASSERT_EQ(registry.resolve<ServiceB>()->value, 2);
    LAMP_ASSERT_EQ(registry.resolve<ServiceC>()->value, 3);
}

// ============================================================
// Test 10: Interface Registration
// ============================================================

void testInterfaceRegistration() {
    ServiceRegistry registry;

    auto implementation = std::make_shared<ConsoleLogger>();

    registry.registerAs<ILogger>(implementation);

    auto logger = registry.resolve<ILogger>();

    LAMP_ASSERT_EQ(
        logger->message(),
        std::string("console")
    );

    LAMP_ASSERT(
        std::dynamic_pointer_cast<ConsoleLogger>(logger)
        == implementation
    );
}

// ============================================================
// Test 11: Interface Key Isolation
// ============================================================

void testInterfaceKeyIsolation() {
    ServiceRegistry registry;

    registry.registerAs<ILogger>(
        std::make_shared<ConsoleLogger>()
    );

    LAMP_ASSERT(registry.contains<ILogger>());
    LAMP_ASSERT(!registry.contains<ConsoleLogger>());
    LAMP_ASSERT_EQ(registry.size(), 1U);
}

// ============================================================
// Test 12: Const Service Resolution
// ============================================================

void testConstServiceResolution() {
    ServiceRegistry registry;

    (void)registry.emplace<ConfigService>(33);

    auto service = registry.resolve<const ConfigService>();

    static_assert(
        std::is_same_v<
            decltype(service),
            std::shared_ptr<const ConfigService>
        >
    );

    LAMP_ASSERT_EQ(service->value, 33);
}

// ============================================================
// Test 13: Replace Existing Service
// ============================================================

void testReplaceExistingService() {
    ServiceRegistry registry;

    auto original = registry.emplace<ConfigService>(10);
    auto replacement = std::make_shared<ConfigService>(20);

    registry.replace(replacement);

    LAMP_ASSERT(
        registry.resolve<ConfigService>() == replacement
    );

    LAMP_ASSERT_EQ(original->value, 10);
    LAMP_ASSERT_EQ(registry.size(), 1U);
}

// ============================================================
// Test 14: Replace Missing Service
// ============================================================

void testReplaceMissingService() {
    ServiceRegistry registry;

    expectRegistryError(
        [&]() {
            registry.replace(
                std::make_shared<ConfigService>(5)
            );
        },
        ServiceRegistryErrorCode::NotRegistered
    );
}

// ============================================================
// Test 15: Replace With Null
// ============================================================

void testReplaceWithNull() {
    ServiceRegistry registry;

    (void)registry.emplace<ConfigService>(10);

    std::shared_ptr<ConfigService> replacement;

    expectRegistryError(
        [&]() {
            registry.replace(replacement);
        },
        ServiceRegistryErrorCode::NullService
    );

    LAMP_ASSERT_EQ(
        registry.resolve<ConfigService>()->value,
        10
    );
}

// ============================================================
// Test 16: Remove Service
// ============================================================

void testRemoveService() {
    ServiceRegistry registry;

    (void)registry.emplace<ConfigService>(10);

    LAMP_ASSERT(registry.remove<ConfigService>());
    LAMP_ASSERT(!registry.contains<ConfigService>());
    LAMP_ASSERT(registry.empty());
}

// ============================================================
// Test 17: Remove Missing Service
// ============================================================

void testRemoveMissingService() {
    ServiceRegistry registry;

    LAMP_ASSERT(!registry.remove<ConfigService>());
    LAMP_ASSERT(registry.empty());
}

// ============================================================
// Test 18: Ownership Survives Removal
// ============================================================

void testSharedOwnershipSurvivesRemoval() {
    ServiceRegistry registry;

    auto service = registry.emplace<ConfigService>(99);
    auto retained = registry.resolve<ConfigService>();

    LAMP_ASSERT(registry.remove<ConfigService>());

    LAMP_ASSERT_EQ(retained->value, 99);
    LAMP_ASSERT(retained == service);
}

// ============================================================
// Test 19: Clear Registry
// ============================================================

void testClearRegistry() {
    ServiceRegistry registry;

    (void)registry.emplace<ServiceA>();
    (void)registry.emplace<ServiceB>();
    (void)registry.emplace<ServiceC>();

    registry.clear();

    LAMP_ASSERT(registry.empty());
    LAMP_ASSERT(!registry.contains<ServiceA>());
    LAMP_ASSERT(!registry.contains<ServiceB>());
    LAMP_ASSERT(!registry.contains<ServiceC>());
}

// ============================================================
// Test 20: Ownership Survives Clear
// ============================================================

void testSharedOwnershipSurvivesClear() {
    ServiceRegistry registry;

    auto retained = registry.emplace<ConfigService>(123);

    registry.clear();

    LAMP_ASSERT(registry.empty());
    LAMP_ASSERT_EQ(retained->value, 123);
}

// ============================================================
// Test 21: Close Registry
// ============================================================

void testCloseRegistry() {
    ServiceRegistry registry;

    registry.close();

    LAMP_ASSERT(registry.isClosed());

    expectRegistryError(
        [&]() {
            (void)registry.emplace<ConfigService>(1);
        },
        ServiceRegistryErrorCode::RegistryClosed
    );
}

// ============================================================
// Test 22: Resolve After Close
// ============================================================

void testResolveAfterClose() {
    ServiceRegistry registry;

    (void)registry.emplace<ConfigService>(50);
    registry.close();

    LAMP_ASSERT_EQ(
        registry.resolve<ConfigService>()->value,
        50
    );
}

// ============================================================
// Test 23: Mutations Rejected After Close
// ============================================================

void testMutationsRejectedAfterClose() {
    ServiceRegistry registry;

    (void)registry.emplace<ConfigService>(1);
    registry.close();

    expectRegistryError(
        [&]() {
            registry.replace(
                std::make_shared<ConfigService>(2)
            );
        },
        ServiceRegistryErrorCode::RegistryClosed
    );

    expectRegistryError(
        [&]() {
            (void)registry.remove<ConfigService>();
        },
        ServiceRegistryErrorCode::RegistryClosed
    );
}

// ============================================================
// Test 24: Clear After Close
// ============================================================

void testClearAfterClose() {
    ServiceRegistry registry;

    (void)registry.emplace<ServiceA>();
    (void)registry.emplace<ServiceB>();

    registry.close();
    registry.clear();

    LAMP_ASSERT(registry.empty());
    LAMP_ASSERT(registry.isClosed());
}

// ============================================================
// Test 25: Destruction On Removal
// ============================================================

void testDestructionOnRemoval() {
    std::atomic<int> destroyed{0};

    ServiceRegistry registry;

    registry.registerService(
        std::make_shared<DestructionTracker>(destroyed)
    );

    LAMP_ASSERT_EQ(destroyed.load(), 0);
    LAMP_ASSERT(registry.remove<DestructionTracker>());
    LAMP_ASSERT_EQ(destroyed.load(), 1);
}

// ============================================================
// Test 26: Reentrant Destruction On Removal
// ============================================================

void testReentrantServiceDestruction() {
    ServiceRegistry registry;
    std::atomic<bool> callbackExecuted{false};

    auto service =
        std::make_shared<ReentrantDestructionService>();

    service->registry = &registry;
    service->callbackExecuted = &callbackExecuted;

    registry.registerService(service);
    service.reset();

    LAMP_ASSERT(
        registry.remove<ReentrantDestructionService>()
    );

    LAMP_ASSERT(callbackExecuted.load());
}

// ============================================================
// Test 27: Concurrent Resolution
// ============================================================

void testConcurrentResolution() {
    ServiceRegistry registry;

    auto counter = registry.emplace<CounterService>();

    constexpr int threadCount = 8;
    constexpr int iterations = 1000;

    std::vector<std::thread> threads;

    for (int i = 0; i < threadCount; ++i) {
        threads.emplace_back([&]() {
            for (int j = 0; j < iterations; ++j) {
                registry.resolve<CounterService>()->increment();
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    LAMP_ASSERT_EQ(
        counter->get(),
        threadCount * iterations
    );
}

// ============================================================
// Test 28: Concurrent Replacement And Resolution
// ============================================================

void testConcurrentRegistrationAndResolution() {
    ServiceRegistry registry;

    (void)registry.emplace<ConfigService>(0);

    constexpr int iterations = 500;
    constexpr int readerCount = 6;

    std::atomic<bool> start{false};
    std::atomic<bool> valid{true};

    std::thread writer([&]() {
        while (!start.load()) {
            std::this_thread::yield();
        }

        for (int i = 1; i <= iterations; ++i) {
            registry.replace(
                std::make_shared<ConfigService>(i)
            );
        }
    });

    std::vector<std::thread> readers;

    for (int i = 0; i < readerCount; ++i) {
        readers.emplace_back([&]() {
            while (!start.load()) {
                std::this_thread::yield();
            }

            for (int j = 0; j < iterations; ++j) {
                auto service =
                    registry.resolve<ConfigService>();

                if (!service ||
                    service->value < 0 ||
                    service->value > iterations) {
                    valid.store(false);
                }
            }
        });
    }

    start.store(true);

    writer.join();

    for (auto& reader : readers) {
        reader.join();
    }

    LAMP_ASSERT(valid.load());
    LAMP_ASSERT_EQ(
        registry.resolve<ConfigService>()->value,
        iterations
    );
}

// ============================================================
// Test 29: Register Const-Only Service
// ============================================================

void testRegisterConstOnlyService() {
    ServiceRegistry registry;

    std::shared_ptr<const ConfigService> service =
        std::make_shared<const ConfigService>(101);

    registry.registerService(service);

    LAMP_ASSERT(registry.contains<ConfigService>());
    LAMP_ASSERT(registry.contains<const ConfigService>());
    LAMP_ASSERT(!registry.isMutable<ConfigService>());

    auto resolved =
        registry.resolve<const ConfigService>();

    LAMP_ASSERT(resolved == service);
    LAMP_ASSERT_EQ(resolved->value, 101);
}

// ============================================================
// Test 30: Mutable Resolution Of Const Service Rejected
// ============================================================

void testMutableResolutionOfConstServiceRejected() {
    ServiceRegistry registry;

    registry.registerService(
        std::make_shared<const ConfigService>(202)
    );

    expectRegistryError(
        [&]() {
            (void)registry.resolve<ConfigService>();
        },
        ServiceRegistryErrorCode::ConstViolation
    );

    LAMP_ASSERT(
        !registry.tryResolve<ConfigService>()
    );

    LAMP_ASSERT(
        registry.tryResolve<const ConfigService>()
    );
}

// ============================================================
// Test 31: Mutable Registration Supports Const Access
// ============================================================

void testMutableRegistrationSupportsConstAccess() {
    ServiceRegistry registry;

    auto service =
        registry.emplace<ConfigService>(303);

    LAMP_ASSERT(registry.isMutable<ConfigService>());

    auto mutableService =
        registry.resolve<ConfigService>();

    auto constService =
        registry.resolve<const ConfigService>();

    LAMP_ASSERT(mutableService == service);
    LAMP_ASSERT(constService.get() == service.get());

    mutableService->value = 404;

    LAMP_ASSERT_EQ(constService->value, 404);
}

// ============================================================
// Test 32: Replace Mutable With Const-Only
// ============================================================

void testReplaceMutableWithConstOnly() {
    ServiceRegistry registry;

    auto original =
        registry.emplace<ConfigService>(10);

    std::shared_ptr<const ConfigService> replacement =
        std::make_shared<const ConfigService>(20);

    registry.replace(replacement);

    LAMP_ASSERT(!registry.isMutable<ConfigService>());

    LAMP_ASSERT_EQ(
        registry.resolve<const ConfigService>()->value,
        20
    );

    expectRegistryError(
        [&]() {
            (void)registry.resolve<ConfigService>();
        },
        ServiceRegistryErrorCode::ConstViolation
    );

    LAMP_ASSERT_EQ(original->value, 10);
}

// ============================================================
// Test 33: Replace Const-Only With Mutable
// ============================================================

void testReplaceConstOnlyWithMutable() {
    ServiceRegistry registry;

    registry.registerService(
        std::make_shared<const ConfigService>(10)
    );

    auto replacement =
        std::make_shared<ConfigService>(30);

    registry.replace(replacement);

    LAMP_ASSERT(registry.isMutable<ConfigService>());

    auto resolved =
        registry.resolve<ConfigService>();

    LAMP_ASSERT(resolved == replacement);

    resolved->value = 40;

    LAMP_ASSERT_EQ(
        registry.resolve<const ConfigService>()->value,
        40
    );
}

// ============================================================
// Test 34: Failed Replacement Preserves Service
// ============================================================

void testFailedReplacementPreservesService() {
    ServiceRegistry registry;

    auto original =
        registry.emplace<ConfigService>(55);

    std::shared_ptr<ConfigService> nullReplacement;

    expectRegistryError(
        [&]() {
            registry.replace(nullReplacement);
        },
        ServiceRegistryErrorCode::NullService
    );

    LAMP_ASSERT(
        registry.resolve<ConfigService>() == original
    );

    registry.close();

    expectRegistryError(
        [&]() {
            registry.replace(
                std::make_shared<ConfigService>(99)
            );
        },
        ServiceRegistryErrorCode::RegistryClosed
    );

    LAMP_ASSERT(
        registry.resolve<ConfigService>() == original
    );
}

// ============================================================
// Test 35: Reentrant Destruction On Replacement
// ============================================================

void testReentrantDestructionOnReplacement() {
    ServiceRegistry registry;

    std::atomic<bool> callbackExecuted{false};

    auto original =
        std::make_shared<ReentrantDestructionService>();

    original->registry = &registry;
    original->callbackExecuted = &callbackExecuted;

    registry.registerService(original);
    original.reset();

    auto replacement =
        std::make_shared<ReentrantDestructionService>();

    registry.replace(replacement);

    LAMP_ASSERT(callbackExecuted.load());

    // Prevent the replacement's destructor from accessing
    // the registry during its own destruction.
    replacement->registry = nullptr;
    replacement->callbackExecuted = nullptr;

    registry.clear();
}

// ============================================================
// Test 36: Concurrent Const Resolution And Replacement
// ============================================================

void testConcurrentConstResolutionAndReplacement() {
    ServiceRegistry registry;

    registry.registerService(
        std::make_shared<const ConfigService>(0)
    );

    constexpr int iterations = 1000;
    constexpr int readerCount = 8;

    std::atomic<bool> start{false};
    std::atomic<bool> valid{true};

    std::thread writer([&]() {
        while (!start.load()) {
            std::this_thread::yield();
        }

        for (int i = 1; i <= iterations; ++i) {
            if ((i % 2) == 0) {
                registry.replace(
                    std::make_shared<ConfigService>(i)
                );
            } else {
                registry.replace(
                    std::make_shared<const ConfigService>(i)
                );
            }
        }
    });

    std::vector<std::thread> readers;

    for (int i = 0; i < readerCount; ++i) {
        readers.emplace_back([&]() {
            while (!start.load()) {
                std::this_thread::yield();
            }

            for (int j = 0; j < iterations; ++j) {
                auto service =
                    registry.resolve<const ConfigService>();

                if (!service ||
                    service->value < 0 ||
                    service->value > iterations) {
                    valid.store(false);
                }
            }
        });
    }

    start.store(true);

    writer.join();

    for (auto& reader : readers) {
        reader.join();
    }

    LAMP_ASSERT(valid.load());

    LAMP_ASSERT_EQ(
        registry.resolve<const ConfigService>()->value,
        iterations
    );

    LAMP_ASSERT(registry.isMutable<ConfigService>());
}

// ============================================================
// Test Registration
// ============================================================

struct TestCase {
    std::string_view name;
    void (*execute)();
};

// ============================================================
// Test Runner
// ============================================================

int runTests() {
    const std::vector<TestCase> tests{
        {"Empty registry", testEmptyRegistry},
        {"Register existing service", testRegisterExistingService},
        {"Resolve registered service", testResolveRegisteredService},
        {"Resolve missing service", testResolveMissingService},
        {"Optional resolution", testOptionalResolution},
        {"Duplicate registration", testDuplicateRegistration},
        {"Null registration", testNullRegistration},
        {"Emplace service", testEmplaceService},
        {"Multiple service types", testMultipleServiceTypes},
        {"Interface registration", testInterfaceRegistration},
        {"Interface key isolation", testInterfaceKeyIsolation},
        {"Const service resolution", testConstServiceResolution},
        {"Replace existing service", testReplaceExistingService},
        {"Replace missing service", testReplaceMissingService},
        {"Replace with null", testReplaceWithNull},
        {"Remove service", testRemoveService},
        {"Remove missing service", testRemoveMissingService},
        {"Ownership survives removal", testSharedOwnershipSurvivesRemoval},
        {"Clear registry", testClearRegistry},
        {"Ownership survives clear", testSharedOwnershipSurvivesClear},
        {"Close registry", testCloseRegistry},
        {"Resolve after close", testResolveAfterClose},
        {"Mutations rejected after close", testMutationsRejectedAfterClose},
        {"Clear after close", testClearAfterClose},
        {"Destruction on removal", testDestructionOnRemoval},
        {"Reentrant destruction on removal", testReentrantServiceDestruction},
        {"Concurrent resolution", testConcurrentResolution},
        {"Concurrent replacement and resolution",
         testConcurrentRegistrationAndResolution},
        {"Register const-only service", testRegisterConstOnlyService},
        {"Mutable resolution of const service rejected",
         testMutableResolutionOfConstServiceRejected},
        {"Mutable registration supports const access",
         testMutableRegistrationSupportsConstAccess},
        {"Replace mutable with const-only",
         testReplaceMutableWithConstOnly},
        {"Replace const-only with mutable",
         testReplaceConstOnlyWithMutable},
        {"Failed replacement preserves service",
         testFailedReplacementPreservesService},
        {"Reentrant destruction on replacement",
         testReentrantDestructionOnReplacement},
        {"Concurrent const resolution and replacement",
         testConcurrentConstResolutionAndReplacement}
    };

    std::size_t passed = 0;
    std::size_t failed = 0;

    std::cout
        << "\n========================================\n"
        << " Lamp Service Registry Tests\n"
        << "========================================\n\n";

    for (const auto& test : tests) {
        try {
            test.execute();
            ++passed;
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failed;
            std::cout
                << "[FAIL] " << test.name
                << "\n       " << error.what() << '\n';
        } catch (...) {
            ++failed;
            std::cout
                << "[FAIL] " << test.name
                << "\n       Unknown exception\n";
        }
    }

    std::cout
        << "\n========================================\n"
        << " Passed: " << passed << '\n'
        << " Failed: " << failed << '\n'
        << " Total : " << tests.size() << '\n'
        << "========================================\n";

    return failed == 0
        ? EXIT_SUCCESS
        : EXIT_FAILURE;
}

} // anonymous namespace

int main() {
    return runTests();
}
