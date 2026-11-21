
#include "http/http_connection.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using lamp::http::HttpConnection;
using lamp::http::HttpConnectionConfig;
using lamp::http::HttpConnectionState;
using lamp::http::HttpMethod;
using lamp::http::HttpResponse;
using lamp::http::HttpRouter;
using lamp::http::HttpStatus;

// ============================================================
// Minimal Test Framework
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

template <typename Left, typename Right>
void requireEqual(
    const Left& actual,
    const Right& expected,
    std::string_view message
) {
    if (!(actual == expected)) {
        throw TestFailure(std::string(message));
    }
}

using TestFunction = void (*)();

struct TestCase final {
    const char* name;
    TestFunction function;
};

std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

struct Registration final {
    Registration(
        const char* name,
        TestFunction function
    ) {
        registry().push_back({name, function});
    }
};

#define LAMP_TEST(name)                         \
    void name();                                \
    const Registration register_##name(         \
        #name, &name                            \
    );                                          \
    void name()

// ============================================================
// HTTP Helpers
// ============================================================

[[nodiscard]]
std::string makeRequest(
    std::string_view method = "GET",
    std::string_view target = "/",
    std::string_view connection = "keep-alive",
    std::string_view extraHeaders = {},
    std::string_view body = {}
) {
    std::string result;

    result += method;
    result += " ";
    result += target;
    result += " HTTP/1.1\r\n";

    result += "Host: localhost\r\n";

    result += "Connection: ";
    result += connection;
    result += "\r\n";

    result += extraHeaders;

    result += "\r\n";
    result += body;

    return result;
}

[[nodiscard]]
HttpResponse textResponse(
    std::string_view body,
    HttpStatus status = HttpStatus::Ok
) {
    HttpResponse response(status);

    response.setHeader(
        "Content-Type",
        "text/plain"
    );

    response.setBody(body);

    return response;
}

[[nodiscard]]
std::shared_ptr<HttpRouter> makeRouter() {
    auto router = std::make_shared<HttpRouter>();

    router->get(
        "/hello",
        [](const auto&, const auto&) {
            return textResponse("Hello Lamp");
        }
    );

    router->get(
        "/ping",
        [](const auto&, const auto&) {
            return textResponse("pong");
        }
    );

    router->post(
        "/echo",
        [](const auto& request, const auto&) {
            std::string body;
            body.reserve(request.bodySize());

            for (const auto byte : request.body()) {
                body.push_back(
                    static_cast<char>(
                        std::to_integer<unsigned char>(
                            byte
                        )
                    )
                );
            }

            return textResponse(body);
        }
    );

    return router;
}

[[nodiscard]]
std::string drainOutput(
    HttpConnection& connection,
    std::size_t chunkSize = 4096
) {
    std::string result;

    while (connection.hasPendingOutput()) {
        const auto chunk =
            connection.pendingOutput(chunkSize);

        require(
            !chunk.empty(),
            "Pending output returned an empty segment"
        );

        result += chunk;

        connection.consumeOutput(chunk.size());
    }

    return result;
}

[[nodiscard]]
int responseStatus(std::string_view response) {
    const auto firstSpace = response.find(' ');

    require(
        firstSpace != std::string_view::npos,
        "Missing HTTP status separator"
    );

    const auto secondSpace =
        response.find(' ', firstSpace + 1);

    require(
        secondSpace != std::string_view::npos,
        "Missing HTTP reason phrase"
    );

    return std::stoi(
        std::string(
            response.substr(
                firstSpace + 1,
                secondSpace - firstSpace - 1
            )
        )
    );
}

[[nodiscard]]
std::string_view responseBody(
    std::string_view response
) {
    const auto position =
        response.find("\r\n\r\n");

    require(
        position != std::string_view::npos,
        "Missing HTTP header terminator"
    );

    return response.substr(position + 4);
}

// ============================================================
// Test 1: Initial State
// ============================================================

LAMP_TEST(connection_starts_open) {
    HttpConnection connection(makeRouter());

    require(
        connection.isOpen(),
        "New connection should be open"
    );

    requireEqual(
        connection.requestsHandled(),
        std::size_t{0},
        "Initial request count must be zero"
    );

    require(
        !connection.hasPendingOutput(),
        "New connection should have no pending output"
    );
}

// ============================================================
// Test 2: Null Router
// ============================================================

LAMP_TEST(connection_requires_router) {
    bool threw = false;

    try {
        HttpConnection connection(nullptr);
        (void)connection;
    } catch (const lamp::http::HttpConnectionError&) {
        threw = true;
    }

    require(
        threw,
        "Connection must reject a null router"
    );
}

// ============================================================
// Test 3: GET Routing
// ============================================================

LAMP_TEST(get_request_dispatches_route) {
    HttpConnection connection(makeRouter());

    const auto input =
        makeRequest("GET", "/hello");

    const auto result = connection.receive(input);

    requireEqual(
        result.consumed,
        input.size(),
        "GET request was not fully consumed"
    );

    requireEqual(
        result.requestsCompleted,
        std::size_t{1},
        "Expected one completed request"
    );

    const auto output = drainOutput(connection);

    requireEqual(
        responseStatus(output),
        200,
        "GET should return HTTP 200"
    );

    requireEqual(
        responseBody(output),
        std::string_view("Hello Lamp"),
        "GET response body mismatch"
    );
}

// ============================================================
// Test 4: POST Body
// ============================================================

LAMP_TEST(post_request_preserves_body) {
    HttpConnection connection(makeRouter());

    const auto input = makeRequest(
        "POST",
        "/echo",
        "close",
        "Content-Length: 4\r\n",
        "lamp"
    );

    const auto result = connection.receive(input);

    requireEqual(
        result.requestsCompleted,
        std::size_t{1},
        "POST request did not complete"
    );

    const auto output = drainOutput(connection);

    requireEqual(
        responseBody(output),
        std::string_view("lamp"),
        "POST body mismatch"
    );
}

// ============================================================
// Test 5: Missing Route
// ============================================================

LAMP_TEST(missing_route_returns_404) {
    HttpConnection connection(makeRouter());

    const auto result = connection.receive(
        makeRequest("GET", "/missing")
    );

    requireEqual(
        result.requestsCompleted,
        std::size_t{1},
        "Missing route request did not complete"
    );

    const auto output = drainOutput(connection);

    requireEqual(
        responseStatus(output),
        404,
        "Missing route should return HTTP 404"
    );
}

// ============================================================
// Test 6: Wrong Method
// ============================================================

LAMP_TEST(wrong_method_returns_405) {
    HttpConnection connection(makeRouter());

    const auto result = connection.receive(
        makeRequest("POST", "/hello")
    );

    requireEqual(
        result.requestsCompleted,
        std::size_t{1},
        "Wrong-method request did not complete"
    );

    const auto output = drainOutput(connection);

    requireEqual(
        responseStatus(output),
        405,
        "Wrong method should return HTTP 405"
    );
}

// ============================================================
// Test 7: Fragmented Request
// ============================================================

LAMP_TEST(fragmented_request_completes) {
    HttpConnection connection(makeRouter());

    const auto first = connection.receive(
        "GET /hello HTTP/1.1\r\n"
        "Host: local"
    );

    requireEqual(
        first.requestsCompleted,
        std::size_t{0},
        "Partial headers must not complete request"
    );

    const auto second = connection.receive(
        "host\r\n"
        "Connection: keep-alive\r\n"
        "\r\n"
    );

    requireEqual(
        second.requestsCompleted,
        std::size_t{1},
        "Fragmented request did not complete"
    );

    const auto output = drainOutput(connection);

    requireEqual(
        responseBody(output),
        std::string_view("Hello Lamp"),
        "Fragmented response mismatch"
    );
}

// ============================================================
// Test 8: Keep-Alive
// ============================================================

LAMP_TEST(keep_alive_accepts_multiple_requests) {
    HttpConnection connection(makeRouter());

    const auto first = connection.receive(
        makeRequest("GET", "/hello")
    );

    requireEqual(
        first.requestsCompleted,
        std::size_t{1},
        "First keep-alive request failed"
    );

    drainOutput(connection);

    require(
        connection.isOpen(),
        "Keep-alive connection should remain open"
    );

    const auto second = connection.receive(
        makeRequest("GET", "/ping")
    );

    requireEqual(
        second.requestsCompleted,
        std::size_t{1},
        "Second keep-alive request failed"
    );

    const auto output = drainOutput(connection);

    requireEqual(
        responseBody(output),
        std::string_view("pong"),
        "Second keep-alive response mismatch"
    );
}

// ============================================================
// Test 9: Connection Close
// ============================================================

LAMP_TEST(connection_close_waits_for_output) {
    HttpConnection connection(makeRouter());

    const auto result = connection.receive(
        makeRequest(
            "GET",
            "/hello",
            "close"
        )
    );

    requireEqual(
        result.requestsCompleted,
        std::size_t{1},
        "Close request did not complete"
    );

    requireEqual(
        connection.state(),
        HttpConnectionState::Closing,
        "Connection should wait for pending output"
    );

    require(
        !connection.shouldClose(),
        "Connection should not close before output drains"
    );

    drainOutput(connection);

    require(
        connection.shouldClose(),
        "Connection should close after output drains"
    );

    requireEqual(
        connection.state(),
        HttpConnectionState::Closed,
        "Expected Closed state"
    );
}

// ============================================================
// Test 10: HTTP Pipelining
// ============================================================

LAMP_TEST(pipelined_requests_preserve_order) {
    HttpConnection connection(makeRouter());

    std::string input;

    input += makeRequest(
        "GET",
        "/hello"
    );

    input += makeRequest(
        "GET",
        "/ping",
        "close"
    );

    const auto result = connection.receive(input);

    requireEqual(
        result.requestsCompleted,
        std::size_t{2},
        "Expected two pipelined requests"
    );

    const auto output = drainOutput(connection);

    const auto firstPosition =
        output.find("Hello Lamp");

    const auto secondPosition =
        output.find("pong");

    require(
        firstPosition != std::string::npos,
        "Missing first response"
    );

    require(
        secondPosition != std::string::npos,
        "Missing second response"
    );

    require(
        firstPosition < secondPosition,
        "Pipelined response order was incorrect"
    );
}

// ============================================================
// Test 11: Partial Output Writes
// ============================================================

LAMP_TEST(partial_output_consumption_works) {
    HttpConnection connection(makeRouter());

    connection.receive(
        makeRequest("GET", "/hello")
    );

    const auto initialSize =
        connection.pendingWriteBytes();

    require(
        initialSize > 10,
        "Expected buffered response"
    );

    const auto firstChunk =
        connection.pendingOutput(5);

    requireEqual(
        firstChunk.size(),
        std::size_t{5},
        "Expected five output bytes"
    );

    connection.consumeOutput(5);

    requireEqual(
        connection.pendingWriteBytes(),
        initialSize - 5,
        "Pending output size was not updated"
    );

    const auto remaining = drainOutput(connection);

    require(
        remaining.find("Hello Lamp") !=
            std::string::npos,
        "Remaining output lost response body"
    );
}

// ============================================================
// Test 12: Zero-Length Output Request
// ============================================================

LAMP_TEST(zero_length_output_request_is_empty) {
    HttpConnection connection(makeRouter());

    connection.receive(
        makeRequest("GET", "/hello")
    );

    require(
        connection.pendingOutput(0).empty(),
        "Zero-length output request must be empty"
    );
}

// ============================================================
// Test 13: Excessive Output Consumption
// ============================================================

LAMP_TEST(rejects_excessive_output_consumption) {
    HttpConnection connection(makeRouter());

    connection.receive(
        makeRequest("GET", "/hello")
    );

    bool threw = false;

    try {
        connection.consumeOutput(
            connection.pendingWriteBytes() + 1
        );
    } catch (const lamp::http::HttpConnectionError&) {
        threw = true;
    }

    require(
        threw,
        "Consuming more than pending bytes must throw"
    );
}

// ============================================================
// Test 14: Malformed Request
// ============================================================

LAMP_TEST(malformed_request_generates_400) {
    HttpConnection connection(makeRouter());

    const auto result = connection.receive(
        "GET / HTTP/1.1\n"
    );

    require(
        result.failed(),
        "Malformed request should fail"
    );

    const auto output = drainOutput(connection);

    requireEqual(
        responseStatus(output),
        400,
        "Malformed request should return HTTP 400"
    );
}

// ============================================================
// Test 15: Conflicting Content-Length
// ============================================================

LAMP_TEST(conflicting_content_length_generates_400) {
    HttpConnection connection(makeRouter());

    const auto result = connection.receive(
        "POST /echo HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: 3\r\n"
        "Content-Length: 4\r\n"
        "\r\n"
    );

    require(
        result.failed(),
        "Conflicting Content-Length must fail"
    );

    const auto output = drainOutput(connection);

    requireEqual(
        responseStatus(output),
        400,
        "Conflicting framing should return HTTP 400"
    );
}

// ============================================================
// Test 16: Body Size Limit
// ============================================================

LAMP_TEST(body_limit_generates_413) {
    HttpConnectionConfig config;

    config.parser.maxBodySize = 4;

    HttpConnection connection(
        makeRouter(),
        config
    );

    const auto result = connection.receive(
        "POST /echo HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: 10\r\n"
        "\r\n"
    );

    require(
        result.failed(),
        "Oversized request body must fail"
    );

    const auto output = drainOutput(connection);

    requireEqual(
        responseStatus(output),
        413,
        "Oversized body should return HTTP 413"
    );
}

// ============================================================
// Test 17: Header Count Limit
// ============================================================

LAMP_TEST(header_limit_generates_431) {
    HttpConnectionConfig config;

    config.parser.maxHeaderCount = 1;

    HttpConnection connection(
        makeRouter(),
        config
    );

    const auto result = connection.receive(
        "GET /hello HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "X-Extra: value\r\n"
        "\r\n"
    );

    require(
        result.failed(),
        "Excessive headers must fail"
    );

    const auto output = drainOutput(connection);

    requireEqual(
        responseStatus(output),
        431,
        "Excessive headers should return HTTP 431"
    );
}

// ============================================================
// Test 18: Handler Exception
// ============================================================

LAMP_TEST(handler_exception_generates_500) {
    auto router = std::make_shared<HttpRouter>();

    router->get(
        "/throw",
        [](const auto&, const auto&) -> HttpResponse {
            throw std::runtime_error(
                "Intentional handler failure"
            );
        }
    );

    HttpConnection connection(router);

    connection.receive(
        makeRequest("GET", "/throw")
    );

    const auto output = drainOutput(connection);

    requireEqual(
        responseStatus(output),
        500,
        "Handler exception should return HTTP 500"
    );
}

// ============================================================
// Test 19: Request Count
// ============================================================

LAMP_TEST(connection_tracks_request_count) {
    HttpConnection connection(makeRouter());

    for (int i = 0; i < 3; ++i) {
        const auto result = connection.receive(
            makeRequest("GET", "/ping")
        );

        requireEqual(
            result.requestsCompleted,
            std::size_t{1},
            "Expected one completed request"
        );

        drainOutput(connection);
    }

    requireEqual(
        connection.requestsHandled(),
        std::size_t{3},
        "Connection request count mismatch"
    );
}

// ============================================================
// Test 20: Maximum Requests
// ============================================================

LAMP_TEST(max_requests_closes_connection) {
    HttpConnectionConfig config;
    config.maxRequestsPerConnection = 2;

    HttpConnection connection(
        makeRouter(),
        config
    );

    connection.receive(
        makeRequest("GET", "/ping")
    );

    drainOutput(connection);

    require(
        connection.isOpen(),
        "Connection closed before request limit"
    );

    connection.receive(
        makeRequest("GET", "/ping")
    );

    requireEqual(
        connection.state(),
        HttpConnectionState::Closing,
        "Connection should close at request limit"
    );

    drainOutput(connection);

    require(
        connection.shouldClose(),
        "Connection should close after final response"
    );
}

// ============================================================
// Test 21: Disabled Keep-Alive
// ============================================================

LAMP_TEST(disabled_keep_alive_closes_connection) {
    HttpConnectionConfig config;
    config.enableKeepAlive = false;

    HttpConnection connection(
        makeRouter(),
        config
    );

    connection.receive(
        makeRequest("GET", "/ping")
    );

    requireEqual(
        connection.state(),
        HttpConnectionState::Closing,
        "Disabled keep-alive must close connection"
    );

    drainOutput(connection);

    require(
        connection.shouldClose(),
        "Connection must close after response"
    );
}

// ============================================================
// Test 22: Manual Close
// ============================================================

LAMP_TEST(manual_close_is_idempotent) {
    HttpConnection connection(makeRouter());

    connection.close();
    connection.close();

    requireEqual(
        connection.state(),
        HttpConnectionState::Closed,
        "Repeated close must preserve Closed state"
    );
}

// ============================================================
// Test 23: Close With Pending Output
// ============================================================

LAMP_TEST(manual_close_drains_pending_output) {
    HttpConnection connection(makeRouter());

    connection.receive(
        makeRequest("GET", "/hello")
    );

    connection.close();

    requireEqual(
        connection.state(),
        HttpConnectionState::Closing,
        "Close should wait for pending output"
    );

    drainOutput(connection);

    requireEqual(
        connection.state(),
        HttpConnectionState::Closed,
        "Connection should close after output drains"
    );
}

// ============================================================
// Test 24: Abort
// ============================================================

LAMP_TEST(abort_discards_pending_output) {
    HttpConnection connection(makeRouter());

    connection.receive(
        makeRequest("GET", "/hello")
    );

    require(
        connection.hasPendingOutput(),
        "Expected pending output before abort"
    );

    connection.abort();

    requireEqual(
        connection.state(),
        HttpConnectionState::Failed,
        "Abort should enter Failed state"
    );

    require(
        !connection.hasPendingOutput(),
        "Abort must discard pending output"
    );

    require(
        connection.shouldClose(),
        "Aborted connection should close"
    );
}

// ============================================================
// Test 25: Receive After Close
// ============================================================

LAMP_TEST(receive_after_close_consumes_nothing) {
    HttpConnection connection(makeRouter());

    connection.close();

    const auto result = connection.receive(
        makeRequest("GET", "/hello")
    );

    requireEqual(
        result.consumed,
        std::size_t{0},
        "Closed connection must not consume input"
    );

    requireEqual(
        result.requestsCompleted,
        std::size_t{0},
        "Closed connection must not process requests"
    );
}

// ============================================================
// Test 26: Receive After Abort
// ============================================================

LAMP_TEST(receive_after_abort_consumes_nothing) {
    HttpConnection connection(makeRouter());

    connection.abort();

    const auto result = connection.receive(
        makeRequest("GET", "/hello")
    );

    requireEqual(
        result.consumed,
        std::size_t{0},
        "Aborted connection must not consume input"
    );
}

// ============================================================
// Test 27: Small Output Buffer
// ============================================================

LAMP_TEST(output_buffer_limit_is_enforced) {
    HttpConnectionConfig config;

    config.maxPendingWriteBytes = 32;

    HttpConnection connection(
        makeRouter(),
        config
    );

    connection.receive(
        makeRequest("GET", "/hello")
    );

    require(
        connection.state() != HttpConnectionState::Open,
        "Output overflow must not leave connection open"
    );
}

// ============================================================
// Test 28: Invalid Configuration
// ============================================================

LAMP_TEST(rejects_invalid_connection_configuration) {
    HttpConnectionConfig config;

    config.maxRequestsPerConnection = 0;

    bool threw = false;

    try {
        HttpConnection connection(
            makeRouter(),
            config
        );

        (void)connection;
    } catch (const lamp::http::HttpConnectionError&) {
        threw = true;
    }

    require(
        threw,
        "Invalid connection configuration must throw"
    );
}

// ============================================================
// Test 29: HEAD Response
// ============================================================

LAMP_TEST(head_response_suppresses_body) {
    HttpConnection connection(makeRouter());

    connection.receive(
        makeRequest("HEAD", "/hello")
    );

    const auto output = drainOutput(connection);

    requireEqual(
        responseStatus(output),
        200,
        "HEAD request should return HTTP 200"
    );

    require(
        responseBody(output).empty(),
        "HEAD response must not transmit a body"
    );
}

// ============================================================
// Test 30: HTTP/1.0 Connection
// ============================================================

LAMP_TEST(http_10_defaults_to_close) {
    HttpConnection connection(makeRouter());

    const auto result = connection.receive(
        "GET /ping HTTP/1.0\r\n"
        "\r\n"
    );

    requireEqual(
        result.requestsCompleted,
        std::size_t{1},
        "HTTP/1.0 request should complete"
    );

    requireEqual(
        connection.state(),
        HttpConnectionState::Closing,
        "HTTP/1.0 should close by default"
    );

    drainOutput(connection);

    require(
        connection.shouldClose(),
        "HTTP/1.0 connection should close"
    );
}

// ============================================================
// Test Runner
// ============================================================

} // namespace

int main() {
    std::size_t passed = 0;
    std::size_t failed = 0;

    for (const auto& test : registry()) {
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
        << "\n========================================\n"
        << "Lamp HTTP Connection Tests\n"
        << "========================================\n"
        << "Passed: " << passed << '\n'
        << "Failed: " << failed << '\n'
        << "Total : " << passed + failed << '\n'
        << "========================================\n";

    return failed == 0 ? 0 : 1;
}
