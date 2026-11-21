
#include "bridge/async_http_operation_pump.hpp"
#include "bridge/async_http_transport_adapter.hpp"
#include "bridge/execution_dispatcher.hpp"
#include "bridge/operation_registry.hpp"

#include "execution/native_worker_pool.hpp"

#include "http/http_async_server.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
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

using Pump = bridge::AsyncHttpOperationPump;
using Adapter = bridge::AsyncHttpTransportAdapter;
using Coordinator = bridge::AsyncOperationCoordinator;
using CompletionQueue = http::HttpCompletionQueue;
using Connection = http::HttpAsyncConnection;
using Token = http::HttpCompletionToken;
using TaskHandle = execution::AsyncTaskHandle;
using TaskCompletion = execution::AsyncTaskCompletion;
using SubmitResult = execution::AsyncSubmitResult;
using SubmitStatus = execution::AsyncSubmitStatus;
using ScheduleOptions = execution::AsyncScheduleOptions;

void Require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bridge::RequestEnvelope MakeEnvelope(
    std::uint64_t id,
    std::string operationId = "users.get"
) {
    bridge::RequestEnvelope request{};
    request.request_id = id;
    request.operation_id = std::move(operationId);
    request.transport = bridge::TransportKind::Http;
    request.format = bridge::SerializationFormat::Json;
    return request;
}

bridge::ResultEnvelope MakeSuccessResult(
    const bridge::RequestEnvelope& request,
    std::string_view jsonBody
) {
    bridge::ResultEnvelope::Payload payload(
        jsonBody.begin(),
        jsonBody.end()
    );
    return bridge::ResultEnvelope::Success(
        request,
        std::move(payload),
        bridge::SerializationFormat::Json,
        bridge::ExecutionBackend::Native
    );
}

http::HttpResponse OkResponse(std::string_view body) {
    http::HttpResponse response(http::HttpStatus::Ok);
    response.setBody(body);
    return response;
}

class MockScheduler final
    : public execution::IAsyncExecutionScheduler {
public:
    SubmitResult Submit(
        bridge::RequestEnvelope request,
        ScheduleOptions,
        TaskHandle& output
    ) override {
        if (!running_) {
            return SubmitResult::Failure(
                SubmitStatus::SchedulerStopped,
                "Mock scheduler stopped"
            );
        }

        if (nextRejection_.has_value()) {
            const auto status = *nextRejection_;
            nextRejection_.reset();
            return SubmitResult::Failure(status, "Rejected");
        }

        auto completion =
            std::make_shared<TaskCompletion>(
                std::move(request)
            );

        Require(
            completion->MarkQueued(),
            "Mock task must enter queued state"
        );

        output = TaskHandle(completion);
        tasks_.push_back(std::move(completion));

        return SubmitResult::Success();
    }

    bool IsRunning() const noexcept override {
        return running_;
    }

    std::size_t PendingTasks() const noexcept override {
        return tasks_.size();
    }

    std::size_t ActiveTasks() const noexcept override {
        return 0;
    }

    void Shutdown() override {
        running_ = false;
    }

    void RejectNext(SubmitStatus status = SubmitStatus::QueueFull) {
        nextRejection_ = status;
    }

    void Complete(
        std::size_t index,
        bridge::ResultEnvelope result = {}
    ) {
        Require(
            index < tasks_.size(),
            "Invalid mock task index"
        );

        auto& task = tasks_[index];

        Require(
            task->MarkRunning(),
            "Mock task must enter running state"
        );

        Require(
            task->Complete(std::move(result)),
            "Mock task completion must succeed"
        );
    }

    TaskHandle Handle(std::size_t index) const {
        return TaskHandle(tasks_.at(index));
    }

    const bridge::RequestEnvelope& RequestAt(
        std::size_t index
    ) const {
        return tasks_.at(index)->Request();
    }

    std::size_t SubmittedCount() const noexcept {
        return tasks_.size();
    }

private:
    bool running_ = true;
    std::optional<SubmitStatus> nextRejection_;
    std::vector<std::shared_ptr<TaskCompletion>> tasks_;
};

// ============================================================
// Deterministic Pump Tests
// ============================================================

void TestConstructionValidation() {
    auto scheduler = std::make_shared<MockScheduler>();
    auto coordinator = std::make_shared<Coordinator>(scheduler);
    auto queue = std::make_shared<CompletionQueue>();
    auto mapper = [](TaskHandle) {
        return OkResponse("ok");
    };

    bool threwCoord = false;
    try {
        Pump p(nullptr, queue, mapper);
    } catch (const bridge::AsyncHttpOperationPumpError&) {
        threwCoord = true;
    }
    Require(threwCoord, "Null coordinator must throw");

    bool threwQueue = false;
    try {
        Pump p(coordinator, nullptr, mapper);
    } catch (const bridge::AsyncHttpOperationPumpError&) {
        threwQueue = true;
    }
    Require(threwQueue, "Null completion queue must throw");

    bool threwMapper = false;
    try {
        Pump p(coordinator, queue, {});
    } catch (const bridge::AsyncHttpOperationPumpError&) {
        threwMapper = true;
    }
    Require(threwMapper, "Empty mapper must throw");
}

void TestSubmitAndDeliverThroughQueue() {
    auto scheduler = std::make_shared<MockScheduler>();
    auto coordinator = std::make_shared<Coordinator>(scheduler);
    auto queue = std::make_shared<CompletionQueue>();

    Pump pump(
        coordinator,
        queue,
        [](TaskHandle handle) {
            const auto res = handle.Get();
            std::string body(
                res.payload.begin(),
                res.payload.end()
            );
            return OkResponse(body);
        }
    );

    std::vector<Token> tokens;
    auto conn = std::make_shared<Connection>(
        1,
        [&](const Token& token, http::HttpRequest) {
            tokens.push_back(token);
        }
    );

    const auto recv = conn->receive(
        "GET /items HTTP/1.1\r\nHost: test\r\n\r\n"
    );
    Require(recv.dispatched == 1, "Request dispatched");
    Require(tokens.size() == 1, "Token captured");

    const auto submitRes = pump.Submit(
        tokens[0],
        conn,
        MakeEnvelope(10)
    );
    Require(submitRes.Accepted(), "Pump accepted submission");
    Require(pump.Outstanding() == 1, "One task outstanding");
    Require(pump.QueuedCompletions() == 0, "Nothing queued yet");

    scheduler->Complete(
        0,
        MakeSuccessResult(scheduler->RequestAt(0), R"({"ok":true})")
    );

    Require(pump.Poll() == 1, "Poll processed 1 completion");
    Require(pump.Outstanding() == 0, "Outstanding released");
    Require(pump.QueuedCompletions() == 1, "Completion queued");
    Require(!conn->hasPendingOutput(), "Not delivered until Drain");

    Require(pump.Drain() == 1, "Drain delivered 1 completion");
    Require(pump.QueuedCompletions() == 0, "Queue drained");
    Require(conn->hasPendingOutput(), "Connection buffered output");
    Require(
        conn->pendingOutput().find(R"({"ok":true})") !=
            std::string_view::npos,
        "Mapped body delivered"
    );

    const auto stats = pump.Stats();
    Require(stats.submitted == 1, "submitted stat");
    Require(stats.mappedResponses == 1, "mappedResponses stat");
    Require(stats.queuedCompletions == 1, "queuedCompletions stat");
    Require(stats.drained == 1, "drained stat");
}

void TestOutOfOrderCompletionsPreserveHttpOrder() {
    auto scheduler = std::make_shared<MockScheduler>();
    auto coordinator = std::make_shared<Coordinator>(scheduler);
    auto queue = std::make_shared<CompletionQueue>();

    Pump pump(
        coordinator,
        queue,
        [](TaskHandle handle) {
            const auto res = handle.Get();
            std::string body(
                res.payload.begin(),
                res.payload.end()
            );
            return OkResponse(body);
        }
    );

    std::vector<Token> tokens;
    auto conn = std::make_shared<Connection>(
        2,
        [&](const Token& token, http::HttpRequest) {
            tokens.push_back(token);
        }
    );

    (void)conn->receive(
        "GET /1 HTTP/1.1\r\nHost: t\r\n\r\n"
        "GET /2 HTTP/1.1\r\nHost: t\r\n\r\n"
        "GET /3 HTTP/1.1\r\nHost: t\r\n\r\n"
    );
    Require(tokens.size() == 3, "Three tokens");

    for (std::size_t i = 0; i < 3; ++i) {
        Require(
            pump.Submit(tokens[i], conn, MakeEnvelope(i + 1))
                .Accepted(),
            "Submit succeeded"
        );
    }

    scheduler->Complete(
        2,
        MakeSuccessResult(scheduler->RequestAt(2), "THIRD")
    );
    scheduler->Complete(
        0,
        MakeSuccessResult(scheduler->RequestAt(0), "FIRST")
    );
    Require(pump.Advance() == 4, "Two polled + two drained");

    const auto partialOut = conn->pendingOutput();
    Require(
        partialOut.find("FIRST") != std::string_view::npos,
        "First emitted"
    );
    Require(
        partialOut.find("THIRD") == std::string_view::npos,
        "Third held back behind second"
    );

    scheduler->Complete(
        1,
        MakeSuccessResult(scheduler->RequestAt(1), "SECOND")
    );
    Require(pump.Advance() == 2, "One polled + one drained");

    const auto fullOut = conn->pendingOutput();
    const auto p1 = fullOut.find("FIRST");
    const auto p2 = fullOut.find("SECOND");
    const auto p3 = fullOut.find("THIRD");
    Require(
        p1 != std::string_view::npos &&
        p2 != std::string_view::npos &&
        p3 != std::string_view::npos &&
        p1 < p2 && p2 < p3,
        "HTTP/1.1 pipelined response order preserved"
    );
}

void TestInvalidAndStaleTokensRejected() {
    auto scheduler = std::make_shared<MockScheduler>();
    auto coordinator = std::make_shared<Coordinator>(scheduler);
    auto queue = std::make_shared<CompletionQueue>();

    Pump pump(
        coordinator,
        queue,
        [](TaskHandle) { return OkResponse("ok"); }
    );

    std::vector<Token> tokens;
    auto conn = std::make_shared<Connection>(
        10,
        [&](const Token& token, http::HttpRequest) {
            tokens.push_back(token);
        }
    );
    (void)conn->receive("GET / HTTP/1.1\r\nHost: t\r\n\r\n");

    const Token invalid{};
    Require(
        pump.Submit(invalid, conn, MakeEnvelope(1)).status ==
            SubmitStatus::InvalidRequest,
        "Invalid token rejected"
    );

    Token wrongConn = tokens[0];
    wrongConn.connectionId = 999;
    Require(
        pump.Submit(wrongConn, conn, MakeEnvelope(2)).status ==
            SubmitStatus::InvalidRequest,
        "Mismatched connectionId rejected"
    );

    Token wrongGen = tokens[0];
    wrongGen.generation += 1;
    Require(
        pump.Submit(wrongGen, conn, MakeEnvelope(3)).status ==
            SubmitStatus::InvalidRequest,
        "Stale generation rejected"
    );

    // Valid submit succeeds, duplicate active token rejected
    Require(
        pump.Submit(tokens[0], conn, MakeEnvelope(4)).Accepted(),
        "First valid submit accepted"
    );
    Require(
        pump.Submit(tokens[0], conn, MakeEnvelope(5)).status ==
            SubmitStatus::InvalidRequest,
        "Duplicate active token rejected"
    );

    // Closed connection rejected
    auto closedConn = std::make_shared<Connection>(
        11,
        [&](const Token& token, http::HttpRequest) {
            tokens.push_back(token);
        }
    );
    (void)closedConn->receive("GET / HTTP/1.1\r\nHost: t\r\n\r\n");
    const Token closedTok = tokens.back();
    closedConn->abort();
    Require(
        pump.Submit(closedTok, closedConn, MakeEnvelope(6)).status ==
            SubmitStatus::InvalidRequest,
        "Aborted connection rejected"
    );

    Require(
        scheduler->SubmittedCount() == 1,
        "Only valid submission reached scheduler"
    );
}

void TestDestroyedConnectionsHandledSafely() {
    auto scheduler = std::make_shared<MockScheduler>();
    auto coordinator = std::make_shared<Coordinator>(scheduler);
    auto queue = std::make_shared<CompletionQueue>();

    std::atomic<int> mapperCalls{0};
    Pump pump(
        coordinator,
        queue,
        [&](TaskHandle) {
            ++mapperCalls;
            return OkResponse("ok");
        }
    );

    // Case 1: destroyed before Poll()
    {
        Token tok{};
        auto conn = std::make_shared<Connection>(
            20,
            [&](const Token& t, http::HttpRequest) { tok = t; }
        );
        (void)conn->receive("GET / HTTP/1.1\r\nHost: t\r\n\r\n");
        Require(
            pump.Submit(tok, conn, MakeEnvelope(20)).Accepted(),
            "Submit for ephemeral conn 1"
        );
        conn.reset();

        scheduler->Complete(0);
        Require(pump.Poll() == 1, "Poll runs callback");
        Require(
            mapperCalls.load() == 0,
            "Mapper skipped for destroyed connection"
        );
        Require(
            pump.QueuedCompletions() == 0,
            "Dead completion not queued"
        );
        Require(
            pump.Stats().droppedExpiredConnection == 1,
            "Expired connection recorded"
        );
    }

    // Case 2: destroyed between Poll() and Drain()
    {
        Token tok{};
        auto conn = std::make_shared<Connection>(
            21,
            [&](const Token& t, http::HttpRequest) { tok = t; }
        );
        (void)conn->receive("GET / HTTP/1.1\r\nHost: t\r\n\r\n");
        Require(
            pump.Submit(tok, conn, MakeEnvelope(21)).Accepted(),
            "Submit for ephemeral conn 2"
        );

        scheduler->Complete(1);
        Require(pump.Poll() == 1, "Poll queues while conn alive");
        Require(mapperCalls.load() == 1, "Mapper ran");
        Require(pump.QueuedCompletions() == 1, "Queued");

        conn.reset();
        Require(
            pump.Drain() == 1,
            "Drain executes safely when weak_ptr expired"
        );
    }
}

void TestMapperExceptionProduces500() {
    auto scheduler = std::make_shared<MockScheduler>();
    auto coordinator = std::make_shared<Coordinator>(scheduler);
    auto queue = std::make_shared<CompletionQueue>();

    Pump pump(
        coordinator,
        queue,
        [](TaskHandle) -> http::HttpResponse {
            throw std::runtime_error("sensitive_mapper_bug");
        }
    );

    Token tok{};
    auto conn = std::make_shared<Connection>(
        30,
        [&](const Token& t, http::HttpRequest) { tok = t; }
    );
    (void)conn->receive("GET / HTTP/1.1\r\nHost: t\r\n\r\n");
    Require(pump.Submit(tok, conn, MakeEnvelope(30)).Accepted(), "Submit");

    scheduler->Complete(0);
    Require(pump.Advance() == 2, "Advance");

    const auto out = conn->pendingOutput();
    Require(
        out.find("500 Internal Server Error") !=
            std::string_view::npos,
        "Mapper throw yields 500"
    );
    Require(
        out.find("sensitive_mapper_bug") ==
            std::string_view::npos,
        "Exception detail not leaked"
    );
    Require(
        pump.Stats().mapperFailures == 1,
        "mapperFailures tracked"
    );
}

void TestSchedulerRejectionAndSubmitOrComplete() {
    auto scheduler = std::make_shared<MockScheduler>();
    auto coordinator = std::make_shared<Coordinator>(scheduler);
    auto queue = std::make_shared<CompletionQueue>();

    Pump pump(
        coordinator,
        queue,
        [](TaskHandle) { return OkResponse("ok"); }
    );

    std::vector<Token> tokens;
    auto conn = std::make_shared<Connection>(
        40,
        [&](const Token& t, http::HttpRequest) {
            tokens.push_back(t);
        }
    );
    (void)conn->receive(
        "GET /a HTTP/1.1\r\nHost: t\r\n\r\n"
        "GET /b HTTP/1.1\r\nHost: t\r\n\r\n"
        "GET /c HTTP/1.1\r\nHost: t\r\n\r\n"
    );

    // Pure Submit propagates rejection without completing conn
    scheduler->RejectNext(SubmitStatus::QueueFull);
    const auto rejected =
        pump.Submit(tokens[0], conn, MakeEnvelope(41));
    Require(
        rejected.status == SubmitStatus::QueueFull,
        "QueueFull propagated"
    );
    Require(
        !conn->hasPendingOutput(),
        "Pure Submit did not auto-complete"
    );
    (void)conn->complete(
        tokens[0],
        Pump::MapSubmitFailure(rejected)
    );

    // SubmitOrComplete handles DeadlineExceeded -> 504
    scheduler->RejectNext(SubmitStatus::DeadlineExceeded);
    const auto expired = pump.SubmitOrComplete(
        tokens[1],
        conn,
        MakeEnvelope(42)
    );
    Require(
        expired.status == SubmitStatus::DeadlineExceeded,
        "DeadlineExceeded returned"
    );

    // SubmitOrComplete handles QueueFull -> 503
    scheduler->RejectNext(SubmitStatus::QueueFull);
    const auto full = pump.SubmitOrComplete(
        tokens[2],
        conn,
        MakeEnvelope(43)
    );
    Require(full.status == SubmitStatus::QueueFull, "QueueFull returned");

    const auto out = conn->pendingOutput();
    Require(
        out.find("503 Service Unavailable") != std::string_view::npos,
        "503 response emitted"
    );
    Require(
        out.find("504 Gateway Timeout") != std::string_view::npos,
        "504 response emitted"
    );
    Require(conn->inflight() == 0, "All rejected slots resolved");
    Require(
        pump.Stats().submitRejected == 3,
        "submitRejected stat tracked"
    );
}

void TestCompletionQueueSaturationAndClosedQueue() {
    auto scheduler = std::make_shared<MockScheduler>();
    auto coordinator = std::make_shared<Coordinator>(scheduler);
    auto queue = std::make_shared<CompletionQueue>(
        http::HttpCompletionQueueConfig{1}
    );

    Pump pump(
        coordinator,
        queue,
        [](TaskHandle) { return OkResponse("SAT_OK"); }
    );

    std::vector<Token> tokens;
    auto conn = std::make_shared<Connection>(
        50,
        [&](const Token& t, http::HttpRequest) {
            tokens.push_back(t);
        }
    );
    (void)conn->receive(
        "GET /1 HTTP/1.1\r\nHost: t\r\n\r\n"
        "GET /2 HTTP/1.1\r\nHost: t\r\n\r\n"
    );

    // Pre-fill capacity-1 queue externally
    bool externalRan = false;
    Require(
        queue->post(http::HttpCompletion{
            Token{999, 1, 1},
            [&](const Token&) { externalRan = true; }
        }) == http::HttpCompletionPostResult::Accepted,
        "External completion pre-filled queue"
    );

    Require(pump.Submit(tokens[0], conn, MakeEnvelope(51)).Accepted(), "S1");
    scheduler->Complete(0);

    Require(pump.Poll() == 1, "Poll processes completion");
    Require(
        !externalRan,
        "Poll did not prematurely drain external item"
    );
    Require(
        conn->pendingOutput().find("SAT_OK") != std::string_view::npos,
        "Saturated queue fell back to direct owner-thread delivery"
    );
    Require(
        pump.Stats().directSaturationDeliveries == 1,
        "directSaturationDeliveries tracked"
    );

    // Drain external item, then close queue and test Closed drop
    Require(pump.Drain() == 1, "External item drained");
    Require(externalRan, "External callback ran");

    queue->close();

    Require(pump.Submit(tokens[1], conn, MakeEnvelope(52)).Accepted(), "S2");
    scheduler->Complete(1);
    Require(pump.Poll() == 1, "Poll runs callback");
    Require(
        conn->inflight() == 1,
        "Closed queue did not bypass closure to complete slot"
    );
    Require(
        pump.Stats().droppedClosedQueue == 1,
        "droppedClosedQueue tracked"
    );
}

void TestCloseWhileTasksOutstanding() {
    auto scheduler = std::make_shared<MockScheduler>();
    auto coordinator = std::make_shared<Coordinator>(scheduler);
    auto queue = std::make_shared<CompletionQueue>();

    Pump pump(
        coordinator,
        queue,
        [](TaskHandle) { return OkResponse("ok"); }
    );

    Token tok{};
    auto conn = std::make_shared<Connection>(
        60,
        [&](const Token& t, http::HttpRequest) { tok = t; }
    );
    (void)conn->receive("GET / HTTP/1.1\r\nHost: t\r\n\r\n");

    Require(pump.Submit(tok, conn, MakeEnvelope(60)).Accepted(), "Submit");
    Require(
        !scheduler->Handle(0).CancellationRequested(),
        "Not cancelled yet"
    );

    pump.Close();
    Require(pump.Closed(), "Pump closed");
    Require(pump.Outstanding() == 0, "Coordinator cleared");
    Require(
        scheduler->Handle(0).CancellationRequested(),
        "Outstanding task cancelled on Close()"
    );
    Require(pump.Poll() == 0, "Poll returns 0 when closed");
    Require(pump.Drain() == 0, "Drain returns 0 when closed");
    Require(
        pump.Submit(tok, conn, MakeEnvelope(61)).status ==
            SubmitStatus::SchedulerStopped,
        "Submit rejected after Close()"
    );

    // Idempotent second Close()
    pump.Close();
}

void TestCancelConnectionAndTokenOnDisconnect() {
    auto scheduler = std::make_shared<MockScheduler>();
    auto coordinator = std::make_shared<Coordinator>(scheduler);
    auto queue = std::make_shared<CompletionQueue>();

    Pump pump(
        coordinator,
        queue,
        [](TaskHandle) { return OkResponse("ok"); }
    );

    std::vector<Token> tokensA;
    auto connA = std::make_shared<Connection>(
        70,
        [&](const Token& t, http::HttpRequest) {
            tokensA.push_back(t);
        }
    );
    (void)connA->receive(
        "GET /1 HTTP/1.1\r\nHost: t\r\n\r\n"
        "GET /2 HTTP/1.1\r\nHost: t\r\n\r\n"
    );

    std::vector<Token> tokensB;
    auto connB = std::make_shared<Connection>(
        71,
        [&](const Token& t, http::HttpRequest) {
            tokensB.push_back(t);
        }
    );
    (void)connB->receive(
        "GET /1 HTTP/1.1\r\nHost: t\r\n\r\n"
        "GET /2 HTTP/1.1\r\nHost: t\r\n\r\n"
    );

    Require(pump.Submit(tokensA[0], connA, MakeEnvelope(701)).Accepted(), "A0");
    Require(pump.Submit(tokensA[1], connA, MakeEnvelope(702)).Accepted(), "A1");
    Require(pump.Submit(tokensB[0], connB, MakeEnvelope(703)).Accepted(), "B0");
    Require(pump.Submit(tokensB[1], connB, MakeEnvelope(704)).Accepted(), "B1");

    // Cancel single token on B
    Require(pump.CancelToken(tokensB[0]), "CancelToken B0");
    Require(
        scheduler->Handle(2).CancellationRequested() &&
        scheduler->RequestAt(2).IsCancelled(),
        "B0 cancelled"
    );
    Require(
        !scheduler->Handle(3).CancellationRequested(),
        "B1 unaffected"
    );

    // Cancel entire connection A
    const auto cancelledCount = pump.CancelConnection(*connA);
    Require(cancelledCount == 2, "Cancelled both tasks on connA");
    Require(
        scheduler->Handle(0).CancellationRequested() &&
        scheduler->Handle(1).CancellationRequested() &&
        scheduler->RequestAt(0).IsCancelled() &&
        scheduler->RequestAt(1).IsCancelled(),
        "All connA tasks cancelled"
    );
    Require(
        !scheduler->Handle(3).CancellationRequested(),
        "B1 still unaffected"
    );

    connA->abort();
    scheduler->Complete(0);
    scheduler->Complete(1);
    Require(pump.Poll() == 2, "Poll cleans up aborted connA");
    Require(
        pump.Stats().droppedExpiredConnection == 2,
        "Aborted connA completions dropped"
    );
}

// ============================================================
// Real NativeWorkerPool + AsyncHttpTransportAdapter Tests
// ============================================================

class NativeOpHandler final
    : public bridge::INativeOperationHandler {
public:
    using Callback = std::function<
        bridge::ResultEnvelope(const bridge::RequestEnvelope&)
    >;

    explicit NativeOpHandler(Callback callback)
        : callback_(std::move(callback)) {}

    bridge::ResultEnvelope Execute(
        const bridge::RequestEnvelope& request
    ) override {
        return callback_(request);
    }

private:
    Callback callback_;
};

bridge::OperationDescriptor MakeOpDescriptor(
    std::string id
) {
    bridge::OperationDescriptor desc;
    desc.id = std::move(id);
    desc.execution.mode = bridge::ExecutionMode::Native;
    desc.input_format = bridge::SerializationFormat::Json;
    desc.output_format = bridge::SerializationFormat::Json;
    return desc;
}

void RegisterOp(
    const std::shared_ptr<bridge::OperationRegistry>& registry,
    std::string id,
    NativeOpHandler::Callback fn
) {
    bridge::OperationBackends backends;
    backends.native =
        std::make_shared<NativeOpHandler>(std::move(fn));
    Require(
        registry->Register(
            MakeOpDescriptor(std::move(id)),
            std::move(backends)
        ).Ok(),
        "Operation registration must succeed"
    );
}

void TestAdapterWithNativeWorkerPoolPipelinedOutOfOrder() {
    auto registry = std::make_shared<bridge::OperationRegistry>();

    std::promise<void> slowEnteredPromise;
    auto slowEntered = slowEnteredPromise.get_future();
    std::promise<void> slowReleasePromise;
    auto slowRelease = slowReleasePromise.get_future().share();

    RegisterOp(
        registry,
        "ops.slow",
        [&](const bridge::RequestEnvelope& req) {
            slowEnteredPromise.set_value();
            slowRelease.wait();
            return MakeSuccessResult(req, R"({"step":"SLOW"})");
        }
    );

    RegisterOp(
        registry,
        "ops.fast",
        [](const bridge::RequestEnvelope& req) {
            return MakeSuccessResult(req, R"({"step":"FAST"})");
        }
    );

    auto dispatcher =
        std::make_shared<bridge::ExecutionDispatcher>(registry);

    execution::NativeWorkerPoolConfig poolConfig;
    poolConfig.worker_count = 2;
    poolConfig.max_pending_tasks = 16;
    auto pool = std::make_shared<execution::NativeWorkerPool>(
        dispatcher,
        poolConfig
    );

    auto coordinator = std::make_shared<Coordinator>(pool, 16);
    auto queue = std::make_shared<CompletionQueue>();
    auto pump = std::make_shared<Pump>(
        coordinator,
        queue,
        Adapter::DefaultResponseMapper()
    );

    Adapter adapter(pump);
    adapter.Register({
        http::HttpMethod::Get,
        "/slow",
        "ops.slow",
        bridge::SerializationFormat::Json
    });
    adapter.Register({
        http::HttpMethod::Get,
        "/fast",
        "ops.fast",
        bridge::SerializationFormat::Json
    });

    std::shared_ptr<Connection> conn;
    conn = std::make_shared<Connection>(
        100,
        [&adapter, &conn](
            const Token& token,
            http::HttpRequest request
        ) {
            adapter.Dispatch(token, conn, request);
        }
    );

    // Owner-thread receive must NOT block on ops.slow!
    const auto recv = conn->receive(
        "GET /slow HTTP/1.1\r\nHost: localhost\r\n\r\n"
        "GET /fast HTTP/1.1\r\nHost: localhost\r\n\r\n"
    );
    Require(recv.dispatched == 2, "Both requests dispatched");
    Require(!recv.failed(), "No parse failure");

    Require(
        slowEntered.wait_for(2s) == std::future_status::ready,
        "Slow worker entered handler"
    );

    // Wait for fast task to finish while slow task is still blocked
    const auto deadlineFast =
        std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadlineFast) {
        (void)pump->Advance();
        if (pump->Outstanding() == 1) {
            break;
        }
        std::this_thread::sleep_for(2ms);
    }

    Require(
        pump->Outstanding() == 1,
        "Fast task completed while slow task still blocked"
    );
    Require(
        !conn->hasPendingOutput(),
        "Fast response held behind slow response"
    );

    // Release slow task and drain both in HTTP/1.1 order
    slowReleasePromise.set_value();

    const auto deadlineAll =
        std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadlineAll) {
        (void)pump->Advance();
        if (pump->Outstanding() == 0 && conn->inflight() == 0) {
            break;
        }
        std::this_thread::sleep_for(2ms);
    }

    Require(pump->Outstanding() == 0, "All tasks completed");
    Require(conn->inflight() == 0, "All slots flushed");

    const auto wire = conn->pendingOutput();
    const auto slowPos = wire.find(R"({"step":"SLOW"})");
    const auto fastPos = wire.find(R"({"step":"FAST"})");
    Require(
        slowPos != std::string_view::npos &&
        fastPos != std::string_view::npos &&
        slowPos < fastPos,
        "Wire output preserves HTTP/1.1 order"
    );

    pump->Close();
    pool->Shutdown();
}

void TestAdapterRoutesErrorsAndHeadAndClose() {
    auto registry = std::make_shared<bridge::OperationRegistry>();

    RegisterOp(
        registry,
        "users.get",
        [](const bridge::RequestEnvelope& req) {
            return MakeSuccessResult(req, R"({"id":7})");
        }
    );

    RegisterOp(
        registry,
        "users.fail",
        [](const bridge::RequestEnvelope&) -> bridge::ResultEnvelope {
            throw std::runtime_error("secret_db_crash");
        }
    );

    RegisterOp(
        registry,
        "users.missing",
        [](const bridge::RequestEnvelope& req) {
            return bridge::ResultEnvelope::Failure(
                req,
                bridge::OperationError{
                    bridge::OperationErrorCode::NotFound,
                    "internal user row 7 missing",
                    "USER_NOT_FOUND",
                    false
                },
                bridge::ExecutionBackend::Native
            );
        }
    );

    auto dispatcher =
        std::make_shared<bridge::ExecutionDispatcher>(registry);

    execution::NativeWorkerPoolConfig poolConfig;
    poolConfig.worker_count = 2;
    auto pool = std::make_shared<execution::NativeWorkerPool>(
        dispatcher,
        poolConfig
    );

    auto coordinator = std::make_shared<Coordinator>(pool, 32);
    auto queue = std::make_shared<CompletionQueue>();
    auto pump = std::make_shared<Pump>(
        coordinator,
        queue,
        Adapter::DefaultResponseMapper()
    );

    Adapter adapter(pump);
    adapter.Register({
        http::HttpMethod::Get,
        "/users/:id",
        "users.get",
        bridge::SerializationFormat::Json
    });
    adapter.Register({
        http::HttpMethod::Head,
        "/users/:id",
        "users.get",
        bridge::SerializationFormat::Json
    });
    adapter.Register({
        http::HttpMethod::Post,
        "/fail",
        "users.fail",
        bridge::SerializationFormat::Json
    });
    adapter.Register({
        http::HttpMethod::Get,
        "/missing-op",
        "users.missing",
        bridge::SerializationFormat::Json
    });

    std::shared_ptr<Connection> liveConn;
    liveConn = std::make_shared<Connection>(
        202,
        [&adapter, &liveConn](
            const Token& t,
            http::HttpRequest r
        ) {
            adapter.Dispatch(t, liveConn, r);
        }
    );

    // Send 404, 405, HEAD 200, 500 (throwing op), 404 (op error) + close
    const std::string requests =
        "GET /unknown HTTP/1.1\r\nHost: t\r\n\r\n"
        "DELETE /users/7 HTTP/1.1\r\nHost: t\r\n\r\n"
        "HEAD /users/7 HTTP/1.1\r\nHost: t\r\n\r\n"
        "POST /fail HTTP/1.1\r\nHost: t\r\nContent-Length: 2\r\n\r\n{}"
        "GET /missing-op HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n";

    const auto recv = liveConn->receive(requests);
    Require(recv.dispatched == 5, "All 5 requests dispatched");
    Require(!liveConn->accepting(), "Connection: close stopped accepting");

    const auto deadline =
        std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline) {
        (void)pump->Advance();
        if (pump->Outstanding() == 0 && liveConn->inflight() == 0) {
            break;
        }
        std::this_thread::sleep_for(2ms);
    }

    Require(liveConn->inflight() == 0, "All 5 responses flushed");
    const std::string wire(liveConn->pendingOutput());

    Require(
        wire.find("404 Not Found") != std::string::npos,
        "404 present"
    );
    Require(
        wire.find("405 Method Not Allowed") != std::string::npos &&
        wire.find("Allow: GET, HEAD") != std::string::npos,
        "405 with Allow header present"
    );
    Require(
        wire.find("200 OK") != std::string::npos &&
        wire.find("Content-Length: 8") != std::string::npos &&
        wire.find(R"({"id":7})") == std::string::npos,
        "HEAD preserves Content-Length: 8 and omits body"
    );
    Require(
        wire.find("500 Internal Server Error") != std::string::npos &&
        wire.find("secret_db_crash") == std::string::npos,
        "500 sanitizes throwing handler"
    );
    Require(
        wire.find("internal user row 7 missing") == std::string::npos,
        "Error response sanitizes handler error message"
    );
    Require(
        wire.find("Connection: close") != std::string::npos,
        "Connection: close emitted"
    );

    liveConn->consumeOutput(wire.size());
    Require(liveConn->shouldClose(), "Connection closes cleanly");

    pump->Close();
    pool->Shutdown();
}

void TestAdapterQueueFullUnderLoadProduces503() {
    auto registry = std::make_shared<bridge::OperationRegistry>();

    std::promise<void> gateEnteredPromise;
    auto gateEntered = gateEnteredPromise.get_future();
    std::promise<void> gateReleasePromise;
    auto gateRelease = gateReleasePromise.get_future().share();
    std::atomic<int> calls{0};

    RegisterOp(
        registry,
        "ops.gated",
        [&](const bridge::RequestEnvelope& req) {
            if (calls.fetch_add(1) == 0) {
                gateEnteredPromise.set_value();
                gateRelease.wait();
            }
            return MakeSuccessResult(req, R"({"ok":1})");
        }
    );

    auto dispatcher =
        std::make_shared<bridge::ExecutionDispatcher>(registry);

    execution::NativeWorkerPoolConfig poolConfig;
    poolConfig.worker_count = 1;
    poolConfig.max_pending_tasks = 1;
    auto pool = std::make_shared<execution::NativeWorkerPool>(
        dispatcher,
        poolConfig
    );

    auto coordinator = std::make_shared<Coordinator>(pool, 16);
    auto queue = std::make_shared<CompletionQueue>();
    auto pump = std::make_shared<Pump>(
        coordinator,
        queue,
        Adapter::DefaultResponseMapper()
    );

    auto adapter = std::make_shared<Adapter>(pump);
    adapter->Register({
        http::HttpMethod::Get,
        "/work",
        "ops.gated",
        bridge::SerializationFormat::Json
    });

    std::shared_ptr<Connection> conn;
    conn = std::make_shared<Connection>(
        300,
        [&](const Token& t, http::HttpRequest r) {
            adapter->Dispatch(t, conn, r);
        }
    );

    // 1st request enters worker 1
    Require(
        conn->receive("GET /work HTTP/1.1\r\nHost: t\r\n\r\n")
            .dispatched == 1,
        "First dispatched"
    );
    Require(
        gateEntered.wait_for(2s) == std::future_status::ready,
        "First entered worker"
    );

    // 2nd fills pending queue (capacity 1), 3rd hits QueueFull -> 503
    Require(
        conn->receive(
            "GET /work HTTP/1.1\r\nHost: t\r\n\r\n"
            "GET /work HTTP/1.1\r\nHost: t\r\n\r\n"
        ).dispatched == 2,
        "2nd and 3rd dispatched"
    );

    // Verify MakeConnectionDispatcher remains usable after adapter destroyed
    Token detachedToken{};
    http::HttpRequest detachedReq(http::HttpMethod::Get, "/unknown");
    auto detachedConn = std::make_shared<Connection>(
        301,
        [&](const Token& t, http::HttpRequest) {
            detachedToken = t;
        }
    );
    auto detachedDispatcher =
        adapter->MakeConnectionDispatcher(detachedConn);
    adapter.reset();

    (void)detachedConn->receive("GET /unknown HTTP/1.1\r\nHost: t\r\n\r\n");
    detachedDispatcher(detachedToken, std::move(detachedReq));
    Require(
        detachedConn->pendingOutput().find("404 Not Found") !=
            std::string_view::npos,
        "Detached dispatcher works after adapter destroyed"
    );

    gateReleasePromise.set_value();

    const auto deadline =
        std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        (void)pump->Advance();
        if (pump->Outstanding() == 0 && conn->inflight() == 0) {
            break;
        }
        std::this_thread::sleep_for(2ms);
    }

    Require(conn->inflight() == 0, "All 3 slots completed");
    const auto out = conn->pendingOutput();
    const auto firstOk = out.find("200 OK");
    const auto third503 = out.find("503 Service Unavailable");
    Require(
        firstOk != std::string_view::npos &&
        third503 != std::string_view::npos &&
        firstOk < third503,
        "200 responses precede 503 overload response"
    );

    pump->Close();
    pool->Shutdown();
}

void TestLiveTcpAsyncServerWithNativePoolAndDisconnectCancel() {
#ifdef _WIN32
    WSADATA wsaData{};
    Require(::WSAStartup(MAKEWORD(2, 2), &wsaData) == 0, "WSAStartup");
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

    auto connectLoopback = [&](std::uint16_t port) -> SockType {
        SockType s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        Require(s != kInvalid, "client socket");
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        Require(
            ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) == 1,
            "inet_pton"
        );
        if (::connect(
                s,
                reinterpret_cast<const sockaddr*>(&addr),
                sizeof(addr)
            ) != 0) {
            closeSock(s);
            Require(false, "connect failed");
        }
        return s;
    };

    auto registry = std::make_shared<bridge::OperationRegistry>();

    std::promise<void> slowEnteredPromise;
    auto slowEntered = slowEnteredPromise.get_future();
    std::promise<void> slowReleasePromise;
    auto slowRelease = slowReleasePromise.get_future().share();
    std::once_flag slowOnce;

    RegisterOp(
        registry,
        "live.slow",
        [&](const bridge::RequestEnvelope& req) {
            std::call_once(slowOnce, [&]() {
                slowEnteredPromise.set_value();
            });
            slowRelease.wait();
            return MakeSuccessResult(req, R"({"op":"slow"})");
        }
    );

    RegisterOp(
        registry,
        "live.fast",
        [](const bridge::RequestEnvelope& req) {
            return MakeSuccessResult(req, R"({"op":"fast"})");
        }
    );

    std::atomic<bool> cancelEntered{false};
    std::atomic<bool> cancelObserved{false};
    RegisterOp(
        registry,
        "live.wait_cancel",
        [&](const bridge::RequestEnvelope& req) {
            cancelEntered.store(true, std::memory_order_release);
            const auto deadline = std::chrono::steady_clock::now() + 3s;
            while (std::chrono::steady_clock::now() < deadline) {
                if (req.IsCancelled()) {
                    cancelObserved.store(true, std::memory_order_release);
                    return bridge::ResultEnvelope::Cancelled(req);
                }
                std::this_thread::sleep_for(2ms);
            }
            return MakeSuccessResult(req, R"({"op":"late"})");
        }
    );

    auto dispatcher =
        std::make_shared<bridge::ExecutionDispatcher>(registry);
    execution::NativeWorkerPoolConfig poolConfig;
    poolConfig.worker_count = 3;
    auto pool = std::make_shared<execution::NativeWorkerPool>(
        dispatcher,
        poolConfig
    );

    auto coordinator = std::make_shared<Coordinator>(pool, 32);
    auto queue = std::make_shared<CompletionQueue>();
    auto pump = std::make_shared<Pump>(
        coordinator,
        queue,
        Adapter::DefaultResponseMapper()
    );

    Adapter adapter(pump);
    adapter.Register({
        http::HttpMethod::Get,
        "/slow",
        "live.slow",
        bridge::SerializationFormat::Json
    });
    adapter.Register({
        http::HttpMethod::Get,
        "/fast",
        "live.fast",
        bridge::SerializationFormat::Json
    });
    adapter.Register({
        http::HttpMethod::Get,
        "/wait-cancel",
        "live.wait_cancel",
        bridge::SerializationFormat::Json
    });

    http::HttpAsyncServerConfig serverCfg;
    serverCfg.port = 0;
    serverCfg.pollTimeout = 200ms; // long poll to verify instant wakeup!
    http::HttpAsyncServer server(
        adapter.CreateServerCallbacks(),
        serverCfg
    );
    pump->SetWakeupCallback(server.wakeupCallback());
    server.start();

    // 1. Live pipelined out-of-order TCP test with instant wakeup
    {
        SockType client = connectLoopback(server.port());
        const std::string reqs =
            "GET /slow HTTP/1.1\r\nHost: localhost\r\n\r\n"
            "GET /fast HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        Require(
            ::send(client, reqs.data(), static_cast<int>(reqs.size()), 0) ==
                static_cast<int>(reqs.size()),
            "send pipelined"
        );

        Require(
            slowEntered.wait_for(2s) == std::future_status::ready,
            "slow handler entered"
        );
        std::this_thread::sleep_for(15ms);
        slowReleasePromise.set_value();

        std::string received;
        char buf[4096];
        while (true) {
            const int n = ::recv(client, buf, sizeof(buf), 0);
            if (n <= 0) {
                break;
            }
            received.append(buf, static_cast<std::size_t>(n));
        }
        closeSock(client);

        const auto slowPos = received.find(R"({"op":"slow"})");
        const auto fastPos = received.find(R"({"op":"fast"})");
        Require(
            slowPos != std::string::npos &&
            fastPos != std::string::npos &&
            slowPos < fastPos,
            "Live TCP server preserved HTTP/1.1 order across workers"
        );
    }

    // 2. Live TCP disconnect triggers cancellation in NativeWorkerPool
    {
        SockType client = connectLoopback(server.port());
        const std::string req =
            "GET /wait-cancel HTTP/1.1\r\nHost: localhost\r\n\r\n";
        Require(
            ::send(client, req.data(), static_cast<int>(req.size()), 0) ==
                static_cast<int>(req.size()),
            "send wait-cancel"
        );

        const auto waitStart = std::chrono::steady_clock::now() + 2s;
        while (!cancelEntered.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < waitStart) {
            std::this_thread::sleep_for(2ms);
        }
        Require(cancelEntered.load(), "wait_cancel handler entered");

        // Hard-reset client socket so server detects disconnect immediately
        linger lg{1, 0};
        ::setsockopt(
            client,
            SOL_SOCKET,
            SO_LINGER,
            reinterpret_cast<const char*>(&lg),
            sizeof(lg)
        );
        closeSock(client);

        const auto cancelDeadline = std::chrono::steady_clock::now() + 2s;
        while (!cancelObserved.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < cancelDeadline) {
            std::this_thread::sleep_for(5ms);
        }
        Require(
            cancelObserved.load(),
            "TCP disconnect propagated cancellation to worker task"
        );
    }

    server.shutdown();
    pump->Close();
    pool->Shutdown();
}

} // namespace

int main() {
    struct TestCase {
        const char* name;
        std::function<void()> run;
    };

    const std::vector<TestCase> tests = {
        {"ConstructionValidation", TestConstructionValidation},
        {"SubmitAndDeliverThroughQueue", TestSubmitAndDeliverThroughQueue},
        {"OutOfOrderCompletionsPreserveHttpOrder",
         TestOutOfOrderCompletionsPreserveHttpOrder},
        {"InvalidAndStaleTokensRejected", TestInvalidAndStaleTokensRejected},
        {"DestroyedConnectionsHandledSafely",
         TestDestroyedConnectionsHandledSafely},
        {"MapperExceptionProduces500", TestMapperExceptionProduces500},
        {"SchedulerRejectionAndSubmitOrComplete",
         TestSchedulerRejectionAndSubmitOrComplete},
        {"CompletionQueueSaturationAndClosedQueue",
         TestCompletionQueueSaturationAndClosedQueue},
        {"CloseWhileTasksOutstanding", TestCloseWhileTasksOutstanding},
        {"CancelConnectionAndTokenOnDisconnect",
         TestCancelConnectionAndTokenOnDisconnect},
        {"AdapterWithNativeWorkerPoolPipelinedOutOfOrder",
         TestAdapterWithNativeWorkerPoolPipelinedOutOfOrder},
        {"AdapterRoutesErrorsAndHeadAndClose",
         TestAdapterRoutesErrorsAndHeadAndClose},
        {"AdapterQueueFullUnderLoadProduces503",
         TestAdapterQueueFullUnderLoadProduces503},
        {"LiveTcpAsyncServerWithNativePoolAndDisconnectCancel",
         TestLiveTcpAsyncServerWithNativePoolAndDisconnectCancel}
    };

    std::size_t passed = 0;

    for (const auto& test : tests) {
        try {
            test.run();
            ++passed;
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception& error) {
            std::cerr
                << "[FAIL] " << test.name
                << ": " << error.what() << '\n';
            return 1;
        } catch (...) {
            std::cerr
                << "[FAIL] " << test.name
                << ": unknown exception\n";
            return 1;
        }
    }

    std::cout
        << "Lamp async HTTP operation pump: "
        << passed << "/" << tests.size()
        << " tests passed\n";

    return 0;
}
