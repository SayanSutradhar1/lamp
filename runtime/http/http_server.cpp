
#include "http/http_server.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
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
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace lamp::http {

namespace {

#ifdef _WIN32

using SocketHandle = SOCKET;

constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;

[[nodiscard]]
int socketError() noexcept {
    return WSAGetLastError();
}

[[nodiscard]]
bool isInterrupted(int error) noexcept {
    return error == WSAEINTR;
}

[[nodiscard]]
bool isTimeout(int error) noexcept {
    return error == WSAETIMEDOUT ||
           error == WSAEWOULDBLOCK;
}

void initializeSockets() {
    WSADATA data{};

    const int result =
        WSAStartup(MAKEWORD(2, 2), &data);

    if (result != 0) {
        throw HttpServerError(
            "WSAStartup failed: " +
            std::to_string(result)
        );
    }
}

void cleanupSockets() noexcept {
    WSACleanup();
}

#else

using SocketHandle = int;

constexpr SocketHandle kInvalidSocket = -1;

[[nodiscard]]
int socketError() noexcept {
    return errno;
}

[[nodiscard]]
bool isInterrupted(int error) noexcept {
    return error == EINTR;
}

[[nodiscard]]
bool isTimeout(int error) noexcept {
    return error == EAGAIN ||
           error == EWOULDBLOCK ||
           error == ETIMEDOUT;
}

void initializeSockets() {}

void cleanupSockets() noexcept {}

#endif

[[nodiscard]]
std::string socketErrorMessage(
    std::string_view operation
) {
    return std::string(operation) +
           " failed with socket error " +
           std::to_string(socketError());
}

void closeNativeSocket(SocketHandle socket) noexcept {
    if (socket == kInvalidSocket) {
        return;
    }

#ifdef _WIN32
    ::closesocket(socket);
#else
    ::close(socket);
#endif
}

void shutdownNativeSocket(SocketHandle socket) noexcept {
    if (socket == kInvalidSocket) {
        return;
    }

#ifdef _WIN32
    ::shutdown(socket, SD_BOTH);
#else
    ::shutdown(socket, SHUT_RDWR);
#endif
}

[[nodiscard]]
bool setSocketTimeout(
    SocketHandle socket,
    int option,
    std::chrono::milliseconds timeout
) noexcept {
#ifdef _WIN32
    const auto bounded =
        std::clamp<std::int64_t>(
            timeout.count(),
            1,
            std::numeric_limits<DWORD>::max()
        );

    const DWORD value = static_cast<DWORD>(bounded);

    return ::setsockopt(
        socket,
        SOL_SOCKET,
        option,
        reinterpret_cast<const char*>(&value),
        sizeof(value)
    ) == 0;
#else
    const auto seconds =
        std::chrono::duration_cast<std::chrono::seconds>(
            timeout
        );

    const auto remainder = timeout - seconds;

    timeval value{};

    value.tv_sec =
        static_cast<decltype(value.tv_sec)>(
            seconds.count()
        );

    value.tv_usec =
        static_cast<decltype(value.tv_usec)>(
            std::chrono::duration_cast<
                std::chrono::microseconds
            >(remainder).count()
        );

    return ::setsockopt(
        socket,
        SOL_SOCKET,
        option,
        &value,
        sizeof(value)
    ) == 0;
#endif
}

[[nodiscard]]
bool waitForReadable(
    SocketHandle socket,
    std::chrono::milliseconds timeout
) noexcept {
#ifndef _WIN32
    if (socket < 0 || socket >= FD_SETSIZE) {
        return false;
    }
#endif

    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(socket, &readSet);

    timeval waitTime{};
    waitTime.tv_sec = static_cast<long>(
        timeout.count() / 1000
    );
    waitTime.tv_usec = static_cast<long>(
        (timeout.count() % 1000) * 1000
    );

#ifdef _WIN32
    const int result =
        ::select(0, &readSet, nullptr, nullptr, &waitTime);
#else
    const int result =
        ::select(
            socket + 1,
            &readSet,
            nullptr,
            nullptr,
            &waitTime
        );
#endif

    return result > 0 && FD_ISSET(socket, &readSet);
}

[[nodiscard]]
int receiveBytes(
    SocketHandle socket,
    char* buffer,
    std::size_t capacity
) noexcept {
    const auto length = static_cast<int>(
        std::min<std::size_t>(
            capacity,
            std::numeric_limits<int>::max()
        )
    );

#ifdef _WIN32
    return ::recv(socket, buffer, length, 0);
#else
    return static_cast<int>(
        ::recv(socket, buffer, length, 0)
    );
#endif
}

[[nodiscard]]
int sendBytes(
    SocketHandle socket,
    const char* data,
    std::size_t size
) noexcept {
    const auto length = static_cast<int>(
        std::min<std::size_t>(
            size,
            std::numeric_limits<int>::max()
        )
    );

#ifdef _WIN32
    return ::send(socket, data, length, 0);
#else
#ifdef MSG_NOSIGNAL
    return static_cast<int>(
        ::send(socket, data, length, MSG_NOSIGNAL)
    );
#else
    return static_cast<int>(
        ::send(socket, data, length, 0)
    );
#endif
#endif
}

} // namespace

// ============================================================
// Construction / Destruction
// ============================================================

HttpServer::HttpServer(HttpServerConfig config)
    : config_(std::move(config)),
      router_(std::make_shared<HttpRouter>()) {
    validateConfig();
}

HttpServer::~HttpServer() noexcept {
    try {
        shutdown();
    } catch (...) {
        std::terminate();
    }
}

// ============================================================
// Configuration Validation
// ============================================================

void HttpServer::validateConfig() const {
    if (config_.host.empty()) {
        throw HttpServerError(
            "HTTP server host cannot be empty"
        );
    }

    if (config_.listenBacklog <= 0) {
        throw HttpServerError(
            "HTTP server backlog must be positive"
        );
    }

    if (config_.maxConnections == 0) {
        throw HttpServerError(
            "Maximum connections must be positive"
        );
    }

    if (config_.readBufferSize == 0) {
        throw HttpServerError(
            "Read buffer size must be positive"
        );
    }

    if (config_.readTimeout.count() <= 0 ||
        config_.writeTimeout.count() <= 0 ||
        config_.shutdownTimeout.count() <= 0) {
        throw HttpServerError(
            "HTTP server timeouts must be positive"
        );
    }

    if (config_.connection.maxPendingWriteBytes == 0 ||
        config_.connection.maxRequestsPerConnection == 0) {
        throw HttpServerError(
            "HTTP connection limits must be positive"
        );
    }
}

// ============================================================
// Listener Management
// ============================================================

void HttpServer::openListener() {
    initializeSockets();

    SocketHandle socket = kInvalidSocket;

    try {
        socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

        if (socket == kInvalidSocket) {
            throw HttpServerError(
                socketErrorMessage("socket")
            );
        }

        int reuseAddress = 1;

#ifdef _WIN32
        // Exclusive binding avoids accidental port sharing
        // with another Windows process.
        reuseAddress = 1;

        if (::setsockopt(
                socket,
                SOL_SOCKET,
                SO_EXCLUSIVEADDRUSE,
                reinterpret_cast<const char*>(&reuseAddress),
                sizeof(reuseAddress)
            ) != 0) {
            throw HttpServerError(
                socketErrorMessage("setsockopt")
            );
        }
#else
        if (::setsockopt(
                socket,
                SOL_SOCKET,
                SO_REUSEADDR,
                &reuseAddress,
                sizeof(reuseAddress)
            ) != 0) {
            throw HttpServerError(
                socketErrorMessage("setsockopt")
            );
        }
#endif

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(config_.port);

        if (::inet_pton(
                AF_INET,
                config_.host.c_str(),
                &address.sin_addr
            ) != 1) {
            throw HttpServerError(
                "Invalid IPv4 HTTP server host"
            );
        }

        if (::bind(
                socket,
                reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)
            ) != 0) {
            throw HttpServerError(
                socketErrorMessage("bind")
            );
        }

        if (::listen(
                socket,
                config_.listenBacklog
            ) != 0) {
            throw HttpServerError(
                socketErrorMessage("listen")
            );
        }

        sockaddr_in boundAddress{};

#ifdef _WIN32
        int addressLength = sizeof(boundAddress);
#else
        socklen_t addressLength = sizeof(boundAddress);
#endif

        if (::getsockname(
                socket,
                reinterpret_cast<sockaddr*>(&boundAddress),
                &addressLength
            ) != 0) {
            throw HttpServerError(
                socketErrorMessage("getsockname")
            );
        }

        boundPort_.store(
            ntohs(boundAddress.sin_port),
            std::memory_order_release
        );

        // The listener is published before the accept
        // thread starts.
        listenerSocket_ =
            static_cast<NativeSocket>(socket);

    } catch (...) {
        closeNativeSocket(socket);
        cleanupSockets();
        boundPort_.store(0);
        throw;
    }
}

void HttpServer::closeListener() noexcept {
    const auto socket =
        static_cast<SocketHandle>(listenerSocket_);

    listenerSocket_ = InvalidSocket;

    closeNativeSocket(socket);
    boundPort_.store(0, std::memory_order_release);

    cleanupSockets();
}

// ============================================================
// Server Startup
// ============================================================

void HttpServer::start() {
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);

        if (state_ != HttpServerState::Created) {
            throw HttpServerError(
                "HTTP server can only start from Created"
            );
        }

        state_ = HttpServerState::Starting;
    }

    shutdownRequested_.store(
        false,
        std::memory_order_release
    );

    try {
        openListener();

        acceptThread_ = std::thread(
            [this]() {
                acceptLoop();
            }
        );

        {
            std::lock_guard<std::mutex> lock(lifecycleMutex_);

            state_ = HttpServerState::Running;
        }

        lifecycleCondition_.notify_all();

    } catch (...) {
        shutdownRequested_.store(
            true,
            std::memory_order_release
        );

        if (acceptThread_.joinable()) {
            acceptThread_.join();
        }

        if (listenerSocket_ != InvalidSocket) {
            closeListener();
        }

        {
            std::lock_guard<std::mutex> lock(lifecycleMutex_);
            state_ = HttpServerState::Failed;
        }

        lifecycleCondition_.notify_all();
        throw;
    }
}

// ============================================================
// Accept Loop
// ============================================================

void HttpServer::acceptLoop() noexcept {
    const auto listener =
        static_cast<SocketHandle>(listenerSocket_);

    while (!isShutdownRequested()) {
        // A bounded wait allows shutdown to stop the
        // accept loop without relying on cross-platform
        // behavior of closing a blocked accept socket.

        if (!waitForReadable(
                listener,
                std::chrono::milliseconds{200}
            )) {
            continue;
        }

        if (isShutdownRequested()) {
            break;
        }

        sockaddr_in clientAddress{};

#ifdef _WIN32
        int addressLength = sizeof(clientAddress);
#else
        socklen_t addressLength = sizeof(clientAddress);
#endif

        const SocketHandle clientSocket = ::accept(
            listener,
            reinterpret_cast<sockaddr*>(&clientAddress),
            &addressLength
        );

        if (clientSocket == kInvalidSocket) {
            if (isShutdownRequested()) {
                break;
            }

            if (isInterrupted(socketError())) {
                continue;
            }

            continue;
        }

        if (isShutdownRequested()) {
            closeNativeSocket(clientSocket);
            break;
        }

        try {
            acceptClient(
                static_cast<NativeSocket>(clientSocket)
            );
        } catch (...) {
            rejectedConnections_.fetch_add(
                1,
                std::memory_order_relaxed
            );

            closeNativeSocket(clientSocket);
        }

        reapFinishedConnections();
    }
}

// ============================================================
// Accept Client
// ============================================================

void HttpServer::acceptClient(
    NativeSocket clientSocket
) {
    const auto socket =
        static_cast<SocketHandle>(clientSocket);

    if (isShutdownRequested() ||
        activeConnections_.load(
            std::memory_order_acquire
        ) >= config_.maxConnections) {
        rejectedConnections_.fetch_add(
            1,
            std::memory_order_relaxed
        );

        closeNativeSocket(socket);
        return;
    }

    if (!setSocketTimeout(
            socket,
            SO_RCVTIMEO,
            config_.readTimeout
        ) ||
        !setSocketTimeout(
            socket,
            SO_SNDTIMEO,
            config_.writeTimeout
        )) {
        throw HttpServerError(
            "Failed to configure client socket timeouts"
        );
    }

    auto client = std::make_shared<ClientConnection>();

    client->id = nextConnectionId_.fetch_add(
        1,
        std::memory_order_relaxed
    );

    client->socket = clientSocket;

    client->protocol = std::make_shared<HttpConnection>(
        router_,
        config_.connection
    );

    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);

        if (isShutdownRequested()) {
            closeNativeSocket(socket);
            return;
        }

        connections_.emplace(client->id, client);

        activeConnections_.fetch_add(
            1,
            std::memory_order_relaxed
        );
    }

    try {
        client->worker = std::thread(
            [this, client]() noexcept {
                clientLoop(client);
            }
        );

        acceptedConnections_.fetch_add(
            1,
            std::memory_order_relaxed
        );

    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(
                connectionsMutex_
            );

            connections_.erase(client->id);

            activeConnections_.fetch_sub(
                1,
                std::memory_order_relaxed
            );
        }

        throw;
    }
}

// ============================================================
// Client Processing
// ============================================================

void HttpServer::clientLoop(
    const std::shared_ptr<ClientConnection>& client
) noexcept {
    const auto socket =
        static_cast<SocketHandle>(client->socket);

    try {
        std::vector<char> buffer(
            config_.readBufferSize
        );

        bool connected = true;

        while (connected && !isShutdownRequested()) {
            const int received = receiveBytes(
                socket,
                buffer.data(),
                buffer.size()
            );

            if (received == 0) {
                break;
            }

            if (received < 0) {
                const int error = socketError();

                if (isInterrupted(error)) {
                    continue;
                }

                if (isTimeout(error)) {
                    // The initial implementation treats
                    // a receive timeout as an idle
                    // connection timeout.
                    break;
                }

                break;
            }

            std::string_view input(
                buffer.data(),
                static_cast<std::size_t>(received)
            );

            while (!input.empty() && connected) {
                const auto result =
                    client->protocol->receive(input);

                completedRequests_.fetch_add(
                    result.requestsCompleted,
                    std::memory_order_relaxed
                );

                input.remove_prefix(result.consumed);

                // Flush all queued output before parsing
                // more requests from this read buffer.
                while (client->protocol->hasPendingOutput()) {
                    const std::string output =
                        client->protocol->pendingOutput(
                            config_.readBufferSize
                        );

                    if (output.empty()) {
                        connected = false;
                        break;
                    }

                    const int written = sendBytes(
                        socket,
                        output.data(),
                        output.size()
                    );

                    if (written < 0) {
                        if (isInterrupted(socketError())) {
                            continue;
                        }

                        connected = false;
                        break;
                    }

                    if (written == 0) {
                        connected = false;
                        break;
                    }

                    client->protocol->consumeOutput(
                        static_cast<std::size_t>(written)
                    );
                }

                if (result.shouldClose ||
                    client->protocol->shouldClose()) {
                    connected = false;
                    break;
                }

                if (result.consumed == 0) {
                    connected = false;
                    break;
                }
            }
        }

        // Flush any remaining queued responses unless
        // shutdown has explicitly interrupted the client.
        while (!isShutdownRequested() &&
               client->protocol->hasPendingOutput()) {
            const auto output =
                client->protocol->pendingOutput(
                    config_.readBufferSize
                );

            if (output.empty()) {
                break;
            }

            const int written = sendBytes(
                socket,
                output.data(),
                output.size()
            );

            if (written < 0) {
                if (isInterrupted(socketError())) {
                    continue;
                }

                break;
            }

            if (written == 0) {
                break;
            }

            client->protocol->consumeOutput(
                static_cast<std::size_t>(written)
            );
        }

    } catch (...) {
        // Client-level failures must not terminate the
        // accept loop or the server process.
    }

    client->protocol->close();

    closeNativeSocket(socket);

    client->socket = InvalidSocket;

    client->finished.store(
        true,
        std::memory_order_release
    );

    activeConnections_.fetch_sub(
        1,
        std::memory_order_relaxed
    );

    completedConnections_.fetch_add(
        1,
        std::memory_order_relaxed
    );

    lifecycleCondition_.notify_all();
}

// ============================================================
// Reap Finished Connections
// ============================================================

void HttpServer::reapFinishedConnections() {
    std::vector<std::shared_ptr<ClientConnection>> finished;

    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);

        for (auto iterator = connections_.begin();
             iterator != connections_.end();) {
            if (iterator->second->finished.load(
                    std::memory_order_acquire
                )) {
                finished.push_back(iterator->second);
                iterator = connections_.erase(iterator);
            } else {
                ++iterator;
            }
        }
    }

    for (auto& client : finished) {
        if (client->worker.joinable()) {
            client->worker.join();
        }
    }
}

// ============================================================
// Stop Active Connections
// ============================================================

void HttpServer::stopActiveConnections() noexcept {
    std::lock_guard<std::mutex> lock(connectionsMutex_);

    for (auto& [id, client] : connections_) {
        (void)id;

        if (!client->finished.load(
                std::memory_order_acquire
            )) {
            shutdownSocket(client->socket);
        }
    }
}

// ============================================================
// Join Connection Workers
// ============================================================

void HttpServer::joinConnectionWorkers() noexcept {
    std::vector<std::shared_ptr<ClientConnection>> clients;

    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);

        clients.reserve(connections_.size());

        for (auto& [id, client] : connections_) {
            (void)id;
            clients.push_back(client);
        }

        connections_.clear();
    }

    for (auto& client : clients) {
        if (client->worker.joinable()) {
            if (client->worker.get_id() ==
                std::this_thread::get_id()) {
                std::terminate();
            }

            client->worker.join();
        }
    }
}

// ============================================================
// Server Shutdown
// ============================================================

void HttpServer::shutdown() {
    {
        std::unique_lock<std::mutex> lock(lifecycleMutex_);

        lifecycleCondition_.wait(
            lock,
            [this]() {
                return state_ != HttpServerState::Starting &&
                       state_ != HttpServerState::Stopping;
            }
        );

        if (state_ == HttpServerState::Stopped) {
            return;
        }

        if (state_ == HttpServerState::Created) {
            state_ = HttpServerState::Stopped;
            lock.unlock();
            lifecycleCondition_.notify_all();
            return;
        }

        state_ = HttpServerState::Stopping;
    }

    shutdownRequested_.store(
        true,
        std::memory_order_release
    );

    // The accept loop polls with a bounded timeout.
    // Wait for it to exit before closing the listener.
    if (acceptThread_.joinable()) {
        acceptThread_.join();
    }

    // Allow active clients a bounded grace period.
    const auto deadline =
        std::chrono::steady_clock::now() +
        config_.shutdownTimeout;

    {
        std::unique_lock<std::mutex> lock(lifecycleMutex_);

        lifecycleCondition_.wait_until(
            lock,
            deadline,
            [this]() {
                return activeConnections_.load(
                    std::memory_order_acquire
                ) == 0;
            }
        );
    }

    // Interrupt clients that did not finish in time.
    stopActiveConnections();

    // All workers must be joined before resources owned
    // by the server can be destroyed.
    joinConnectionWorkers();

    if (listenerSocket_ != InvalidSocket) {
        closeListener();
    }

    {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        state_ = HttpServerState::Stopped;
    }

    lifecycleCondition_.notify_all();
}

// ============================================================
// Native Socket Helpers
// ============================================================

void HttpServer::closeSocket(
    NativeSocket socket
) noexcept {
    closeNativeSocket(
        static_cast<SocketHandle>(socket)
    );
}

void HttpServer::shutdownSocket(
    NativeSocket socket
) noexcept {
    shutdownNativeSocket(
        static_cast<SocketHandle>(socket)
    );
}

// ============================================================
// Lifecycle Helpers
// ============================================================

void HttpServer::setState(
    HttpServerState state
) noexcept {
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        state_ = state;
    }

    lifecycleCondition_.notify_all();
}

bool HttpServer::isShutdownRequested() const noexcept {
    return shutdownRequested_.load(
        std::memory_order_acquire
    );
}

// ============================================================
// State Inspection
// ============================================================

HttpServerState HttpServer::state() const noexcept {
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    return state_;
}

bool HttpServer::isRunning() const noexcept {
    return state() == HttpServerState::Running;
}

bool HttpServer::isStopping() const noexcept {
    return state() == HttpServerState::Stopping;
}

bool HttpServer::isStopped() const noexcept {
    return state() == HttpServerState::Stopped;
}

bool HttpServer::hasFailed() const noexcept {
    return state() == HttpServerState::Failed;
}

// ============================================================
// Network Information
// ============================================================

std::string HttpServer::host() const {
    return config_.host;
}

std::uint16_t HttpServer::port() const noexcept {
    return boundPort_.load(
        std::memory_order_acquire
    );
}

HttpServerConfig HttpServer::config() const {
    return config_;
}

// ============================================================
// Router Access
// ============================================================

std::shared_ptr<HttpRouter>
HttpServer::router() const noexcept {
    return router_;
}

// ============================================================
// Route Registration
// ============================================================

void HttpServer::route(
    HttpMethod method,
    std::string pattern,
    HttpRouteHandler handler
) {
    router_->add(
        method,
        std::move(pattern),
        std::move(handler)
    );
}

void HttpServer::get(
    std::string pattern,
    HttpRouteHandler handler
) {
    router_->get(
        std::move(pattern),
        std::move(handler)
    );
}

void HttpServer::post(
    std::string pattern,
    HttpRouteHandler handler
) {
    router_->post(
        std::move(pattern),
        std::move(handler)
    );
}

void HttpServer::put(
    std::string pattern,
    HttpRouteHandler handler
) {
    router_->put(
        std::move(pattern),
        std::move(handler)
    );
}

void HttpServer::patch(
    std::string pattern,
    HttpRouteHandler handler
) {
    router_->patch(
        std::move(pattern),
        std::move(handler)
    );
}

void HttpServer::del(
    std::string pattern,
    HttpRouteHandler handler
) {
    router_->del(
        std::move(pattern),
        std::move(handler)
    );
}

void HttpServer::head(
    std::string pattern,
    HttpRouteHandler handler
) {
    router_->head(
        std::move(pattern),
        std::move(handler)
    );
}

void HttpServer::options(
    std::string pattern,
    HttpRouteHandler handler
) {
    router_->options(
        std::move(pattern),
        std::move(handler)
    );
}

// ============================================================
// Statistics
// ============================================================

HttpServerStats HttpServer::stats() const noexcept {
    HttpServerStats result;

    result.acceptedConnections =
        acceptedConnections_.load(
            std::memory_order_relaxed
        );

    result.rejectedConnections =
        rejectedConnections_.load(
            std::memory_order_relaxed
        );

    result.completedConnections =
        completedConnections_.load(
            std::memory_order_relaxed
        );

    result.completedRequests =
        completedRequests_.load(
            std::memory_order_relaxed
        );

    result.activeConnections =
        activeConnections_.load(
            std::memory_order_relaxed
        );

    return result;
}

std::size_t
HttpServer::activeConnectionCount() const noexcept {
    return activeConnections_.load(
        std::memory_order_relaxed
    );
}

} // namespace lamp::http
