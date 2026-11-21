
#include "bridge/async_http_operation_pump.hpp"
#include "bridge/async_http_transport_adapter.hpp"
#include "bridge/execution_dispatcher.hpp"
#include "bridge/operation_registry.hpp"

#include "execution/async_execution_dispatcher.hpp"
#include "execution/native_worker_pool.hpp"
#include "execution/v8_isolate_worker_pool.hpp"

#include "http/http_async_server.hpp"

#include <libplatform/libplatform.h>
#include <v8.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

using namespace std::chrono_literals;

namespace bridge = lamp::bridge;
namespace execution = lamp::execution;
namespace http = lamp::http;

void Check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

struct V8Runtime final {
    std::unique_ptr<v8::Platform> platform;

    V8Runtime() {
        platform = v8::platform::NewDefaultPlatform();
        v8::V8::InitializePlatform(platform.get());
        Check(v8::V8::Initialize(), "V8 initialization failed");
    }

    ~V8Runtime() {
        v8::V8::Dispose();
        v8::V8::DisposePlatform();
    }

    V8Runtime(const V8Runtime&) = delete;
    V8Runtime& operator=(const V8Runtime&) = delete;
};

class NativeGateHandler final
    : public bridge::INativeOperationHandler {
public:
    using Callback = std::function<
        bridge::ResultEnvelope(const bridge::RequestEnvelope&)
    >;

    explicit NativeGateHandler(Callback fn)
        : fn_(std::move(fn)) {}

    bridge::ResultEnvelope Execute(
        const bridge::RequestEnvelope& request
    ) override {
        return fn_(request);
    }

private:
    Callback fn_;
};

void TestMixedNativeAndV8AsyncHttpPipelining() {
    const auto ownerThreadId = std::this_thread::get_id();

    // 1. Native backend setup with a gated slow operation
    auto registry = std::make_shared<bridge::OperationRegistry>();

    std::promise<void> nativeEnteredPromise;
    auto nativeEntered = nativeEnteredPromise.get_future();
    std::promise<void> nativeReleasePromise;
    auto nativeRelease =
        nativeReleasePromise.get_future().share();

    {
        bridge::OperationDescriptor desc;
        desc.id = "native.slow";
        desc.execution.mode = bridge::ExecutionMode::Native;
        desc.input_format = bridge::SerializationFormat::Json;
        desc.output_format = bridge::SerializationFormat::Json;

        bridge::OperationBackends backends;
        backends.native = std::make_shared<NativeGateHandler>(
            [&](const bridge::RequestEnvelope& req) {
                Check(
                    std::this_thread::get_id() != ownerThreadId,
                    "Native operation ran on HTTP owner thread"
                );
                nativeEnteredPromise.set_value();
                nativeRelease.wait();

                const std::string body = R"({"backend":"native"})";
                bridge::ResultEnvelope::Payload payload(
                    body.begin(),
                    body.end()
                );
                return bridge::ResultEnvelope::Success(
                    req,
                    std::move(payload),
                    bridge::SerializationFormat::Json,
                    bridge::ExecutionBackend::Native
                );
            }
        );

        Check(
            registry->Register(std::move(desc), std::move(backends))
                .Ok(),
            "Failed to register native.slow"
        );
    }

    auto syncDispatcher =
        std::make_shared<bridge::ExecutionDispatcher>(registry);

    execution::NativeWorkerPoolConfig nativeConfig;
    nativeConfig.worker_count = 1;
    nativeConfig.max_pending_tasks = 16;
    auto nativePool =
        std::make_shared<execution::NativeWorkerPool>(
            syncDispatcher,
            nativeConfig
        );

    // 2. V8 isolate worker pool with Promise execution and
    //    strict worker thread affinity verification
    std::atomic<int> jsExecutorCalls{0};
    std::atomic<int> jsMapperCalls{0};

    auto jsExecutor =
        [&](
            const bridge::RequestEnvelope& request,
            const execution::V8WorkerExecutionContext& worker
        ) -> execution::V8WorkerAsyncResult {
            ++jsExecutorCalls;

            Check(
                std::this_thread::get_id() != ownerThreadId,
                "V8 executor ran on HTTP owner thread"
            );
            Check(
                worker.isolate != nullptr &&
                v8::Isolate::GetCurrent() == worker.isolate,
                "V8 executor missing current worker isolate"
            );

            std::string inputText(
                request.payload.begin(),
                request.payload.end()
            );

            const std::string jsSource =
                "Promise.resolve(" +
                (inputText.empty() ? std::string("{}") : inputText) +
                ").then(v => JSON.stringify({backend:'v8',echo:v}))";

            v8::Local<v8::String> source =
                v8::String::NewFromUtf8(
                    worker.isolate,
                    jsSource.data(),
                    v8::NewStringType::kNormal,
                    static_cast<int>(jsSource.size())
                ).ToLocalChecked();

            v8::Local<v8::Script> script =
                v8::Script::Compile(worker.context, source)
                    .ToLocalChecked();

            v8::Local<v8::Value> value =
                script->Run(worker.context).ToLocalChecked();

            Check(
                value->IsPromise(),
                "Expected JS script to return a Promise"
            );

            return execution::V8WorkerAsyncResult::Pending(
                value.As<v8::Promise>()
            );
        };

    auto jsMapper =
        [&](
            const bridge::RequestEnvelope& request,
            execution::V8PromiseSettlementKind kind,
            v8::Local<v8::Value> value
        ) -> bridge::ResultEnvelope {
            ++jsMapperCalls;

            Check(
                std::this_thread::get_id() != ownerThreadId,
                "V8 mapper ran on HTTP owner thread"
            );

            if (kind != execution::V8PromiseSettlementKind::Fulfilled) {
                return bridge::ResultEnvelope::Failure(
                    request,
                    {
                        bridge::OperationErrorCode::ExecutionFailed,
                        "Promise rejected",
                        "JS_PROMISE_REJECTED",
                        false
                    },
                    bridge::ExecutionBackend::JavaScript
                );
            }

            v8::Isolate* isolate = v8::Isolate::GetCurrent();
            Check(isolate != nullptr, "V8 isolate not current in mapper");

            v8::String::Utf8Value utf8(isolate, value);
            const std::string json =
                (*utf8 != nullptr) ? std::string(*utf8, utf8.length())
                                   : std::string("{}");

            bridge::ResultEnvelope::Payload payload(
                json.begin(),
                json.end()
            );
            return bridge::ResultEnvelope::Success(
                request,
                std::move(payload),
                bridge::SerializationFormat::Json,
                bridge::ExecutionBackend::JavaScript
            );
        };

    execution::V8IsolateWorkerPoolConfig v8Config;
    v8Config.worker_count = 1;
    v8Config.max_pending_tasks = 16;
    v8Config.max_heap_size_mb = 128;
    v8Config.drain_on_shutdown = true;

    execution::V8WorkerAsyncExecutionConfig v8AsyncConfig;
    v8AsyncConfig.max_pending_continuations_per_worker = 16;
    v8AsyncConfig.microtask_checkpoints_per_pump = 2;
    v8AsyncConfig.idle_pump_interval = 2ms;

    auto v8Pool = std::make_shared<execution::V8IsolateWorkerPool>(
        jsExecutor,
        jsMapper,
        v8Config,
        v8AsyncConfig
    );

    // 3. Combined AsyncExecutionDispatcher + Pump + Adapter + HttpAsyncConnection
    auto asyncDispatcher =
        std::make_shared<execution::AsyncExecutionDispatcher>(
            nativePool,
            v8Pool
        );

    auto coordinator =
        std::make_shared<bridge::AsyncOperationCoordinator>(
            asyncDispatcher,
            32
        );
    auto queue = std::make_shared<http::HttpCompletionQueue>();
    auto pump = std::make_shared<bridge::AsyncHttpOperationPump>(
        coordinator,
        queue,
        bridge::AsyncHttpTransportAdapter::DefaultResponseMapper()
    );

    bridge::AsyncHttpTransportAdapter adapter(pump);

    execution::AsyncScheduleOptions nativeOpts;
    nativeOpts.target = execution::TaskExecutionTarget::NativeWorker;
    adapter.Register(
        {
            http::HttpMethod::Get,
            "/native/slow",
            "native.slow",
            bridge::SerializationFormat::Json
        },
        nativeOpts
    );

    execution::AsyncScheduleOptions jsOpts;
    jsOpts.target = execution::TaskExecutionTarget::JavaScriptWorker;
    adapter.Register(
        {
            http::HttpMethod::Post,
            "/js/echo",
            "js.echo",
            bridge::SerializationFormat::Json
        },
        jsOpts
    );

    std::shared_ptr<http::HttpAsyncConnection> conn;
    conn = std::make_shared<http::HttpAsyncConnection>(
        1,
        [&adapter, &conn](
            const http::HttpCompletionToken& token,
            http::HttpRequest request
        ) {
            adapter.Dispatch(token, conn, request);
        }
    );

    const std::string pipelined =
        "GET /native/slow HTTP/1.1\r\nHost: localhost\r\n\r\n"
        "POST /js/echo HTTP/1.1\r\nHost: localhost\r\n"
        "Content-Length: 14\r\n\r\n"
        R"({"user":"ada"})";

    const auto recv = conn->receive(pipelined);
    Check(recv.dispatched == 2, "Both requests dispatched");
    Check(!recv.failed(), "No parse errors");

    Check(
        nativeEntered.wait_for(3s) == std::future_status::ready,
        "Native slow handler did not start"
    );

    // Wait for V8 JS Promise task to finish while Native slow task is gated
    const auto jsDeadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < jsDeadline) {
        (void)pump->Advance();
        if (pump->Outstanding() == 1) {
            break;
        }
        std::this_thread::sleep_for(2ms);
    }

    Check(
        pump->Outstanding() == 1,
        "V8 Promise task should finish before gated native task"
    );
    Check(
        !conn->hasPendingOutput(),
        "V8 response must wait behind earlier native request"
    );

    // Release native task and drain both responses
    nativeReleasePromise.set_value();

    const auto allDeadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < allDeadline) {
        (void)pump->Advance();
        if (pump->Outstanding() == 0 && conn->inflight() == 0) {
            break;
        }
        std::this_thread::sleep_for(2ms);
    }

    Check(pump->Outstanding() == 0, "All tasks completed");
    Check(conn->inflight() == 0, "All HTTP slots flushed");
    Check(jsExecutorCalls.load() == 1, "JS executor ran once");
    Check(jsMapperCalls.load() == 1, "JS mapper ran once");

    const std::string wire(conn->pendingOutput());
    const auto nativePos = wire.find(R"({"backend":"native"})");
    const auto jsPos =
        wire.find(R"({"backend":"v8","echo":{"user":"ada"}})");

    Check(nativePos != std::string::npos, "Native response missing");
    Check(jsPos != std::string::npos, "V8 JS response missing");
    Check(
        nativePos < jsPos,
        "HTTP/1.1 response ordering violated across Native and V8 backends"
    );

    pump->Close();
    asyncDispatcher->Shutdown();
}

void TestLiveTcpMixedNativeAndV8AsyncHttpServer() {
#ifdef _WIN32
    WSADATA wsaData{};
    Check(::WSAStartup(MAKEWORD(2, 2), &wsaData) == 0, "WSAStartup failed");
    struct WsaGuard final {
        ~WsaGuard() { ::WSACleanup(); }
    } wsaGuard;
    using SockType = SOCKET;
    constexpr SockType kInvalid = INVALID_SOCKET;
    auto closeSock = [](SockType s) {
        if (s != INVALID_SOCKET) { ::closesocket(s); }
    };
#else
    using SockType = int;
    constexpr SockType kInvalid = -1;
    auto closeSock = [](SockType s) {
        if (s != -1) { ::close(s); }
    };
#endif

    const auto mainThreadId = std::this_thread::get_id();
    auto registry = std::make_shared<bridge::OperationRegistry>();

    std::promise<void> nativeEnteredPromise;
    auto nativeEntered = nativeEnteredPromise.get_future();
    std::promise<void> nativeReleasePromise;
    auto nativeRelease = nativeReleasePromise.get_future().share();
    std::once_flag nativeOnce;

    {
        bridge::OperationDescriptor desc;
        desc.id = "live.native.slow";
        desc.execution.mode = bridge::ExecutionMode::Native;
        desc.input_format = bridge::SerializationFormat::Json;
        desc.output_format = bridge::SerializationFormat::Json;

        bridge::OperationBackends backends;
        backends.native = std::make_shared<NativeGateHandler>(
            [&](const bridge::RequestEnvelope& req) {
                std::call_once(nativeOnce, [&]() {
                    nativeEnteredPromise.set_value();
                });
                nativeRelease.wait();
                const std::string body = R"({"layer":"native"})";
                bridge::ResultEnvelope::Payload payload(
                    body.begin(),
                    body.end()
                );
                return bridge::ResultEnvelope::Success(
                    req,
                    std::move(payload),
                    bridge::SerializationFormat::Json,
                    bridge::ExecutionBackend::Native
                );
            }
        );

        Check(
            registry->Register(std::move(desc), std::move(backends))
                .Ok(),
            "Failed to register live.native.slow"
        );
    }

    auto syncDispatcher =
        std::make_shared<bridge::ExecutionDispatcher>(registry);

    execution::NativeWorkerPoolConfig nativeConfig;
    nativeConfig.worker_count = 1;
    auto nativePool =
        std::make_shared<execution::NativeWorkerPool>(
            syncDispatcher,
            nativeConfig
        );

    std::atomic<int> jsRuns{0};
    auto jsExecutor =
        [&](
            const bridge::RequestEnvelope& request,
            const execution::V8WorkerExecutionContext& worker
        ) -> execution::V8WorkerAsyncResult {
            ++jsRuns;
            Check(
                std::this_thread::get_id() != mainThreadId,
                "V8 executor ran on main thread"
            );
            Check(
                worker.isolate != nullptr &&
                v8::Isolate::GetCurrent() == worker.isolate,
                "V8 isolate affinity check failed"
            );

            std::string inputText(
                request.payload.begin(),
                request.payload.end()
            );
            const std::string jsSource =
                "Promise.resolve(" +
                (inputText.empty() ? std::string("{}") : inputText) +
                ").then(v => JSON.stringify({layer:'v8',echo:v}))";

            v8::Local<v8::String> source =
                v8::String::NewFromUtf8(
                    worker.isolate,
                    jsSource.data(),
                    v8::NewStringType::kNormal,
                    static_cast<int>(jsSource.size())
                ).ToLocalChecked();

            v8::Local<v8::Script> script =
                v8::Script::Compile(worker.context, source)
                    .ToLocalChecked();
            v8::Local<v8::Value> value =
                script->Run(worker.context).ToLocalChecked();
            Check(value->IsPromise(), "Expected Promise from JS");
            return execution::V8WorkerAsyncResult::Pending(
                value.As<v8::Promise>()
            );
        };

    auto jsMapper =
        [](
            const bridge::RequestEnvelope& request,
            execution::V8PromiseSettlementKind kind,
            v8::Local<v8::Value> value
        ) -> bridge::ResultEnvelope {
            if (kind != execution::V8PromiseSettlementKind::Fulfilled) {
                return bridge::ResultEnvelope::Failure(
                    request,
                    {
                        bridge::OperationErrorCode::ExecutionFailed,
                        "Promise rejected",
                        "JS_PROMISE_REJECTED",
                        false
                    },
                    bridge::ExecutionBackend::JavaScript
                );
            }
            v8::Isolate* isolate = v8::Isolate::GetCurrent();
            v8::String::Utf8Value utf8(isolate, value);
            const std::string json =
                (*utf8 != nullptr) ? std::string(*utf8, utf8.length())
                                   : std::string("{}");
            bridge::ResultEnvelope::Payload payload(
                json.begin(),
                json.end()
            );
            return bridge::ResultEnvelope::Success(
                request,
                std::move(payload),
                bridge::SerializationFormat::Json,
                bridge::ExecutionBackend::JavaScript
            );
        };

    execution::V8IsolateWorkerPoolConfig v8Config;
    v8Config.worker_count = 1;
    v8Config.max_heap_size_mb = 128;
    execution::V8WorkerAsyncExecutionConfig v8AsyncConfig;
    v8AsyncConfig.idle_pump_interval = 2ms;

    auto v8Pool = std::make_shared<execution::V8IsolateWorkerPool>(
        jsExecutor,
        jsMapper,
        v8Config,
        v8AsyncConfig
    );

    auto asyncDispatcher =
        std::make_shared<execution::AsyncExecutionDispatcher>(
            nativePool,
            v8Pool
        );
    auto coordinator =
        std::make_shared<bridge::AsyncOperationCoordinator>(
            asyncDispatcher,
            32
        );
    auto queue = std::make_shared<http::HttpCompletionQueue>();
    auto pump = std::make_shared<bridge::AsyncHttpOperationPump>(
        coordinator,
        queue,
        bridge::AsyncHttpTransportAdapter::DefaultResponseMapper()
    );

    bridge::AsyncHttpTransportAdapter adapter(pump);
    execution::AsyncScheduleOptions nativeOpts;
    nativeOpts.target = execution::TaskExecutionTarget::NativeWorker;
    adapter.Register(
        {
            http::HttpMethod::Get,
            "/native",
            "live.native.slow",
            bridge::SerializationFormat::Json
        },
        nativeOpts
    );

    execution::AsyncScheduleOptions jsOpts;
    jsOpts.target = execution::TaskExecutionTarget::JavaScriptWorker;
    adapter.Register(
        {
            http::HttpMethod::Post,
            "/js",
            "live.js.echo",
            bridge::SerializationFormat::Json
        },
        jsOpts
    );

    http::HttpAsyncServerConfig serverCfg;
    serverCfg.port = 0;
    serverCfg.pollTimeout = 200ms;
    http::HttpAsyncServer server(
        adapter.CreateServerCallbacks(),
        serverCfg
    );
    pump->SetWakeupCallback(server.wakeupCallback());
    server.start();

    SockType client = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    Check(client != kInvalid, "TCP client socket failed");

    try {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(server.port());
        Check(
            ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) == 1,
            "inet_pton failed"
        );
        Check(
            ::connect(
                client,
                reinterpret_cast<const sockaddr*>(&addr),
                sizeof(addr)
            ) == 0,
            "TCP connect failed"
        );

        const std::string reqs =
            "GET /native HTTP/1.1\r\nHost: localhost\r\n\r\n"
            "POST /js HTTP/1.1\r\nHost: localhost\r\n"
            "Connection: close\r\nContent-Length: 10\r\n\r\n"
            R"({"id":123})";

        Check(
            ::send(client, reqs.data(), static_cast<int>(reqs.size()), 0) ==
                static_cast<int>(reqs.size()),
            "TCP send failed"
        );

        Check(
            nativeEntered.wait_for(3s) == std::future_status::ready,
            "Native handler did not start"
        );
        std::this_thread::sleep_for(25ms);
        nativeReleasePromise.set_value();

        std::string received;
        char buf[4096];
        while (true) {
            const int n = ::recv(client, buf, sizeof(buf), 0);
            if (n <= 0) {
                break;
            }
            received.append(buf, static_cast<std::size_t>(n));
        }

        const auto nativePos = received.find(R"({"layer":"native"})");
        const auto jsPos =
            received.find(R"({"layer":"v8","echo":{"id":123}})");
        Check(nativePos != std::string::npos, "Native TCP response missing");
        Check(jsPos != std::string::npos, "V8 TCP response missing");
        Check(
            nativePos < jsPos,
            "Live TCP HTTP/1.1 ordering violated across Native and V8"
        );
        Check(jsRuns.load() == 1, "Expected 1 V8 execution");
    } catch (...) {
        closeSock(client);
        server.shutdown();
        pump->Close();
        asyncDispatcher->Shutdown();
        throw;
    }

    closeSock(client);
    server.shutdown();
    pump->Close();
    asyncDispatcher->Shutdown();
}

} // namespace

int main() {
    try {
        V8Runtime v8;
        TestMixedNativeAndV8AsyncHttpPipelining();
        std::cout << "[PASS] MixedNativeAndV8AsyncHttpPipelining\n";
        TestLiveTcpMixedNativeAndV8AsyncHttpServer();
        std::cout << "[PASS] LiveTcpMixedNativeAndV8AsyncHttpServer\n";
        std::cout << "Lamp V8 async HTTP pump integration: 2/2 passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] " << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "[FAIL] unknown exception\n";
        return 1;
    }
}
