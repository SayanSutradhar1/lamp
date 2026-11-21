
#include "http/http_server.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
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
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

using namespace std::chrono_literals;

namespace {

// ============================================================
// Minimal Test Framework
// ============================================================

class TestFailure final : public std::runtime_error {
public:
    explicit TestFailure(const std::string& message)
        : std::runtime_error(message) {}
};

void require(
    bool condition,
    std::string_view message
) {
    if (!condition) {
        throw TestFailure(std::string(message));
    }
}

using TestFunction = void (*)();

struct TestCase final {
    const char* name;
    TestFunction function;
};

std::vector<TestCase>& testRegistry() {
    static std::vector<TestCase> tests;
    return tests;
}

struct TestRegistration final {
    TestRegistration(
        const char* name,
        TestFunction function
    ) {
        testRegistry().push_back({name, function});
    }
};

#define LAMP_TEST(name)                              \
    void name();                                     \
    const TestRegistration register_##name(          \
        #name, &name                                 \
    );                                               \
    void name()

// ============================================================
// Cross-Platform TCP Client
// ============================================================

#ifdef _WIN32

using NativeSocket = SOCKET;

constexpr NativeSocket InvalidSocket = INVALID_SOCKET;

void closeSocket(NativeSocket socket) noexcept {
    if (socket != InvalidSocket) {
        ::closesocket(socket);
    }
}

void initializeNetworking() {
    WSADATA data{};

    const int result =
        ::WSAStartup(MAKEWORD(2, 2), &data);

    require(result == 0, "WSAStartup failed");
}

void cleanupNetworking() noexcept {
    ::WSACleanup();
}

#else

using NativeSocket = int;

constexpr NativeSocket InvalidSocket = -1;

void closeSocket(NativeSocket socket) noexcept {
    if (socket != InvalidSocket) {
        ::close(socket);
    }
}

void initializeNetworking() {}

void cleanupNetworking() noexcept {}

#endif

class TcpClient final {
public:
    explicit TcpClient(std::uint16_t port) {
        socket_ = ::socket(
            AF_INET,
            SOCK_STREAM,
            IPPROTO_TCP
        );

        require(
            socket_ != InvalidSocket,
            "Failed to create TCP client socket"
        );

        try {
            configureTimeouts();

            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(port);

            require(
                ::inet_pton(
                    AF_INET,
                    "127.0.0.1",
                    &address.sin_addr
                ) == 1,
                "Failed to parse loopback address"
            );

            require(
                ::connect(
                    socket_,
                    reinterpret_cast<const sockaddr*>(
                        &address
                    ),
                    sizeof(address)
                ) == 0,
                "Failed to connect to HTTP server"
            );
        } catch (...) {
            closeSocket(socket_);
            socket_ = InvalidSocket;
            throw;
        }
    }

    ~TcpClient() {
        closeSocket(socket_);
    }

    TcpClient(const TcpClient&) = delete;
    TcpClient& operator=(const TcpClient&) = delete;

    void sendAll(std::string_view data) {
        while (!data.empty()) {
            const auto length = static_cast<int>(
                std::min<std::size_t>(
                    data.size(),
                    16 * 1024
                )
            );

#ifdef _WIN32
            const int written = ::send(
                socket_,
                data.data(),
                length,
                0
            );
#else
#ifdef MSG_NOSIGNAL
            const int written = static_cast<int>(
                ::send(
                    socket_,
                    data.data(),
                    length,
                    MSG_NOSIGNAL
                )
            );
#else
            const int written = static_cast<int>(
                ::send(
                    socket_,
                    data.data(),
                    length,
                    0
                )
            );
#endif
#endif

            require(
                written > 0,
                "Failed to send complete HTTP request"
            );

            data.remove_prefix(
                static_cast<std::size_t>(written)
            );
        }
    }

    [[nodiscard]]
    std::string receiveResponse() {
        // Preserve bytes belonging to subsequent responses.
        // This is important for HTTP pipelining.

        while (true) {
            const auto headerEnd =
                pending_.find("\r\n\r\n");

            if (headerEnd != std::string::npos) {
                const auto bodyStart = headerEnd + 4;

                const auto length =
                    parseContentLength(
                        std::string_view(
                            pending_.data(),
                            headerEnd
                        )
                    );

                require(
                    length <= 1024 * 1024,
                    "Test response exceeds size limit"
                );

                const auto totalSize =
                    bodyStart + length;

                if (pending_.size() >= totalSize) {
                    std::string response =
                        pending_.substr(0, totalSize);

                    pending_.erase(0, totalSize);

                    return response;
                }
            }

            char buffer[4096];

            const int received = ::recv(
                socket_,
                buffer,
                sizeof(buffer),
                0
            );

            require(
                received > 0,
                "HTTP response was incomplete"
            );

            pending_.append(
                buffer,
                static_cast<std::size_t>(received)
            );

            require(
                pending_.size() <= 2 * 1024 * 1024,
                "Test response buffer exceeded limit"
            );
        }
    }

private:
    void configureTimeouts() {
#ifdef _WIN32
        const DWORD timeout = 3000;

        require(
            ::setsockopt(
                socket_,
                SOL_SOCKET,
                SO_RCVTIMEO,
                reinterpret_cast<const char*>(
                    &timeout
                ),
                sizeof(timeout)
            ) == 0,
            "Failed to set receive timeout"
        );

        require(
            ::setsockopt(
                socket_,
                SOL_SOCKET,
                SO_SNDTIMEO,
                reinterpret_cast<const char*>(
                    &timeout
                ),
                sizeof(timeout)
            ) == 0,
            "Failed to set send timeout"
        );
#else
        timeval timeout{};
        timeout.tv_sec = 3;

        require(
            ::setsockopt(
                socket_,
                SOL_SOCKET,
                SO_RCVTIMEO,
                &timeout,
                sizeof(timeout)
            ) == 0,
            "Failed to set receive timeout"
        );

        require(
            ::setsockopt(
                socket_,
                SOL_SOCKET,
                SO_SNDTIMEO,
                &timeout,
                sizeof(timeout)
            ) == 0,
            "Failed to set send timeout"
        );
#endif
    }

    [[nodiscard]]
    static std::size_t parseContentLength(
        std::string_view headers
    ) {
        constexpr std::string_view name =
            "Content-Length:";

        const auto position = headers.find(name);

        if (position == std::string_view::npos) {
            return 0;
        }

        auto value = headers.substr(
            position + name.size()
        );

        const auto end = value.find("\r\n");

        if (end != std::string_view::npos) {
            value = value.substr(0, end);
        }

        while (!value.empty() &&
               value.front() == ' ') {
            value.remove_prefix(1);
        }

        std::size_t result = 0;

        require(
            !value.empty(),
            "Empty response Content-Length"
        );

        for (const char character : value) {
            require(
                character >= '0' &&
                character <= '9',
                "Invalid response Content-Length"
            );

            result =
                result * 10 +
                static_cast<std::size_t>(
                    character - '0'
                );
        }

        return result;
    }

    NativeSocket socket_ = InvalidSocket;
    std::string pending_;
};

// ============================================================
// Test Server Fixture
// ============================================================

class ServerFixture final {
public:
    ServerFixture() {
        lamp::http::HttpServerConfig config;

        config.host = "127.0.0.1";
        config.port = 0;

        config.readTimeout = 3000ms;
        config.writeTimeout = 3000ms;
        config.shutdownTimeout = 1000ms;

        server_ = std::make_unique<
            lamp::http::HttpServer
        >(config);
    }

    ~ServerFixture() {
        if (server_) {
            server_->shutdown();
        }
    }

    lamp::http::HttpServer& server() {
        return *server_;
    }

    void start() {
        server_->start();

        require(
            server_->isRunning(),
            "HTTP server failed to enter Running state"
        );

        require(
            server_->port() != 0,
            "HTTP server did not bind a TCP port"
        );
    }

    [[nodiscard]]
    std::uint16_t port() const {
        return server_->port();
    }

private:
    std::unique_ptr<lamp::http::HttpServer> server_;
};

// ============================================================
// HTTP Helpers
// ============================================================

[[nodiscard]]
std::string makeRequest(
    std::string_view method,
    std::string_view path,
    std::string_view body = {},
    std::string_view connection = "close"
) {
    std::string request;

    request += method;
    request += " ";
    request += path;
    request += " HTTP/1.1\r\n";

    request += "Host: localhost\r\n";
    request += "Connection: ";
    request += connection;
    request += "\r\n";

    if (!body.empty()) {
        request += "Content-Length: ";
        request += std::to_string(body.size());
        request += "\r\n";
    }

    request += "\r\n";
    request += body;

    return request;
}

[[nodiscard]]
int responseStatus(std::string_view response) {
    const auto firstSpace = response.find(' ');

    require(
        firstSpace != std::string_view::npos,
        "Invalid HTTP response status line"
    );

    const auto secondSpace =
        response.find(' ', firstSpace + 1);

    require(
        secondSpace != std::string_view::npos,
        "Invalid HTTP response status code"
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
    const auto separator =
        response.find("\r\n\r\n");

    require(
        separator != std::string_view::npos,
        "HTTP response has no header terminator"
    );

    return response.substr(separator + 4);
}

[[nodiscard]]
lamp::http::HttpResponse textResponse(
    std::string_view text
) {
    lamp::http::HttpResponse response(
        lamp::http::HttpStatus::Ok
    );

    response.setHeader(
        "Content-Type",
        "text/plain; charset=utf-8"
    );

    response.setBody(text);

    return response;
}

// ============================================================
// Test 1: Server Lifecycle
// ============================================================

LAMP_TEST(server_starts_and_binds_port) {
    ServerFixture fixture;

    require(
        fixture.server().state() ==
            lamp::http::HttpServerState::Created,
        "Initial server state must be Created"
    );

    fixture.start();

    require(
        fixture.port() != 0,
        "Expected an assigned listening port"
    );

    fixture.server().shutdown();

    require(
        fixture.server().isStopped(),
        "Server must stop successfully"
    );
}

// ============================================================
// Test 2: GET Request
// ============================================================

LAMP_TEST(get_request_returns_response) {
    ServerFixture fixture;

    fixture.server().get(
        "/hello",
        [](const auto&, const auto&) {
            return textResponse("Hello Lamp");
        }
    );

    fixture.start();

    TcpClient client(fixture.port());

    client.sendAll(
        makeRequest("GET", "/hello")
    );

    const auto response = client.receiveResponse();

    require(
        responseStatus(response) == 200,
        "GET request should return HTTP 200"
    );

    require(
        responseBody(response) == "Hello Lamp",
        "GET response body mismatch"
    );
}

// ============================================================
// Test 3: POST Body
// ============================================================

LAMP_TEST(post_request_receives_body) {
    ServerFixture fixture;

    fixture.server().post(
        "/echo",
        [](const auto& request, const auto&) {
            std::string body;

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

    fixture.start();

    TcpClient client(fixture.port());

    client.sendAll(
        makeRequest(
            "POST",
            "/echo",
            "Lamp HTTP Body"
        )
    );

    const auto response = client.receiveResponse();

    require(
        responseStatus(response) == 200,
        "POST request should return HTTP 200"
    );

    require(
        responseBody(response) == "Lamp HTTP Body",
        "POST body was not preserved"
    );
}

// ============================================================
// Test 4: Route Parameters
// ============================================================

LAMP_TEST(route_parameters_are_available) {
    ServerFixture fixture;

    fixture.server().get(
        "/users/:id",
        [](const auto&, const auto& context) {
            return textResponse(
                context.param("id")
            );
        }
    );

    fixture.start();

    TcpClient client(fixture.port());

    client.sendAll(
        makeRequest("GET", "/users/42")
    );

    const auto response = client.receiveResponse();

    require(
        responseStatus(response) == 200,
        "Parameterized route should match"
    );

    require(
        responseBody(response) == "42",
        "Route parameter mismatch"
    );
}

// ============================================================
// Test 5: Unknown Route
// ============================================================

LAMP_TEST(unknown_route_returns_404) {
    ServerFixture fixture;

    fixture.start();

    TcpClient client(fixture.port());

    client.sendAll(
        makeRequest("GET", "/missing")
    );

    const auto response = client.receiveResponse();

    require(
        responseStatus(response) == 404,
        "Unknown route should return HTTP 404"
    );
}

// ============================================================
// Test 6: Method Not Allowed
// ============================================================

LAMP_TEST(wrong_method_returns_405) {
    ServerFixture fixture;

    fixture.server().get(
        "/resource",
        [](const auto&, const auto&) {
            return textResponse("resource");
        }
    );

    fixture.start();

    TcpClient client(fixture.port());

    client.sendAll(
        makeRequest("POST", "/resource")
    );

    const auto response = client.receiveResponse();

    require(
        responseStatus(response) == 405,
        "Wrong method should return HTTP 405"
    );
}

// ============================================================
// Test 7: Fragmented Request
// ============================================================

LAMP_TEST(fragmented_request_is_parsed) {
    ServerFixture fixture;

    fixture.server().post(
        "/fragment",
        [](const auto& request, const auto&) {
            return textResponse(
                std::to_string(request.bodySize())
            );
        }
    );

    fixture.start();

    TcpClient client(fixture.port());

    client.sendAll(
        "POST /fragment HTTP/1.1\r\n"
        "Host: local"
    );

    client.sendAll(
        "host\r\n"
        "Content-Length: 5\r\n"
        "Connection: close\r\n"
        "\r\n"
        "he"
    );

    client.sendAll("llo");

    const auto response = client.receiveResponse();

    require(
        responseStatus(response) == 200,
        "Fragmented request should succeed"
    );

    require(
        responseBody(response) == "5",
        "Fragmented request body size mismatch"
    );
}

// ============================================================
// Test 8: Keep-Alive
// ============================================================

LAMP_TEST(keep_alive_handles_multiple_requests) {
    ServerFixture fixture;

    fixture.server().get(
        "/ping",
        [](const auto&, const auto&) {
            return textResponse("pong");
        }
    );

    fixture.start();

    TcpClient client(fixture.port());

    client.sendAll(
        makeRequest(
            "GET",
            "/ping",
            {},
            "keep-alive"
        )
    );

    const auto first = client.receiveResponse();

    require(
        responseStatus(first) == 200,
        "First keep-alive request failed"
    );

    client.sendAll(
        makeRequest(
            "GET",
            "/ping",
            {},
            "close"
        )
    );

    const auto second = client.receiveResponse();

    require(
        responseStatus(second) == 200,
        "Second keep-alive request failed"
    );

    require(
        responseBody(second) == "pong",
        "Second response body mismatch"
    );
}

// ============================================================
// Test 9: HTTP Pipelining
// ============================================================

LAMP_TEST(pipelined_requests_preserve_order) {
    ServerFixture fixture;

    fixture.server().get(
        "/first",
        [](const auto&, const auto&) {
            return textResponse("first");
        }
    );

    fixture.server().get(
        "/second",
        [](const auto&, const auto&) {
            return textResponse("second");
        }
    );

    fixture.start();

    TcpClient client(fixture.port());

    std::string requests;

    requests += makeRequest(
        "GET",
        "/first",
        {},
        "keep-alive"
    );

    requests += makeRequest(
        "GET",
        "/second",
        {},
        "close"
    );

    client.sendAll(requests);

    const auto first = client.receiveResponse();
    const auto second = client.receiveResponse();

    require(
        responseBody(first) == "first",
        "First pipelined response mismatch"
    );

    require(
        responseBody(second) == "second",
        "Second pipelined response mismatch"
    );
}

// ============================================================
// Test 10: Malformed Request
// ============================================================

LAMP_TEST(malformed_request_returns_400) {
    ServerFixture fixture;

    fixture.start();

    TcpClient client(fixture.port());

    client.sendAll(
        "GET / HTTP/1.1\n"
        "Host: localhost\n"
        "\n"
    );

    const auto response = client.receiveResponse();

    require(
        responseStatus(response) == 400,
        "Malformed request should return HTTP 400"
    );
}

// ============================================================
// Test 11: Conflicting Content-Length
// ============================================================

LAMP_TEST(conflicting_content_length_is_rejected) {
    ServerFixture fixture;

    fixture.start();

    TcpClient client(fixture.port());

    client.sendAll(
        "POST / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: 3\r\n"
        "Content-Length: 8\r\n"
        "Connection: close\r\n"
        "\r\n"
        "abc"
    );

    const auto response = client.receiveResponse();

    require(
        responseStatus(response) == 400,
        "Conflicting Content-Length must be rejected"
    );
}

// ============================================================
// Test 12: Concurrent Clients
// ============================================================

LAMP_TEST(concurrent_clients_are_supported) {
    ServerFixture fixture;

    fixture.server().get(
        "/concurrent",
        [](const auto&, const auto&) {
            return textResponse("ok");
        }
    );

    fixture.start();

    constexpr int clientCount = 12;

    std::atomic<int> successes{0};

    std::vector<std::thread> workers;
    workers.reserve(clientCount);

    for (int i = 0; i < clientCount; ++i) {
        workers.emplace_back(
            [&fixture, &successes]() {
                try {
                    TcpClient client(fixture.port());

                    client.sendAll(
                        makeRequest(
                            "GET",
                            "/concurrent"
                        )
                    );

                    const auto response =
                        client.receiveResponse();

                    if (responseStatus(response) == 200 &&
                        responseBody(response) == "ok") {
                        successes.fetch_add(
                            1,
                            std::memory_order_relaxed
                        );
                    }
                } catch (...) {
                    // Count failed clients without allowing
                    // exceptions to terminate worker threads.
                }
            }
        );
    }

    for (auto& worker : workers) {
        worker.join();
    }

    require(
        successes.load() == clientCount,
        "One or more concurrent requests failed"
    );
}

// ============================================================
// Test 13: Request Statistics
// ============================================================

LAMP_TEST(server_tracks_completed_requests) {
    ServerFixture fixture;

    fixture.server().get(
        "/stats",
        [](const auto&, const auto&) {
            return textResponse("ok");
        }
    );

    fixture.start();

    {
        TcpClient client(fixture.port());

        client.sendAll(
            makeRequest("GET", "/stats")
        );

        const auto response =
            client.receiveResponse();

        require(
            responseStatus(response) == 200,
            "Statistics test request failed"
        );
    }

    // Request completion is recorded by the worker thread.
    // Poll briefly rather than assuming immediate scheduling.

    const auto deadline =
        std::chrono::steady_clock::now() + 2s;

    while (std::chrono::steady_clock::now() < deadline) {
        if (fixture.server().stats().completedRequests >= 1) {
            break;
        }

        std::this_thread::sleep_for(5ms);
    }

    const auto stats = fixture.server().stats();

    require(
        stats.completedRequests >= 1,
        "Completed request counter was not updated"
    );
}

// ============================================================
// Test 14: Shutdown Idempotency
// ============================================================

LAMP_TEST(shutdown_can_be_called_twice) {
    ServerFixture fixture;

    fixture.start();

    fixture.server().shutdown();
    fixture.server().shutdown();

    require(
        fixture.server().isStopped(),
        "Repeated shutdown must preserve Stopped state"
    );
}

// ============================================================
// Test Runner
// ============================================================

} // namespace

int main() {
    try {
        initializeNetworking();
    } catch (const std::exception& error) {
        std::cerr
            << "Networking initialization failed: "
            << error.what()
            << '\n';

        return 1;
    }

    std::size_t passed = 0;
    std::size_t failed = 0;

    for (const auto& test : testRegistry()) {
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

    cleanupNetworking();

    std::cout
        << "\n========================================\n"
        << "Lamp HTTP Integration Tests\n"
        << "========================================\n"
        << "Passed: " << passed << '\n'
        << "Failed: " << failed << '\n'
        << "Total : " << passed + failed << '\n'
        << "========================================\n";

    return failed == 0 ? 0 : 1;
}
