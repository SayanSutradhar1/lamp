
#include "bridge/execution_dispatcher.hpp"
#include "bridge/http_transport_adapter.hpp"
#include "bridge/operation_registry.hpp"

#include "http/http_request.hpp"
#include "http/http_response.hpp"
#include "http/http_router.hpp"
#include "http/http_server.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace lamp::bridge;
using namespace lamp::http;

using namespace std::chrono_literals;

// ============================================================
// Minimal test framework
// ============================================================

class TestFailure final : public std::runtime_error {
public:
    explicit TestFailure(const std::string& message)
        : std::runtime_error(message) {}
};

#define LAMP_CHECK(condition)                              \
    do {                                                   \
        if (!(condition)) {                                \
            throw TestFailure(                             \
                std::string("Assertion failed: ") +        \
                #condition + " at line " +                 \
                std::to_string(__LINE__)                   \
            );                                             \
        }                                                  \
    } while (false)

#define LAMP_CHECK_EQ(actual, expected)                    \
    LAMP_CHECK((actual) == (expected))

using TestFunction = void (*)();

struct TestCase final {
    const char* name;
    TestFunction function;
};

std::vector<TestCase>& Tests() {
    static std::vector<TestCase> tests;
    return tests;
}

struct TestRegistrar final {
    TestRegistrar(
        const char* name,
        TestFunction function
    ) {
        Tests().push_back({name, function});
    }
};

#define LAMP_TEST(name)                                    \
    void name();                                           \
    TestRegistrar registrar_##name(#name, &name);         \
    void name()

// ============================================================
// Test helpers
// ============================================================

ResultEnvelope::Payload Bytes(
    std::string_view text
) {
    return {
        text.begin(),
        text.end()
    };
}

HttpRequest::Body HttpBody(std::string_view text) {
    HttpRequest::Body body;

    body.reserve(text.size());

    for (const unsigned char character : text) {
        body.push_back(
            static_cast<std::byte>(character)
        );
    }

    return body;
}

std::string Text(
    const ResultEnvelope::Payload& payload
) {
    return {
        payload.begin(),
        payload.end()
    };
}

RequestEnvelope MakeRequest(
    std::string operation_id,
    std::string_view json = "{}"
) {
    RequestEnvelope request;

    request.request_id = 42;
    request.operation_id = std::move(operation_id);
    request.transport = TransportKind::Internal;
    request.format = SerializationFormat::Json;
    request.payload = Bytes(json);

    return request;
}

OperationDescriptor MakeDescriptor(
    std::string id,
    ExecutionMode mode = ExecutionMode::Native
) {
    OperationDescriptor descriptor;

    descriptor.id = std::move(id);
    descriptor.execution.mode = mode;
    descriptor.input_format = SerializationFormat::Json;
    descriptor.output_format = SerializationFormat::Json;

    return descriptor;
}

// ============================================================
// Native test handler
// ============================================================

class NativeHandler final
    : public INativeOperationHandler {
public:
    using Callback = std::function<
        ResultEnvelope(const RequestEnvelope&)
    >;

    explicit NativeHandler(Callback callback)
        : callback_(std::move(callback)) {}

    ResultEnvelope Execute(
        const RequestEnvelope& request
    ) override {
        calls_.fetch_add(
            1,
            std::memory_order_relaxed
        );

        return callback_(request);
    }

    std::uint64_t Calls() const noexcept {
        return calls_.load(
            std::memory_order_relaxed
        );
    }

private:
    Callback callback_;
    std::atomic<std::uint64_t> calls_{0};
};

std::shared_ptr<NativeHandler> EchoHandler() {
    return std::make_shared<NativeHandler>(
        [](const RequestEnvelope& request) {
            return ResultEnvelope::Success(
                request,
                request.payload,
                SerializationFormat::Json,
                ExecutionBackend::Native
            );
        }
    );
}

std::shared_ptr<NativeHandler> FixedHandler(
    std::string response
) {
    return std::make_shared<NativeHandler>(
        [response = std::move(response)](
            const RequestEnvelope& request
        ) {
            return ResultEnvelope::Success(
                request,
                Bytes(response),
                SerializationFormat::Json,
                ExecutionBackend::Native
            );
        }
    );
}

void RegisterNative(
    const std::shared_ptr<OperationRegistry>& registry,
    OperationDescriptor descriptor,
    std::shared_ptr<INativeOperationHandler> handler
) {
    OperationBackends backends;
    backends.native = std::move(handler);

    const auto result = registry->Register(
        std::move(descriptor),
        std::move(backends)
    );

    LAMP_CHECK(result.Ok());
}

// ============================================================
// Registry tests
// ============================================================

LAMP_TEST(RegisterAndFindOperation) {
    auto registry =
        std::make_shared<OperationRegistry>();

    auto descriptor =
        MakeDescriptor("users.get");

    auto handler = EchoHandler();

    RegisterNative(
        registry,
        std::move(descriptor),
        handler
    );

    LAMP_CHECK_EQ(registry->Size(), 1U);
    LAMP_CHECK(registry->Contains("users.get"));

    auto registration =
        registry->Find("users.get");

    LAMP_CHECK(registration != nullptr);
    LAMP_CHECK_EQ(
        registration->descriptor.id,
        "users.get"
    );
    LAMP_CHECK(
        registration->backends.HasNative()
    );
}

LAMP_TEST(RejectDuplicateOperation) {
    OperationRegistry registry;

    auto handler = EchoHandler();

    OperationBackends first;
    first.native = handler;

    auto result = registry.Register(
        MakeDescriptor("users.get"),
        std::move(first)
    );

    LAMP_CHECK(result.Ok());

    OperationBackends second;
    second.native = handler;

    result = registry.Register(
        MakeDescriptor("users.get"),
        std::move(second)
    );

    LAMP_CHECK_EQ(
        result.status,
        RegistryStatus::AlreadyExists
    );

    LAMP_CHECK_EQ(registry.Size(), 1U);
}

LAMP_TEST(RejectMissingBackend) {
    OperationRegistry registry;

    auto result = registry.Register(
        MakeDescriptor("users.get"),
        OperationBackends{}
    );

    LAMP_CHECK_EQ(
        result.status,
        RegistryStatus::MissingBackend
    );
}

LAMP_TEST(RejectBackendMismatch) {
    OperationRegistry registry;

    OperationBackends backends;
    backends.native = EchoHandler();

    auto result = registry.Register(
        MakeDescriptor(
            "users.get",
            ExecutionMode::JavaScript
        ),
        std::move(backends)
    );

    LAMP_CHECK_EQ(
        result.status,
        RegistryStatus::BackendMismatch
    );
}

LAMP_TEST(RegistrySnapshotSurvivesUnregister) {
    OperationRegistry registry;

    OperationBackends backends;
    backends.native = EchoHandler();

    LAMP_CHECK(
        registry.Register(
            MakeDescriptor("users.get"),
            std::move(backends)
        ).Ok()
    );

    auto snapshot = registry.Find("users.get");

    LAMP_CHECK(snapshot != nullptr);

    LAMP_CHECK(
        registry.Unregister("users.get").Ok()
    );

    LAMP_CHECK(!registry.Contains("users.get"));

    // The old registration remains valid because the
    // snapshot owns a shared reference.
    LAMP_CHECK_EQ(
        snapshot->descriptor.id,
        "users.get"
    );
}

LAMP_TEST(RegistryReplacementPublishesNewSnapshot) {
    OperationRegistry registry;

    OperationBackends first;
    first.native = FixedHandler(R"({"version":1})");

    LAMP_CHECK(
        registry.Register(
            MakeDescriptor("config.get"),
            std::move(first)
        ).Ok()
    );

    auto old_snapshot =
        registry.Find("config.get");

    OperationBackends second;
    second.native = FixedHandler(R"({"version":2})");

    LAMP_CHECK(
        registry.Replace(
            MakeDescriptor("config.get"),
            std::move(second)
        ).Ok()
    );

    auto new_snapshot =
        registry.Find("config.get");

    LAMP_CHECK(old_snapshot != nullptr);
    LAMP_CHECK(new_snapshot != nullptr);

    LAMP_CHECK(
        new_snapshot->revision >
        old_snapshot->revision
    );

    LAMP_CHECK(
        old_snapshot != new_snapshot
    );
}

// ============================================================
// Dispatcher tests
// ============================================================

LAMP_TEST(NativeDispatchSuccess) {
    auto registry =
        std::make_shared<OperationRegistry>();

    auto handler = EchoHandler();

    RegisterNative(
        registry,
        MakeDescriptor("echo.run"),
        handler
    );

    ExecutionDispatcher dispatcher(registry);

    auto request = MakeRequest(
        "echo.run",
        R"({"value":123})"
    );

    const auto result =
        dispatcher.Dispatch(request);

    LAMP_CHECK(result.IsSuccess());

    LAMP_CHECK_EQ(
        result.backend,
        ExecutionBackend::Native
    );

    LAMP_CHECK_EQ(
        result.request_id,
        request.request_id
    );

    LAMP_CHECK_EQ(
        result.operation_id,
        request.operation_id
    );

    LAMP_CHECK_EQ(
        Text(result.payload),
        R"({"value":123})"
    );

    LAMP_CHECK_EQ(handler->Calls(), 1U);

    LAMP_CHECK(result.IsValid());
}

LAMP_TEST(UnknownOperationReturnsNotFound) {
    auto registry =
        std::make_shared<OperationRegistry>();

    ExecutionDispatcher dispatcher(registry);

    auto result = dispatcher.Dispatch(
        MakeRequest("missing.operation")
    );

    LAMP_CHECK(result.IsFailure());

    LAMP_CHECK_EQ(
        result.error.code,
        OperationErrorCode::NotFound
    );
}

LAMP_TEST(RejectInvalidJsonSerializationFormat) {
    auto registry =
        std::make_shared<OperationRegistry>();

    auto handler = EchoHandler();

    RegisterNative(
        registry,
        MakeDescriptor("echo.run"),
        handler
    );

    ExecutionDispatcher dispatcher(registry);

    auto request = MakeRequest("echo.run");

    request.format = SerializationFormat::Binary;

    auto result = dispatcher.Dispatch(request);

    LAMP_CHECK(result.IsFailure());

    LAMP_CHECK_EQ(
        result.error.code,
        OperationErrorCode::InvalidArgument
    );

    LAMP_CHECK_EQ(handler->Calls(), 0U);
}

LAMP_TEST(RejectOversizedInput) {
    auto registry =
        std::make_shared<OperationRegistry>();

    auto descriptor =
        MakeDescriptor("echo.small");

    descriptor.limits.max_input_bytes = 4;

    auto handler = EchoHandler();

    RegisterNative(
        registry,
        std::move(descriptor),
        handler
    );

    ExecutionDispatcher dispatcher(registry);

    auto result = dispatcher.Dispatch(
        MakeRequest("echo.small", R"({"large":true})")
    );

    LAMP_CHECK(result.IsFailure());

    LAMP_CHECK_EQ(
        result.error.code,
        OperationErrorCode::InvalidArgument
    );

    LAMP_CHECK_EQ(handler->Calls(), 0U);
}

LAMP_TEST(RejectOversizedOutput) {
    auto registry =
        std::make_shared<OperationRegistry>();

    auto descriptor =
        MakeDescriptor("echo.large");

    descriptor.limits.max_output_bytes = 4;

    RegisterNative(
        registry,
        std::move(descriptor),
        FixedHandler(R"({"large":true})")
    );

    ExecutionDispatcher dispatcher(registry);

    auto result = dispatcher.Dispatch(
        MakeRequest("echo.large")
    );

    LAMP_CHECK(result.IsFailure());

    LAMP_CHECK_EQ(
        result.error.code,
        OperationErrorCode::Internal
    );

    LAMP_CHECK_EQ(
        result.error.identifier,
        "INVALID_HANDLER_RESULT"
    );
}

LAMP_TEST(CancelledRequestDoesNotExecute) {
    auto registry =
        std::make_shared<OperationRegistry>();

    auto handler = EchoHandler();

    RegisterNative(
        registry,
        MakeDescriptor("echo.cancel"),
        handler
    );

    ExecutionDispatcher dispatcher(registry);

    RequestCancellationSource source;

    auto request = MakeRequest("echo.cancel");

    request.cancellation = source.Token();

    source.Cancel();

    auto result = dispatcher.Dispatch(request);

    LAMP_CHECK(result.IsCancelled());

    LAMP_CHECK_EQ(handler->Calls(), 0U);
}

LAMP_TEST(ExpiredRequestDoesNotExecute) {
    auto registry =
        std::make_shared<OperationRegistry>();

    auto handler = EchoHandler();

    RegisterNative(
        registry,
        MakeDescriptor("echo.deadline"),
        handler
    );

    ExecutionDispatcher dispatcher(registry);

    auto request = MakeRequest("echo.deadline");

    request.deadline =
        RequestDeadline::After(-1ms);

    auto result = dispatcher.Dispatch(request);

    LAMP_CHECK(result.IsDeadlineExceeded());

    LAMP_CHECK_EQ(handler->Calls(), 0U);
}

LAMP_TEST(NativeHandlerExceptionIsContained) {
    auto registry =
        std::make_shared<OperationRegistry>();

    auto handler = std::make_shared<NativeHandler>(
        [](const RequestEnvelope&)
            -> ResultEnvelope {
            throw std::runtime_error(
                "Sensitive internal exception"
            );
        }
    );

    RegisterNative(
        registry,
        MakeDescriptor("echo.throw"),
        handler
    );

    ExecutionDispatcher dispatcher(registry);

    auto result = dispatcher.Dispatch(
        MakeRequest("echo.throw")
    );

    LAMP_CHECK(result.IsFailure());

    LAMP_CHECK_EQ(
        result.error.code,
        OperationErrorCode::ExecutionFailed
    );

    LAMP_CHECK(
        result.error.message.find("Sensitive") ==
        std::string::npos
    );
}

LAMP_TEST(RejectMalformedHandlerIdentity) {
    auto registry =
        std::make_shared<OperationRegistry>();

    auto handler = std::make_shared<NativeHandler>(
        [](const RequestEnvelope& request) {
            auto result = ResultEnvelope::Success(
                request,
                Bytes("{}"),
                SerializationFormat::Json,
                ExecutionBackend::Native
            );

            result.operation_id = "another.operation";

            return result;
        }
    );

    RegisterNative(
        registry,
        MakeDescriptor("echo.identity"),
        handler
    );

    ExecutionDispatcher dispatcher(registry);

    auto result = dispatcher.Dispatch(
        MakeRequest("echo.identity")
    );

    LAMP_CHECK(result.IsFailure());

    LAMP_CHECK_EQ(
        result.error.identifier,
        "INVALID_HANDLER_RESULT"
    );
}

LAMP_TEST(AdaptivePrefersNativeWhenAvailable) {
    auto registry =
        std::make_shared<OperationRegistry>();

    auto handler = EchoHandler();

    OperationBackends backends;
    backends.native = handler;
    backends.javascript.handler_id = 101;

    LAMP_CHECK(
        registry->Register(
            MakeDescriptor(
                "echo.adaptive",
                ExecutionMode::Adaptive
            ),
            std::move(backends)
        ).Ok()
    );

    ExecutionDispatcher dispatcher(registry);

    auto result = dispatcher.Dispatch(
        MakeRequest("echo.adaptive")
    );

    LAMP_CHECK(result.IsSuccess());

    LAMP_CHECK_EQ(
        result.backend,
        ExecutionBackend::Native
    );
}

LAMP_TEST(JavaScriptRequiresExecutor) {
    auto registry =
        std::make_shared<OperationRegistry>();

    OperationBackends backends;
    backends.javascript.handler_id = 202;

    LAMP_CHECK(
        registry->Register(
            MakeDescriptor(
                "echo.javascript",
                ExecutionMode::JavaScript
            ),
            std::move(backends)
        ).Ok()
    );

    // No JavaScript executor has been installed.
    ExecutionDispatcher dispatcher(registry);

    auto result = dispatcher.Dispatch(
        MakeRequest("echo.javascript")
    );

    LAMP_CHECK(result.IsFailure());

    LAMP_CHECK_EQ(
        result.error.code,
        OperationErrorCode::Unavailable
    );
}

// ============================================================
// Concurrency tests
// ============================================================

LAMP_TEST(SerializedOperationRejectsConcurrentCall) {
    auto registry =
        std::make_shared<OperationRegistry>();

    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};

    auto handler = std::make_shared<NativeHandler>(
        [&](const RequestEnvelope& request) {
            entered.store(
                true,
                std::memory_order_release
            );

            while (!release.load(
                std::memory_order_acquire
            )) {
                std::this_thread::yield();
            }

            return ResultEnvelope::Success(
                request,
                Bytes("{}"),
                SerializationFormat::Json,
                ExecutionBackend::Native
            );
        }
    );

    auto descriptor =
        MakeDescriptor("echo.serialized");

    descriptor.execution.concurrency =
        ConcurrencyMode::Serialized;

    RegisterNative(
        registry,
        std::move(descriptor),
        handler
    );

    ExecutionDispatcher dispatcher(registry);

    ResultEnvelope first_result;

    std::thread worker([&] {
        first_result = dispatcher.Dispatch(
            MakeRequest("echo.serialized")
        );
    });

    // Ensure the worker always gets released and joined,
    // even if an assertion below fails.
    struct WorkerGuard final {
        std::atomic<bool>& release;
        std::thread& worker;

        ~WorkerGuard() {
            release.store(
                true,
                std::memory_order_release
            );

            if (worker.joinable()) {
                worker.join();
            }
        }
    } guard{release, worker};

    const auto deadline =
        std::chrono::steady_clock::now() + 2s;

    while (!entered.load(
        std::memory_order_acquire
    )) {
        if (std::chrono::steady_clock::now() >=
            deadline) {
            throw TestFailure(
                "Native worker did not enter handler"
            );
        }

        std::this_thread::yield();
    }

    auto second_result = dispatcher.Dispatch(
        MakeRequest("echo.serialized")
    );

    LAMP_CHECK(second_result.IsFailure());

    LAMP_CHECK_EQ(
        second_result.error.code,
        OperationErrorCode::ResourceExhausted
    );

    LAMP_CHECK_EQ(handler->Calls(), 1U);

    release.store(
        true,
        std::memory_order_release
    );

    worker.join();

    LAMP_CHECK(first_result.IsSuccess());
}

LAMP_TEST(ConcurrentRegistryLookups) {
    auto registry =
        std::make_shared<OperationRegistry>();

    RegisterNative(
        registry,
        MakeDescriptor("echo.concurrent"),
        EchoHandler()
    );

    constexpr int kThreads = 8;
    constexpr int kIterations = 1000;

    std::atomic<int> failures{0};

    std::vector<std::thread> workers;
    workers.reserve(kThreads);

    for (int i = 0; i < kThreads; ++i) {
        workers.emplace_back([&] {
            for (int j = 0; j < kIterations; ++j) {
                auto registration =
                    registry->Find("echo.concurrent");

                if (!registration ||
                    registration->descriptor.id !=
                        "echo.concurrent") {
                    failures.fetch_add(
                        1,
                        std::memory_order_relaxed
                    );
                }
            }
        });
    }

    for (auto& worker : workers) {
        worker.join();
    }

    LAMP_CHECK_EQ(failures.load(), 0);
}

// ============================================================
// HTTP adapter tests
// ============================================================

// These tests use the real HttpRouter and HttpServer route
// registration path, but do not open a TCP socket.
//
// Full socket-level integration tests should be added
// separately.

LAMP_TEST(HttpAdapterDispatchesNativeOperation) {
    auto registry =
        std::make_shared<OperationRegistry>();

    RegisterNative(
        registry,
        MakeDescriptor("users.get"),
        FixedHandler(R"({"id":42,"name":"Lamp"})")
    );

    auto dispatcher =
        std::make_shared<ExecutionDispatcher>(
            registry
        );

    HttpServer server;
    HttpTransportAdapter adapter(dispatcher);

    adapter.Register(
        server,
        {
            HttpMethod::Post,
            "/rpc/users/get",
            "users.get",
            SerializationFormat::Json
        }
    );

    HttpRequest request(
        HttpMethod::Post,
        "/rpc/users/get"
    );

    request.setBody(HttpBody(R"({"id":42})"));

    auto response =
        server.router()->dispatch(request);

    LAMP_CHECK_EQ(
        response.statusCode(),
        200
    );

    std::string body;

    for (const auto byte : response.body()) {
        body.push_back(
            static_cast<char>(
                std::to_integer<unsigned char>(byte)
            )
        );
    }

    LAMP_CHECK_EQ(
        body,
        R"({"id":42,"name":"Lamp"})"
    );
}

LAMP_TEST(HttpAdapterMapsMissingOperation) {
    auto registry =
        std::make_shared<OperationRegistry>();

    auto dispatcher =
        std::make_shared<ExecutionDispatcher>(
            registry
        );

    HttpServer server;
    HttpTransportAdapter adapter(dispatcher);

    adapter.Register(
        server,
        {
            HttpMethod::Post,
            "/rpc/missing",
            "missing.operation",
            SerializationFormat::Json
        }
    );

    HttpRequest request(
        HttpMethod::Post,
        "/rpc/missing"
    );

    request.setBody(HttpBody("{}"));

    auto response =
        server.router()->dispatch(request);

    LAMP_CHECK_EQ(
        response.statusCode(),
        404
    );
}

LAMP_TEST(HttpAdapterDoesNotExposeInternalErrors) {
    auto registry =
        std::make_shared<OperationRegistry>();

    auto handler = std::make_shared<NativeHandler>(
        [](const RequestEnvelope&)
            -> ResultEnvelope {
            throw std::runtime_error(
                "private_database_password"
            );
        }
    );

    RegisterNative(
        registry,
        MakeDescriptor("internal.fail"),
        handler
    );

    auto dispatcher =
        std::make_shared<ExecutionDispatcher>(
            registry
        );

    HttpServer server;
    HttpTransportAdapter adapter(dispatcher);

    adapter.Register(
        server,
        {
            HttpMethod::Post,
            "/rpc/internal/fail",
            "internal.fail",
            SerializationFormat::Json
        }
    );

    HttpRequest request(
        HttpMethod::Post,
        "/rpc/internal/fail"
    );

    request.setBody(HttpBody("{}"));

    auto response =
        server.router()->dispatch(request);

    LAMP_CHECK_EQ(
        response.statusCode(),
        500
    );

    std::string body;

    for (const auto byte : response.body()) {
        body.push_back(
            static_cast<char>(
                std::to_integer<unsigned char>(byte)
            )
        );
    }

    LAMP_CHECK(
        body.find("private_database_password") ==
        std::string::npos
    );
}

// ============================================================
// Test runner
// ============================================================

} // namespace

int main() {
    std::size_t passed = 0;
    std::size_t failed = 0;

    std::cout
        << "\n"
        << "========================================\n"
        << " Lamp Phase 4 - Bridge Integration Tests\n"
        << "========================================\n\n";

    for (const auto& test : Tests()) {
        try {
            test.function();

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
        << "\n----------------------------------------\n"
        << "Passed: " << passed << '\n'
        << "Failed: " << failed << '\n'
        << "Total:  " << passed + failed << '\n'
        << "----------------------------------------\n";

    return failed == 0 ? 0 : 1;
}
