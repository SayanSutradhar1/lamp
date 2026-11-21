
#include "http/http_async_connection.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace lamp::http;

using Token = HttpCompletionToken;
using CompletionResult = HttpAsyncCompleteResult;

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

HttpResponse response(
    std::string_view body,
    HttpStatus status = HttpStatus::Ok
) {
    HttpResponse result(status);
    result.setBody(body);
    return result;
}

struct Harness {
    std::vector<Token> tokens;
    std::vector<std::string> paths;

    HttpAsyncConnectionConfig config{};

    explicit Harness(
        HttpAsyncConnectionConfig options = {}
    ) : config(options) {}

    auto dispatcher() {
        return [this](const Token& token, HttpRequest request) {
            tokens.push_back(token);
            paths.push_back(request.path());
        };
    }
};

void testConstruction() {
    Harness h;
    HttpAsyncConnection connection(100, h.dispatcher());

    require(connection.connectionId() == 100, "Connection ID");
    require(connection.generation() != 0, "Generation");
    require(connection.accepting(), "Initially accepting");
    require(connection.canReceive(), "Initially receivable");
    require(connection.inflight() == 0, "Initially empty");
    require(!connection.shouldClose(), "Initially active");
}

void testInvalidConstruction() {
    Harness h;

    bool rejectedId = false;
    try {
        HttpAsyncConnection connection(0, h.dispatcher());
    } catch (const HttpAsyncConnectionError&) {
        rejectedId = true;
    }
    require(rejectedId, "Zero connection ID rejected");

    bool rejectedCallback = false;
    try {
        HttpAsyncConnection connection(1, {});
    } catch (const HttpAsyncConnectionError&) {
        rejectedCallback = true;
    }
    require(rejectedCallback, "Empty dispatcher rejected");

    HttpAsyncConnectionConfig config;
    config.maxInflightRequests = 0;

    bool rejectedCapacity = false;
    try {
        HttpAsyncConnection connection(1, h.dispatcher(), config);
    } catch (const HttpAsyncConnectionError&) {
        rejectedCapacity = true;
    }
    require(rejectedCapacity, "Zero capacity rejected");
}

void testIncrementalParsing() {
    Harness h;
    HttpAsyncConnection connection(1, h.dispatcher());

    const auto first = connection.receive(
        "GET /hello HTTP/1.1\r\nHost: exam"
    );

    require(first.dispatched == 0, "No partial dispatch");
    require(!first.failed(), "Partial input accepted");

    const auto second = connection.receive(
        "ple.test\r\n\r\n"
    );

    require(second.dispatched == 1, "Complete dispatch");
    require(h.paths.size() == 1, "One request");
    require(h.paths[0] == "/hello", "Correct path");
    require(h.tokens[0].valid(), "Valid token");
}

void testDeferredResponse() {
    Harness h;
    HttpAsyncConnection connection(1, h.dispatcher());

    const auto received = connection.receive(
        "GET /deferred HTTP/1.1\r\nHost: test\r\n\r\n"
    );

    require(received.dispatched == 1, "Request dispatched");
    require(!connection.hasPendingOutput(), "No early response");

    require(
        connection.complete(h.tokens[0], response("done"))
            == CompletionResult::Accepted,
        "Completion accepted"
    );

    require(connection.hasPendingOutput(), "Response buffered");
    require(
        connection.pendingOutput().find("done")
            != std::string_view::npos,
        "Response body present"
    );
}

void testPipelinedRequests() {
    Harness h;
    HttpAsyncConnection connection(1, h.dispatcher());

    const std::string input =
        "GET /one HTTP/1.1\r\nHost: test\r\n\r\n"
        "GET /two HTTP/1.1\r\nHost: test\r\n\r\n";

    const auto received = connection.receive(input);

    require(received.consumed == input.size(), "All bytes consumed");
    require(received.dispatched == 2, "Both dispatched");
    require(h.paths[0] == "/one", "First path");
    require(h.paths[1] == "/two", "Second path");
}

void testOutOfOrderCompletion() {
    Harness h;
    HttpAsyncConnection connection(1, h.dispatcher());

    (void)connection.receive(
        "GET /one HTTP/1.1\r\nHost: test\r\n\r\n"
        "GET /two HTTP/1.1\r\nHost: test\r\n\r\n"
    );

    require(
        connection.complete(h.tokens[1], response("SECOND"))
            == CompletionResult::Accepted,
        "Second completed"
    );

    require(
        !connection.hasPendingOutput(),
        "Later response must wait"
    );

    require(
        connection.complete(h.tokens[0], response("FIRST"))
            == CompletionResult::Accepted,
        "First completed"
    );

    const auto output = connection.pendingOutput();
    const auto first = output.find("FIRST");
    const auto second = output.find("SECOND");

    require(first != std::string_view::npos, "First present");
    require(second != std::string_view::npos, "Second present");
    require(first < second, "Wire response order preserved");
}

void testPartialOutputConsumption() {
    Harness h;
    HttpAsyncConnection connection(1, h.dispatcher());

    (void)connection.receive(
        "GET / HTTP/1.1\r\nHost: test\r\n\r\n"
    );

    (void)connection.complete(h.tokens[0], response("PAYLOAD"));

    const std::string original(connection.pendingOutput());
    require(original.size() > 10, "Response length");

    connection.consumeOutput(10);

    require(
        connection.pendingOutput() == std::string_view(original).substr(10),
        "Partial consumption"
    );

    connection.consumeOutput(original.size() - 10);

    require(!connection.hasPendingOutput(), "Fully consumed");
}

void testInvalidOutputConsumption() {
    Harness h;
    HttpAsyncConnection connection(1, h.dispatcher());

    bool rejected = false;
    try {
        connection.consumeOutput(1);
    } catch (const HttpAsyncConnectionError&) {
        rejected = true;
    }

    require(rejected, "Excessive consumption rejected");
}

void testDuplicateCompletion() {
    Harness h;
    HttpAsyncConnection connection(1, h.dispatcher());

    (void)connection.receive(
        "GET /one HTTP/1.1\r\nHost: test\r\n\r\n"
        "GET /two HTTP/1.1\r\nHost: test\r\n\r\n"
    );

    require(
        connection.complete(h.tokens[1], response("two"))
            == CompletionResult::Accepted,
        "First completion accepted"
    );

    require(
        connection.complete(h.tokens[1], response("again"))
            == CompletionResult::AlreadyCompleted,
        "Duplicate rejected while buffered"
    );
}

void testStaleConnection() {
    Harness firstHarness;
    Harness secondHarness;

    HttpAsyncConnection first(10, firstHarness.dispatcher());
    HttpAsyncConnection second(10, secondHarness.dispatcher());

    (void)first.receive(
        "GET / HTTP/1.1\r\nHost: test\r\n\r\n"
    );

    (void)second.receive(
        "GET / HTTP/1.1\r\nHost: test\r\n\r\n"
    );

    require(
        first.generation() != second.generation(),
        "Distinct generations"
    );

    require(
        second.complete(firstHarness.tokens[0], response("stale"))
            == CompletionResult::StaleConnection,
        "Old token rejected"
    );
}

void testUnknownRequest() {
    Harness h;
    HttpAsyncConnection connection(1, h.dispatcher());

    (void)connection.receive(
        "GET / HTTP/1.1\r\nHost: test\r\n\r\n"
    );

    Token unknown = h.tokens[0];
    unknown.requestId += 100;

    require(
        connection.complete(unknown, response("unknown"))
            == CompletionResult::UnknownRequest,
        "Unknown request rejected"
    );
}

void testHeadResponse() {
    Harness h;
    HttpAsyncConnection connection(1, h.dispatcher());

    (void)connection.receive(
        "HEAD / HTTP/1.1\r\nHost: test\r\n\r\n"
    );

    (void)connection.complete(
        h.tokens[0],
        response("SECRET_BODY")
    );

    const auto output = connection.pendingOutput();

    require(
        output.find("Content-Length: 11\r\n")
            != std::string_view::npos,
        "HEAD preserves representation length"
    );

    require(
        output.find("SECRET_BODY") == std::string_view::npos,
        "HEAD omits body"
    );

    require(output.ends_with("\r\n\r\n"), "Headers terminate");
}

void testHttp10Close() {
    Harness h;
    HttpAsyncConnection connection(1, h.dispatcher());

    const auto received = connection.receive(
        "GET / HTTP/1.0\r\n\r\n"
    );

    require(received.dispatched == 1, "HTTP/1.0 dispatched");
    require(!connection.accepting(), "No implicit keepalive");

    (void)connection.complete(h.tokens[0], response("bye"));

    require(connection.hasPendingOutput(), "Output pending");
    require(!connection.shouldClose(), "Wait for output drain");

    connection.consumeOutput(connection.pendingOutput().size());

    require(connection.shouldClose(), "Close after drain");
}

void testExplicitClose() {
    Harness h;
    HttpAsyncConnection connection(1, h.dispatcher());

    (void)connection.receive(
        "GET / HTTP/1.1\r\n"
        "Host: test\r\n"
        "Connection: close\r\n\r\n"
    );

    require(!connection.accepting(), "Close requested");

    (void)connection.complete(h.tokens[0], response("bye"));
    connection.consumeOutput(connection.pendingOutput().size());

    require(connection.shouldClose(), "Graceful close");
}

void testInflightBackpressure() {
    Harness h;
    HttpAsyncConnectionConfig config;
    config.maxInflightRequests = 1;

    HttpAsyncConnection connection(1, h.dispatcher(), config);

    const std::string first =
        "GET /one HTTP/1.1\r\nHost: test\r\n\r\n";

    const std::string second =
        "GET /two HTTP/1.1\r\nHost: test\r\n\r\n";

    const auto received = connection.receive(first + second);

    require(received.dispatched == 1, "Only first dispatched");
    require(received.consumed == first.size(), "Stopped at boundary");
    require(received.backpressured, "Backpressure reported");

    (void)connection.complete(h.tokens[0], response("one"));

    const auto resumed = connection.receive(second);

    require(resumed.dispatched == 1, "Dispatch resumed");
    require(h.paths[1] == "/two", "Second request preserved");
}

void testDispatchFailure() {
    HttpAsyncConnection connection(
        1,
        [](const Token&, HttpRequest) {
            throw std::runtime_error("scheduler unavailable");
        }
    );

    const auto received = connection.receive(
        "GET / HTTP/1.1\r\nHost: test\r\n\r\n"
    );

    require(received.dispatched == 1, "Request parsed");
    require(
        connection.pendingOutput().find("500")
            != std::string_view::npos,
        "Failure response emitted"
    );
}

void testAbort() {
    Harness h;
    HttpAsyncConnection connection(1, h.dispatcher());

    (void)connection.receive(
        "GET / HTTP/1.1\r\nHost: test\r\n\r\n"
    );

    const Token token = h.tokens[0];

    connection.abort();

    require(connection.closed(), "Connection closed");
    require(connection.shouldClose(), "Socket may close");
    require(connection.inflight() == 0, "Slots discarded");
    require(!connection.hasPendingOutput(), "Output discarded");

    require(
        connection.complete(token, response("late"))
            == CompletionResult::Closed,
        "Late completion rejected"
    );
}

void testStopAccepting() {
    Harness h;
    HttpAsyncConnection connection(1, h.dispatcher());

    (void)connection.receive(
        "GET / HTTP/1.1\r\nHost: test\r\n\r\n"
    );

    connection.stopAccepting();

    require(!connection.accepting(), "Input stopped");
    require(!connection.shouldClose(), "Outstanding request retained");

    (void)connection.complete(h.tokens[0], response("done"));

    connection.consumeOutput(connection.pendingOutput().size());

    require(connection.shouldClose(), "Graceful drain complete");
}

void testOversizedResponseEmits500AndCloses() {
    Harness h;
    HttpAsyncConnectionConfig config;
    config.maxPendingWriteBytes = 256;

    HttpAsyncConnection connection(1, h.dispatcher(), config);

    (void)connection.receive(
        "GET /huge HTTP/1.1\r\nHost: test\r\n\r\n"
    );

    const std::string hugeBody(512, 'X');
    require(
        connection.complete(h.tokens[0], response(hugeBody))
            == CompletionResult::Accepted,
        "Completion accepted"
    );

    require(!connection.accepting(), "Connection stops accepting");
    require(connection.inflight() == 0, "Slot cleared");
    require(connection.hasPendingOutput(), "Fallback 500 emitted");

    const auto out = connection.pendingOutput();
    require(
        out.find("500 Internal Server Error") != std::string_view::npos,
        "Fallback 500 status"
    );
    require(
        out.find("Connection: close") != std::string_view::npos,
        "Fallback Connection: close"
    );

    connection.consumeOutput(out.size());
    require(connection.shouldClose(), "Closes cleanly after drain");
}

void testOversizedResponseTinyBufferAborts() {
    Harness h;
    HttpAsyncConnectionConfig config;
    config.maxPendingWriteBytes = 16;

    HttpAsyncConnection connection(1, h.dispatcher(), config);

    (void)connection.receive(
        "GET /huge HTTP/1.1\r\nHost: test\r\n\r\n"
    );

    require(
        connection.complete(h.tokens[0], response("too big for 16 bytes"))
            == CompletionResult::Accepted,
        "Completion processed"
    );

    require(connection.closed(), "Aborted when even 500 cannot fit");
    require(connection.shouldClose(), "No deadlock on tiny write limit");
}

void testWriteBufferBackpressure() {
    Harness h;
    const auto fullResponse = response(std::string(64, 'A'));
    const auto exactSize = fullResponse.serialize().size();

    HttpAsyncConnectionConfig config;
    config.maxInflightRequests = 8;
    config.maxPendingWriteBytes = exactSize;

    HttpAsyncConnection connection(1, h.dispatcher(), config);

    (void)connection.receive(
        "GET /first HTTP/1.1\r\nHost: test\r\n\r\n"
    );

    (void)connection.complete(h.tokens[0], fullResponse);

    require(
        connection.pendingOutputBytes() == config.maxPendingWriteBytes,
        "Output buffer saturated"
    );

    require(!connection.canReceive(), "Backpressured when write buffer full");

    const std::string nextReq =
        "GET /blocked HTTP/1.1\r\nHost: test\r\n\r\n";
    const auto blocked = connection.receive(nextReq);
    require(blocked.backpressured, "Receive reports backpressure");
    require(blocked.dispatched == 0, "No dispatch while write buffer full");
    require(blocked.consumed == 0, "No bytes consumed while backpressured");

    connection.consumeOutput(connection.pendingOutput().size());
    require(connection.canReceive(), "Receivable after output drained");

    const auto resumed = connection.receive(nextReq);
    require(resumed.dispatched == 1, "Dispatch resumes after drain");
}

void testPipelinedParseErrorPreservesEarlierResponse() {
    Harness h;
    HttpAsyncConnection connection(1, h.dispatcher());

    const std::string pipelined =
        "GET /valid HTTP/1.1\r\nHost: test\r\n\r\n"
        "BAD REQUEST LINE\r\n\r\n";

    const auto received = connection.receive(pipelined);

    require(received.dispatched == 1, "Valid request dispatched");
    require(received.failed(), "Parse error reported");
    require(received.parseError != HttpParseError::None, "Parse error code set");
    require(!connection.accepting(), "Stopped accepting after parse error");
    require(!connection.closed(), "Not aborted; earlier request preserved");

    // Complete the earlier valid request
    require(
        connection.complete(h.tokens[0], response("OK_BODY"))
            == CompletionResult::Accepted,
        "Earlier request still completable"
    );

    const auto out = connection.pendingOutput();
    const auto okPos = out.find("OK_BODY");
    const auto badPos = out.find("400 Bad Request");

    require(okPos != std::string_view::npos, "Earlier response present");
    require(badPos != std::string_view::npos, "400 error response present");
    require(okPos < badPos, "Valid response precedes 400 error");

    connection.consumeOutput(out.size());
    require(connection.shouldClose(), "Closes cleanly after draining both");
}

void testConnectionHeadersEmitted() {
    Harness h;
    HttpAsyncConnection http10(1, h.dispatcher());
    (void)http10.receive("GET / HTTP/1.0\r\n\r\n");
    (void)http10.complete(h.tokens[0], response("v10"));
    require(
        http10.pendingOutput().find("Connection: close") != std::string_view::npos,
        "HTTP/1.0 close emits Connection: close"
    );

    Harness h2;
    HttpAsyncConnection http10KeepAlive(2, h2.dispatcher());
    (void)http10KeepAlive.receive(
        "GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n"
    );
    (void)http10KeepAlive.complete(h2.tokens[0], response("ka"));
    require(
        http10KeepAlive.pendingOutput().find("Connection: keep-alive")
            != std::string_view::npos,
        "HTTP/1.0 keep-alive emits Connection: keep-alive"
    );
}

void testRetainedResponseMemoryLimit() {
    Harness h;
    HttpAsyncConnectionConfig config;
    config.maxInflightRequests = 8;
    config.maxPendingWriteBytes = 4096;
    config.maxRetainedResponseBytes = 256;

    HttpAsyncConnection connection(1, h.dispatcher(), config);

    (void)connection.receive(
        "GET /slow HTTP/1.1\r\nHost: t\r\n\r\n"
        "GET /r2 HTTP/1.1\r\nHost: t\r\n\r\n"
        "GET /r3 HTTP/1.1\r\nHost: t\r\n\r\n"
    );

    // Complete r2 out of order within retained budget (120 bytes + 64 overhead < 256)
    require(
        connection.complete(h.tokens[1], response(std::string(120, 'A')))
            == CompletionResult::Accepted,
        "r2 accepted within retained budget"
    );
    require(connection.retainedResponseBytes() > 0, "Retained bytes tracked");

    // Complete r3 out of order exceeding retained budget (120 + 64 = 184; 184 + 184 > 256)
    require(
        connection.complete(h.tokens[2], response(std::string(120, 'B')))
            == CompletionResult::Accepted,
        "r3 handled without OOM"
    );
    require(!connection.accepting(), "Connection stops accepting after over-budget retained response");

    // Now complete head request r1
    require(
        connection.complete(h.tokens[0], response("HEAD_OK"))
            == CompletionResult::Accepted,
        "r1 completed"
    );

    const auto out = connection.pendingOutput();
    require(out.find("HEAD_OK") != std::string_view::npos, "r1 flushed");
    require(out.find(std::string(120, 'A')) != std::string_view::npos, "r2 flushed");
    require(out.find("503 Service Unavailable") != std::string_view::npos, "r3 bounded to 503");

    connection.consumeOutput(out.size());
    require(connection.retainedResponseBytes() == 0, "Retained bytes zero after flush");
    require(connection.shouldClose(), "Connection closes cleanly");
}

using Test = std::pair<
    const char*,
    std::function<void()>
>;

} // namespace

int main() {
    const std::vector<Test> tests = {
        {"Construction", testConstruction},
        {"InvalidConstruction", testInvalidConstruction},
        {"IncrementalParsing", testIncrementalParsing},
        {"DeferredResponse", testDeferredResponse},
        {"PipelinedRequests", testPipelinedRequests},
        {"OutOfOrderCompletion", testOutOfOrderCompletion},
        {"PartialOutputConsumption", testPartialOutputConsumption},
        {"InvalidOutputConsumption", testInvalidOutputConsumption},
        {"DuplicateCompletion", testDuplicateCompletion},
        {"StaleConnection", testStaleConnection},
        {"UnknownRequest", testUnknownRequest},
        {"HeadResponse", testHeadResponse},
        {"Http10Close", testHttp10Close},
        {"ExplicitClose", testExplicitClose},
        {"InflightBackpressure", testInflightBackpressure},
        {"DispatchFailure", testDispatchFailure},
        {"Abort", testAbort},
        {"StopAccepting", testStopAccepting},
        {"OversizedResponseEmits500AndCloses", testOversizedResponseEmits500AndCloses},
        {"OversizedResponseTinyBufferAborts", testOversizedResponseTinyBufferAborts},
        {"WriteBufferBackpressure", testWriteBufferBackpressure},
        {"PipelinedParseErrorPreservesEarlierResponse",
         testPipelinedParseErrorPreservesEarlierResponse},
        {"ConnectionHeadersEmitted", testConnectionHeadersEmitted},
        {"RetainedResponseMemoryLimit", testRetainedResponseMemoryLimit}
    };

    std::size_t passed = 0;

    for (const auto& [name, test] : tests) {
        try {
            test();
            ++passed;
            std::cout << "[PASS] " << name << '\n';
        } catch (const std::exception& error) {
            std::cerr
                << "[FAIL] " << name
                << ": " << error.what()
                << '\n';
        } catch (...) {
            std::cerr
                << "[FAIL] " << name
                << ": unknown exception\n";
        }
    }

    std::cout
        << "Lamp async HTTP connection: "
        << passed << "/" << tests.size()
        << " tests passed\n";

    return passed == tests.size() ? 0 : 1;
}
