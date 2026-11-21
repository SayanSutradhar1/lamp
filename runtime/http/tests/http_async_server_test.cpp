
#include "http/http_async_server.hpp"
#include "http/http_completion_queue.hpp"

#include <algorithm>
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
#include <tuple>
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

namespace {

using namespace std::chrono_literals;
using namespace lamp::http;

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket InvalidSocket = INVALID_SOCKET;

void closeSocket(NativeSocket s) noexcept {
    if (s != InvalidSocket) {
        ::closesocket(s);
    }
}

void shutdownSend(NativeSocket s) noexcept {
    if (s != InvalidSocket) {
        ::shutdown(s, SD_SEND);
    }
}

void abortSocket(NativeSocket s) noexcept {
    if (s != InvalidSocket) {
        linger lg{1, 0};
        ::setsockopt(
            s,
            SOL_SOCKET,
            SO_LINGER,
            reinterpret_cast<const char*>(&lg),
            sizeof(lg)
        );
        ::closesocket(s);
    }
}

struct WinsockInit final {
    WinsockInit() {
        WSADATA data{};
        require(::WSAStartup(MAKEWORD(2, 2), &data) == 0, "WSAStartup");
    }
    ~WinsockInit() {
        ::WSACleanup();
    }
};
#else
using NativeSocket = int;
constexpr NativeSocket InvalidSocket = -1;

void closeSocket(NativeSocket s) noexcept {
    if (s != InvalidSocket) {
        ::close(s);
    }
}

void shutdownSend(NativeSocket s) noexcept {
    if (s != InvalidSocket) {
        ::shutdown(s, SHUT_WR);
    }
}

void abortSocket(NativeSocket s) noexcept {
    if (s != InvalidSocket) {
        linger lg{1, 0};
        ::setsockopt(s, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
        ::close(s);
    }
}

struct WinsockInit final {};
#endif

std::size_t parseContentLength(std::string_view headers) {
    std::size_t pos = 0;
    while (pos < headers.size()) {
        auto end = headers.find("\r\n", pos);
        if (end == std::string_view::npos) {
            end = headers.size();
        }
        const auto line = headers.substr(pos, end - pos);
        constexpr std::string_view prefix = "Content-Length:";
        if (line.size() >= prefix.size()) {
            bool match = true;
            for (std::size_t i = 0; i < prefix.size(); ++i) {
                const char a = static_cast<char>(
                    std::tolower(static_cast<unsigned char>(line[i]))
                );
                const char b = static_cast<char>(
                    std::tolower(static_cast<unsigned char>(prefix[i]))
                );
                if (a != b) {
                    match = false;
                    break;
                }
            }
            if (match) {
                auto val = line.substr(prefix.size());
                while (!val.empty() && val.front() == ' ') {
                    val.remove_prefix(1);
                }
                std::size_t parsed = 0;
                for (char ch : val) {
                    if (ch >= '0' && ch <= '9') {
                        parsed = parsed * 10 + static_cast<std::size_t>(ch - '0');
                    }
                }
                return parsed;
            }
        }
        pos = end + 2;
    }
    return 0;
}

class TcpClient final {
public:
    explicit TcpClient(std::uint16_t port) {
        socket_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        require(socket_ != InvalidSocket, "socket failed");

        try {
#ifdef _WIN32
            const DWORD timeoutMs = 4000;
            ::setsockopt(
                socket_,
                SOL_SOCKET,
                SO_RCVTIMEO,
                reinterpret_cast<const char*>(&timeoutMs),
                sizeof(timeoutMs)
            );
            ::setsockopt(
                socket_,
                SOL_SOCKET,
                SO_SNDTIMEO,
                reinterpret_cast<const char*>(&timeoutMs),
                sizeof(timeoutMs)
            );
#else
            timeval tv{4, 0};
            ::setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            ::setsockopt(socket_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            require(
                ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) == 1,
                "inet_pton failed"
            );
            require(
                ::connect(
                    socket_,
                    reinterpret_cast<const sockaddr*>(&addr),
                    sizeof(addr)
                ) == 0,
                "connect failed"
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
            const int len = static_cast<int>(
                std::min<std::size_t>(data.size(), 16 * 1024)
            );
            const int written = ::send(socket_, data.data(), len, 0);
            require(written > 0, "send failed");
            data.remove_prefix(static_cast<std::size_t>(written));
        }
    }

    void halfClose() {
        shutdownSend(socket_);
    }

    void resetAbort() {
        abortSocket(socket_);
        socket_ = InvalidSocket;
    }

    std::string receiveResponse() {
        while (true) {
            const auto headerEnd = pending_.find("\r\n\r\n");
            if (headerEnd != std::string::npos) {
                const auto bodyLen = parseContentLength(
                    std::string_view(pending_.data(), headerEnd)
                );
                const auto total = headerEnd + 4 + bodyLen;
                if (pending_.size() >= total) {
                    std::string out = pending_.substr(0, total);
                    pending_.erase(0, total);
                    return out;
                }
            }

            char buf[8192];
            const int n = ::recv(socket_, buf, sizeof(buf), 0);
            require(n > 0, "recv failed or closed prematurely");
            pending_.append(buf, static_cast<std::size_t>(n));
        }
    }

    bool waitForEof() {
        char buf[64];
        const int n = ::recv(socket_, buf, sizeof(buf), 0);
        return n == 0;
    }

private:
    NativeSocket socket_ = InvalidSocket;
    std::string pending_;
};

void testStartAndShutdownLifecycle() {
    auto router = std::make_shared<HttpRouter>();
    router->get("/ping", [](const HttpRequest&, const HttpRouteContext&) {
        HttpResponse resp(HttpStatus::Ok);
        resp.setBody("pong");
        return resp;
    });

    for (int i = 0; i < 3; ++i) {
        HttpAsyncServerConfig cfg;
        cfg.port = 0;
        HttpAsyncServer server(router, cfg);
        require(server.state() == HttpAsyncServerState::Created, "Created");
        server.start();
        require(server.isRunning(), "Running");
        require(server.port() != 0, "Ephemeral port assigned");

        TcpClient client(server.port());
        client.sendAll("GET /ping HTTP/1.1\r\nHost: localhost\r\n\r\n");
        const auto resp = client.receiveResponse();
        require(
            resp.find("200 OK") != std::string::npos &&
            resp.find("pong") != std::string::npos,
            "Got pong"
        );

        server.shutdown();
        require(server.isStopped(), "Stopped");
    }
}

void testConcurrentClientsAndKeepAlive() {
    auto router = std::make_shared<HttpRouter>();
    router->get("/echo/:id", [](const HttpRequest&, const HttpRouteContext& ctx) {
        HttpResponse resp(HttpStatus::Ok);
        resp.setBody(ctx.param("id"));
        return resp;
    });

    HttpAsyncServerConfig cfg;
    cfg.port = 0;
    HttpAsyncServer server(router, cfg);
    server.start();

    constexpr int kClients = 6;
    constexpr int kRequestsPerClient = 5;
    std::vector<std::thread> threads;
    std::atomic<int> failures{0};

    for (int c = 0; c < kClients; ++c) {
        threads.emplace_back([&, c]() {
            try {
                TcpClient client(server.port());
                for (int r = 0; r < kRequestsPerClient; ++r) {
                    const std::string id =
                        std::to_string(c) + "_" + std::to_string(r);
                    client.sendAll(
                        "GET /echo/" + id +
                        " HTTP/1.1\r\nHost: localhost\r\n\r\n"
                    );
                    const auto resp = client.receiveResponse();
                    if (resp.find("200 OK") == std::string::npos ||
                        resp.find(id) == std::string::npos) {
                        ++failures;
                    }
                }
            } catch (...) {
                ++failures;
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    require(failures.load() == 0, "Concurrent client requests failed");
    const auto st = server.stats();
    require(
        st.dispatchedRequests ==
            static_cast<std::uint64_t>(kClients * kRequestsPerClient),
        "Dispatched request count matches"
    );

    server.shutdown();
}

void testAsyncDeferredOutOfOrderPipelinedResponses() {
    auto queue = std::make_shared<HttpCompletionQueue>();
    std::mutex pendingMutex;
    std::vector<std::pair<HttpCompletionToken, std::weak_ptr<HttpAsyncConnection>>>
        pending;
    std::atomic<int> dispatched{0};

    HttpAsyncServer* serverPtr = nullptr;

    HttpAsyncServerCallbacks cbs;
    cbs.onAccept = [&](std::weak_ptr<HttpAsyncConnection> conn) {
        return [&, conn](const HttpCompletionToken& token, HttpRequest) {
            {
                std::lock_guard lock(pendingMutex);
                pending.emplace_back(token, conn);
            }
            ++dispatched;
        };
    };
    cbs.onTick = [&]() {
        (void)queue->drain(64);
    };

    HttpAsyncServerConfig cfg;
    cfg.port = 0;
    HttpAsyncServer server(std::move(cbs), cfg);
    serverPtr = &server;
    server.start();

    TcpClient client(server.port());
    client.sendAll(
        "GET /first HTTP/1.1\r\nHost: localhost\r\n\r\n"
        "GET /second HTTP/1.1\r\nHost: localhost\r\n\r\n"
    );

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (dispatched.load() < 2 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(2ms);
    }
    require(dispatched.load() == 2, "Both pipelined requests dispatched");

    // Complete 2nd BEFORE 1st from an external worker thread and wake reactor
    std::thread worker([&]() {
        std::pair<HttpCompletionToken, std::weak_ptr<HttpAsyncConnection>> first;
        std::pair<HttpCompletionToken, std::weak_ptr<HttpAsyncConnection>> second;
        {
            std::lock_guard lock(pendingMutex);
            first = pending[0];
            second = pending[1];
        }

        (void)queue->post(HttpCompletion{
            second.first,
            [conn = second.second](const HttpCompletionToken& tok) {
                if (auto target = conn.lock()) {
                    HttpResponse resp(HttpStatus::Ok);
                    resp.setBody("SECOND");
                    (void)target->complete(tok, std::move(resp));
                }
            }
        });
        serverPtr->wakeup();

        std::this_thread::sleep_for(15ms);

        (void)queue->post(HttpCompletion{
            first.first,
            [conn = first.second](const HttpCompletionToken& tok) {
                if (auto target = conn.lock()) {
                    HttpResponse resp(HttpStatus::Ok);
                    resp.setBody("FIRST");
                    (void)target->complete(tok, std::move(resp));
                }
            }
        });
        serverPtr->wakeup();
    });

    const auto r1 = client.receiveResponse();
    const auto r2 = client.receiveResponse();
    worker.join();

    require(r1.find("FIRST") != std::string::npos, "First wire response is FIRST");
    require(r2.find("SECOND") != std::string::npos, "Second wire response is SECOND");

    server.shutdown();
}

void testHalfCloseGracefulDrain() {
    auto queue = std::make_shared<HttpCompletionQueue>();
    std::promise<std::pair<HttpCompletionToken, std::weak_ptr<HttpAsyncConnection>>>
        captured;
    auto capturedFuture = captured.get_future();
    std::once_flag once;

    HttpAsyncServerCallbacks cbs;
    cbs.onAccept = [&](std::weak_ptr<HttpAsyncConnection> conn) {
        return [&, conn](const HttpCompletionToken& token, HttpRequest) {
            std::call_once(once, [&]() {
                captured.set_value({token, conn});
            });
        };
    };
    cbs.onTick = [&]() {
        (void)queue->drain(64);
    };

    HttpAsyncServerConfig cfg;
    cfg.port = 0;
    HttpAsyncServer server(std::move(cbs), cfg);
    server.start();

    TcpClient client(server.port());
    client.sendAll("GET /half HTTP/1.1\r\nHost: localhost\r\n\r\n");
    client.halfClose();

    auto item = capturedFuture.get();
    std::this_thread::sleep_for(20ms);

    (void)queue->post(HttpCompletion{
        item.first,
        [conn = item.second](const HttpCompletionToken& tok) {
            if (auto target = conn.lock()) {
                HttpResponse resp(HttpStatus::Ok);
                resp.setBody("HALF_OK");
                (void)target->complete(tok, std::move(resp));
            }
        }
    });
    server.wakeup();

    const auto resp = client.receiveResponse();
    require(resp.find("HALF_OK") != std::string::npos, "Half-closed client received response");

    server.shutdown();
}

void testLargeResponsePartialSocketWrites() {
    const std::string largeBody(512 * 1024, 'Z');
    auto router = std::make_shared<HttpRouter>();
    router->get("/large", [&](const HttpRequest&, const HttpRouteContext&) {
        HttpResponse resp(HttpStatus::Ok);
        resp.setBody(largeBody);
        return resp;
    });

    HttpAsyncServerConfig cfg;
    cfg.port = 0;
    cfg.connection.maxPendingWriteBytes = 1024 * 1024;
    HttpAsyncServer server(router, cfg);
    server.start();

    TcpClient client(server.port());
    client.sendAll("GET /large HTTP/1.1\r\nHost: localhost\r\n\r\n");
    const auto resp = client.receiveResponse();
    require(resp.find("200 OK") != std::string::npos, "Large 200 OK");
    require(resp.find(largeBody) != std::string::npos, "Entire 512KB body received");

    server.shutdown();
}

void testClientDisconnectTriggersClosedCallback() {
    std::promise<std::pair<std::uint64_t, std::uint64_t>> disconnected;
    auto disconnectedFuture = disconnected.get_future();
    std::once_flag discOnce;

    std::promise<void> accepted;
    auto acceptedFuture = accepted.get_future();
    std::once_flag accOnce;

    HttpAsyncServerCallbacks cbs;
    cbs.onAccept = [&](std::weak_ptr<HttpAsyncConnection>) {
        return [&](const HttpCompletionToken&, HttpRequest) {
            std::call_once(accOnce, [&]() { accepted.set_value(); });
        };
    };
    cbs.onConnectionClosed = [&](std::uint64_t id, std::uint64_t gen) {
        std::call_once(discOnce, [&]() {
            disconnected.set_value({id, gen});
        });
    };

    HttpAsyncServerConfig cfg;
    cfg.port = 0;
    HttpAsyncServer server(std::move(cbs), cfg);
    server.start();

    {
        TcpClient client(server.port());
        client.sendAll("GET /hang HTTP/1.1\r\nHost: localhost\r\n\r\n");
        require(
            acceptedFuture.wait_for(2s) == std::future_status::ready,
            "Request arrived"
        );
        client.resetAbort();
    }

    require(
        disconnectedFuture.wait_for(3s) == std::future_status::ready,
        "Disconnect callback fired"
    );
    const auto [id, gen] = disconnectedFuture.get();
    require(id != 0 && gen != 0, "Valid connection id and generation reported");

    server.shutdown();
}

void testGracefulShutdownWhileInflight() {
    auto queue = std::make_shared<HttpCompletionQueue>();
    std::promise<std::pair<HttpCompletionToken, std::weak_ptr<HttpAsyncConnection>>>
        captured;
    auto capturedFuture = captured.get_future();
    std::once_flag once;

    HttpAsyncServerCallbacks cbs;
    cbs.onAccept = [&](std::weak_ptr<HttpAsyncConnection> conn) {
        return [&, conn](const HttpCompletionToken& token, HttpRequest) {
            std::call_once(once, [&]() {
                captured.set_value({token, conn});
            });
        };
    };
    cbs.onTick = [&]() {
        (void)queue->drain(64);
    };

    HttpAsyncServerConfig cfg;
    cfg.port = 0;
    cfg.shutdownTimeout = 2000ms;
    HttpAsyncServer server(std::move(cbs), cfg);
    server.start();

    TcpClient client(server.port());
    client.sendAll("GET /drain HTTP/1.1\r\nHost: localhost\r\n\r\n");
    auto item = capturedFuture.get();

    std::thread finisher([&]() {
        std::this_thread::sleep_for(30ms);
        (void)queue->post(HttpCompletion{
            item.first,
            [conn = item.second](const HttpCompletionToken& tok) {
                if (auto target = conn.lock()) {
                    HttpResponse resp(HttpStatus::Ok);
                    resp.setBody("DRAINED");
                    (void)target->complete(tok, std::move(resp));
                }
            }
        });
        server.wakeup();
    });

    server.shutdown();
    finisher.join();

    const auto resp = client.receiveResponse();
    require(resp.find("DRAINED") != std::string::npos, "In-flight response drained before shutdown");
    require(server.isStopped(), "Server stopped");
}

void testIdleTimeoutClosesConnection() {
    auto router = std::make_shared<HttpRouter>();
    router->get("/ok", [](const HttpRequest&, const HttpRouteContext&) {
        HttpResponse resp(HttpStatus::Ok);
        resp.setBody("OK");
        return resp;
    });

    HttpAsyncServerConfig cfg;
    cfg.port = 0;
    cfg.idleTimeout = 60ms;
    cfg.pollTimeout = 10ms;
    HttpAsyncServer server(router, cfg);
    server.start();

    TcpClient client(server.port());
    client.sendAll("GET /ok HTTP/1.1\r\nHost: localhost\r\n\r\n");
    const auto resp = client.receiveResponse();
    require(resp.find("OK") != std::string::npos, "Initial request succeeded");

    require(client.waitForEof(), "Server closed idle connection");

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (server.activeConnectionCount() > 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    require(server.activeConnectionCount() == 0, "Active connections dropped to 0");

    server.shutdown();
}

void testForcedShutdownTimeoutAbortsStuckConnection() {
    std::promise<void> accepted;
    auto acceptedFuture = accepted.get_future();
    std::once_flag accOnce;

    std::promise<void> closedCb;
    auto closedCbFuture = closedCb.get_future();
    std::once_flag closeOnce;

    HttpAsyncServerCallbacks cbs;
    cbs.onAccept = [&](std::weak_ptr<HttpAsyncConnection>) {
        return [&](const HttpCompletionToken&, HttpRequest) {
            std::call_once(accOnce, [&]() { accepted.set_value(); });
            // Deliberately never complete!
        };
    };
    cbs.onConnectionClosed = [&](std::uint64_t, std::uint64_t) {
        std::call_once(closeOnce, [&]() { closedCb.set_value(); });
    };

    HttpAsyncServerConfig cfg;
    cfg.port = 0;
    cfg.shutdownTimeout = 80ms;
    cfg.pollTimeout = 10ms;
    HttpAsyncServer server(std::move(cbs), cfg);
    server.start();

    TcpClient client(server.port());
    client.sendAll("GET /stuck HTTP/1.1\r\nHost: localhost\r\n\r\n");
    require(
        acceptedFuture.wait_for(2s) == std::future_status::ready,
        "Stuck request accepted"
    );

    const auto startTime = std::chrono::steady_clock::now();
    server.shutdown();
    const auto elapsed = std::chrono::steady_clock::now() - startTime;

    require(server.isStopped(), "Server stopped after forced timeout");
    require(elapsed < 1500ms, "Forced shutdown returned promptly");
    require(
        closedCbFuture.wait_for(500ms) == std::future_status::ready,
        "onConnectionClosed fired on forced abort"
    );
}

void testConcurrentMultiClientPipelinedStress() {
    auto queue = std::make_shared<HttpCompletionQueue>(
        HttpCompletionQueueConfig{4096}
    );
    HttpAsyncServer* serverPtr = nullptr;

    std::mutex workMutex;
    std::vector<std::tuple<HttpCompletionToken, std::weak_ptr<HttpAsyncConnection>, std::string>>
        workItems;
    std::condition_variable workCv;
    std::atomic<bool> stopWorkers{false};

    HttpAsyncServerCallbacks cbs;
    cbs.onAccept = [&](std::weak_ptr<HttpAsyncConnection> conn) {
        return [&, conn](const HttpCompletionToken& token, HttpRequest req) {
            {
                std::lock_guard lock(workMutex);
                workItems.emplace_back(token, conn, req.path());
            }
            workCv.notify_one();
        };
    };
    cbs.onTick = [&]() {
        (void)queue->drain(256);
    };

    HttpAsyncServerConfig cfg;
    cfg.port = 0;
    cfg.maxConnections = 64;
    cfg.connection.maxInflightRequests = 32;
    HttpAsyncServer server(std::move(cbs), cfg);
    serverPtr = &server;
    server.start();

    std::vector<std::thread> workers;
    for (int w = 0; w < 4; ++w) {
        workers.emplace_back([&]() {
            while (true) {
                std::tuple<HttpCompletionToken, std::weak_ptr<HttpAsyncConnection>, std::string>
                    item;
                {
                    std::unique_lock lock(workMutex);
                    workCv.wait(lock, [&]() {
                        return stopWorkers.load() || !workItems.empty();
                    });
                    if (workItems.empty() && stopWorkers.load()) {
                        return;
                    }
                    item = std::move(workItems.back());
                    workItems.pop_back();
                }

                (void)queue->post(HttpCompletion{
                    std::get<0>(item),
                    [conn = std::get<1>(item), path = std::move(std::get<2>(item))](
                        const HttpCompletionToken& tok
                    ) {
                        if (auto target = conn.lock()) {
                            HttpResponse resp(HttpStatus::Ok);
                            resp.setBody(path);
                            (void)target->complete(tok, std::move(resp));
                        }
                    }
                });
                serverPtr->wakeup();
            }
        });
    }

    constexpr int kClients = 8;
    constexpr int kBatches = 3;
    constexpr int kPipelineDepth = 5;
    std::atomic<int> failures{0};
    std::vector<std::thread> clientThreads;

    for (int c = 0; c < kClients; ++c) {
        clientThreads.emplace_back([&, c]() {
            try {
                TcpClient client(server.port());
                for (int b = 0; b < kBatches; ++b) {
                    std::string batch;
                    std::vector<std::string> expectedPaths;
                    for (int p = 0; p < kPipelineDepth; ++p) {
                        std::string path =
                            "/c" + std::to_string(c) +
                            "/b" + std::to_string(b) +
                            "/p" + std::to_string(p);
                        batch += "GET " + path +
                                 " HTTP/1.1\r\nHost: localhost\r\n\r\n";
                        expectedPaths.push_back(std::move(path));
                    }
                    client.sendAll(batch);
                    for (const auto& expected : expectedPaths) {
                        const auto resp = client.receiveResponse();
                        if (resp.find("200 OK") == std::string::npos ||
                            resp.find(expected) == std::string::npos) {
                            ++failures;
                        }
                    }
                }
            } catch (...) {
                ++failures;
            }
        });
    }

    for (auto& t : clientThreads) {
        t.join();
    }

    stopWorkers.store(true);
    workCv.notify_all();
    for (auto& w : workers) {
        w.join();
    }

    require(failures.load() == 0, "Concurrent pipelined stress had failures");
    const auto st = server.stats();
    require(
        st.dispatchedRequests ==
            static_cast<std::uint64_t>(kClients * kBatches * kPipelineDepth),
        "All stress requests dispatched"
    );

    server.shutdown();
}

} // namespace

int main() {
    WinsockInit net;

    struct TestCase {
        const char* name;
        std::function<void()> run;
    };

    const std::vector<TestCase> tests = {
        {"StartAndShutdownLifecycle", testStartAndShutdownLifecycle},
        {"ConcurrentClientsAndKeepAlive", testConcurrentClientsAndKeepAlive},
        {"AsyncDeferredOutOfOrderPipelinedResponses",
         testAsyncDeferredOutOfOrderPipelinedResponses},
        {"HalfCloseGracefulDrain", testHalfCloseGracefulDrain},
        {"LargeResponsePartialSocketWrites", testLargeResponsePartialSocketWrites},
        {"ClientDisconnectTriggersClosedCallback",
         testClientDisconnectTriggersClosedCallback},
        {"GracefulShutdownWhileInflight", testGracefulShutdownWhileInflight},
        {"IdleTimeoutClosesConnection", testIdleTimeoutClosesConnection},
        {"ForcedShutdownTimeoutAbortsStuckConnection",
         testForcedShutdownTimeoutAbortsStuckConnection},
        {"ConcurrentMultiClientPipelinedStress",
         testConcurrentMultiClientPipelinedStress}
    };

    std::size_t passed = 0;
    for (const auto& test : tests) {
        try {
            test.run();
            ++passed;
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception& error) {
            std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n';
            return 1;
        } catch (...) {
            std::cerr << "[FAIL] " << test.name << ": unknown exception\n";
            return 1;
        }
    }

    std::cout
        << "Lamp async HTTP server: "
        << passed << "/" << tests.size()
        << " tests passed\n";
    return 0;
}
