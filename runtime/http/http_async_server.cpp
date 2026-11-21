
#include "http/http_async_server.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <unordered_map>
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
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace lamp::http {

namespace {

#ifdef _WIN32

using SocketHandle = SOCKET;
using PollFd = WSAPOLLFD;

constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
constexpr short kPollRead = POLLRDNORM;
constexpr short kPollWrite = POLLWRNORM;

[[nodiscard]]
int socketError() noexcept {
    return WSAGetLastError();
}

[[nodiscard]]
bool isWouldBlock(int error) noexcept {
    return error == WSAEWOULDBLOCK;
}

[[nodiscard]]
bool isInterrupted(int error) noexcept {
    return error == WSAEINTR;
}

int pollSockets(
    PollFd* fds,
    std::size_t count,
    int timeoutMs
) noexcept {
    return ::WSAPoll(
        fds,
        static_cast<ULONG>(count),
        timeoutMs
    );
}

void setNonBlocking(SocketHandle socket) {
    u_long mode = 1;
    if (::ioctlsocket(socket, FIONBIO, &mode) != 0) {
        throw HttpAsyncServerError(
            "Failed to configure nonblocking socket"
        );
    }
}

void initializeSockets() {
    WSADATA data{};
    const int res = ::WSAStartup(MAKEWORD(2, 2), &data);
    if (res != 0) {
        throw HttpAsyncServerError(
            "WSAStartup failed: " + std::to_string(res)
        );
    }
}

void cleanupSockets() noexcept {
    ::WSACleanup();
}

#else

using SocketHandle = int;
using PollFd = pollfd;

constexpr SocketHandle kInvalidSocket = -1;
constexpr short kPollRead = POLLIN;
constexpr short kPollWrite = POLLOUT;

[[nodiscard]]
int socketError() noexcept {
    return errno;
}

[[nodiscard]]
bool isWouldBlock(int error) noexcept {
    return error == EAGAIN || error == EWOULDBLOCK;
}

[[nodiscard]]
bool isInterrupted(int error) noexcept {
    return error == EINTR;
}

int pollSockets(
    PollFd* fds,
    std::size_t count,
    int timeoutMs
) noexcept {
    return ::poll(
        fds,
        static_cast<nfds_t>(count),
        timeoutMs
    );
}

void setNonBlocking(SocketHandle socket) {
    const int flags = ::fcntl(socket, F_GETFL, 0);
    if (flags < 0 ||
        ::fcntl(socket, F_SETFL, flags | O_NONBLOCK) != 0) {
        throw HttpAsyncServerError(
            "Failed to configure nonblocking socket"
        );
    }
}

void initializeSockets() {}

void cleanupSockets() noexcept {}

#endif

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

void setTcpNoDelay(SocketHandle socket) noexcept {
    int flag = 1;
#ifdef _WIN32
    ::setsockopt(
        socket,
        IPPROTO_TCP,
        TCP_NODELAY,
        reinterpret_cast<const char*>(&flag),
        sizeof(flag)
    );
#else
    ::setsockopt(
        socket,
        IPPROTO_TCP,
        TCP_NODELAY,
        &flag,
        sizeof(flag)
    );
#endif
}

[[nodiscard]]
int recvNonBlocking(
    SocketHandle socket,
    char* buffer,
    std::size_t capacity
) noexcept {
    const int len = static_cast<int>(
        std::min<std::size_t>(
            capacity,
            std::numeric_limits<int>::max()
        )
    );
#ifdef _WIN32
    return ::recv(socket, buffer, len, 0);
#else
    return static_cast<int>(::recv(socket, buffer, len, 0));
#endif
}

[[nodiscard]]
int sendNonBlocking(
    SocketHandle socket,
    const char* data,
    std::size_t size
) noexcept {
    const int len = static_cast<int>(
        std::min<std::size_t>(
            size,
            std::numeric_limits<int>::max()
        )
    );
#ifdef _WIN32
    return ::send(socket, data, len, 0);
#else
#ifdef MSG_NOSIGNAL
    return static_cast<int>(
        ::send(socket, data, len, MSG_NOSIGNAL)
    );
#else
    return static_cast<int>(::send(socket, data, len, 0));
#endif
#endif
}

[[nodiscard]]
std::pair<SocketHandle, SocketHandle>
createLoopbackWakeupPair() {
    SocketHandle tempListener =
        ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    SocketHandle writer = kInvalidSocket;
    SocketHandle reader = kInvalidSocket;

    try {
        if (tempListener == kInvalidSocket) {
            throw HttpAsyncServerError(
                "Failed to create wakeup listener socket"
            );
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = 0;
        if (::inet_pton(
                AF_INET,
                "127.0.0.1",
                &addr.sin_addr
            ) != 1) {
            throw HttpAsyncServerError(
                "Failed to parse wakeup loopback address"
            );
        }

        if (::bind(
                tempListener,
                reinterpret_cast<const sockaddr*>(&addr),
                sizeof(addr)
            ) != 0 ||
            ::listen(tempListener, 1) != 0) {
            throw HttpAsyncServerError(
                "Failed to bind wakeup listener socket"
            );
        }

        sockaddr_in bound{};
#ifdef _WIN32
        int boundLen = sizeof(bound);
#else
        socklen_t boundLen = sizeof(bound);
#endif
        if (::getsockname(
                tempListener,
                reinterpret_cast<sockaddr*>(&bound),
                &boundLen
            ) != 0) {
            throw HttpAsyncServerError(
                "Failed to query wakeup listener port"
            );
        }

        writer = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (writer == kInvalidSocket) {
            throw HttpAsyncServerError(
                "Failed to create wakeup writer socket"
            );
        }

        if (::connect(
                writer,
                reinterpret_cast<const sockaddr*>(&bound),
                sizeof(bound)
            ) != 0) {
            throw HttpAsyncServerError(
                "Failed to connect wakeup socket pair"
            );
        }

        sockaddr_in peer{};
#ifdef _WIN32
        int peerLen = sizeof(peer);
#else
        socklen_t peerLen = sizeof(peer);
#endif
        reader = ::accept(
            tempListener,
            reinterpret_cast<sockaddr*>(&peer),
            &peerLen
        );
        if (reader == kInvalidSocket) {
            throw HttpAsyncServerError(
                "Failed to accept wakeup socket pair"
            );
        }

        closeNativeSocket(tempListener);
        tempListener = kInvalidSocket;

        setNonBlocking(writer);
        setNonBlocking(reader);
        setTcpNoDelay(writer);
        setTcpNoDelay(reader);

        return {reader, writer};
    } catch (...) {
        closeNativeSocket(tempListener);
        closeNativeSocket(writer);
        closeNativeSocket(reader);
        throw;
    }
}

} // namespace

// ============================================================
// WakeupState
// ============================================================

void HttpAsyncServer::WakeupState::trigger() noexcept {
    if (pending.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex);
    if (closed || writeSocket == InvalidSocket) {
        return;
    }

    const char byte = 1;
    (void)sendNonBlocking(
        static_cast<SocketHandle>(writeSocket),
        &byte,
        1
    );
}

void HttpAsyncServer::WakeupState::close() noexcept {
    std::lock_guard<std::mutex> lock(mutex);
    closed = true;
    const auto sock = static_cast<SocketHandle>(writeSocket);
    writeSocket = InvalidSocket;
    closeNativeSocket(sock);
}

// ============================================================
// Construction / Destruction
// ============================================================

HttpAsyncServer::HttpAsyncServer(
    HttpAsyncServerCallbacks callbacks,
    HttpAsyncServerConfig config
)
    : callbacks_(std::move(callbacks)),
      config_(std::move(config)),
      wakeupState_(std::make_shared<WakeupState>()) {
    if (!callbacks_.onAccept) {
        throw HttpAsyncServerError(
            "Async HTTP server requires an onAccept callback"
        );
    }

    validateConfig();
}

HttpAsyncServer::HttpAsyncServer(
    std::shared_ptr<const HttpRouter> router,
    HttpAsyncServerConfig config
)
    : config_(std::move(config)),
      wakeupState_(std::make_shared<WakeupState>()) {
    if (!router) {
        throw HttpAsyncServerError(
            "Async HTTP server requires a router"
        );
    }

    callbacks_.onAccept =
        [router = std::move(router)](
            std::weak_ptr<HttpAsyncConnection> connection
        ) -> HttpAsyncConnection::DispatchCallback {
        return [router, connection = std::move(connection)](
                   const HttpCompletionToken& token,
                   HttpRequest request
               ) {
            auto target = connection.lock();
            if (!target || target->closed()) {
                return;
            }

            HttpResponse response;
            try {
                response = router->dispatch(request);
            } catch (...) {
                response = HttpResponse(
                    HttpStatus::InternalServerError,
                    request.version()
                );
                response.setBody("Internal Server Error");
            }
            (void)target->complete(token, std::move(response));
        };
    };

    validateConfig();
}

HttpAsyncServer::~HttpAsyncServer() noexcept {
    try {
        shutdown();
    } catch (...) {
        std::terminate();
    }
}

void HttpAsyncServer::validateConfig() const {
    if (config_.host.empty()) {
        throw HttpAsyncServerError(
            "HTTP server host cannot be empty"
        );
    }

    if (config_.listenBacklog <= 0) {
        throw HttpAsyncServerError(
            "HTTP server backlog must be positive"
        );
    }

    if (config_.maxConnections == 0) {
        throw HttpAsyncServerError(
            "Maximum connections must be positive"
        );
    }

    if (config_.readBufferSize == 0 ||
        config_.maxClientReadBufferBytes == 0) {
        throw HttpAsyncServerError(
            "Read buffer sizes must be positive"
        );
    }

    if (config_.idleTimeout.count() <= 0 ||
        config_.shutdownTimeout.count() <= 0 ||
        config_.pollTimeout.count() <= 0) {
        throw HttpAsyncServerError(
            "HTTP async server timeouts must be positive"
        );
    }

    if (config_.connection.maxInflightRequests == 0 ||
        config_.connection.maxPendingWriteBytes == 0 ||
        config_.connection.maxRetainedResponseBytes == 0) {
        throw HttpAsyncServerError(
            "HTTP async connection limits must be positive"
        );
    }
}

// ============================================================
// Socket Setup / Teardown
// ============================================================

void HttpAsyncServer::openSockets() {
    initializeSockets();

    SocketHandle listener = kInvalidSocket;
    SocketHandle wakeReader = kInvalidSocket;
    SocketHandle wakeWriter = kInvalidSocket;

    try {
        auto wakePair = createLoopbackWakeupPair();
        wakeReader = wakePair.first;
        wakeWriter = wakePair.second;

        listener =
            ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener == kInvalidSocket) {
            throw HttpAsyncServerError(
                "Failed to create listener socket"
            );
        }

        int reuse = 1;
#ifdef _WIN32
        if (::setsockopt(
                listener,
                SOL_SOCKET,
                SO_EXCLUSIVEADDRUSE,
                reinterpret_cast<const char*>(&reuse),
                sizeof(reuse)
            ) != 0) {
            throw HttpAsyncServerError(
                "Failed to configure SO_EXCLUSIVEADDRUSE"
            );
        }
#else
        if (::setsockopt(
                listener,
                SOL_SOCKET,
                SO_REUSEADDR,
                &reuse,
                sizeof(reuse)
            ) != 0) {
            throw HttpAsyncServerError(
                "Failed to configure SO_REUSEADDR"
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
            throw HttpAsyncServerError(
                "Invalid IPv4 HTTP server host"
            );
        }

        if (::bind(
                listener,
                reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)
            ) != 0) {
            throw HttpAsyncServerError(
                "Failed to bind async HTTP listener"
            );
        }

        if (::listen(listener, config_.listenBacklog) != 0) {
            throw HttpAsyncServerError(
                "Failed to listen on async HTTP socket"
            );
        }

        setNonBlocking(listener);

        sockaddr_in boundAddress{};
#ifdef _WIN32
        int addrLen = sizeof(boundAddress);
#else
        socklen_t addrLen = sizeof(boundAddress);
#endif
        if (::getsockname(
                listener,
                reinterpret_cast<sockaddr*>(&boundAddress),
                &addrLen
            ) != 0) {
            throw HttpAsyncServerError(
                "Failed to query bound port"
            );
        }

        boundPort_.store(
            ntohs(boundAddress.sin_port),
            std::memory_order_release
        );

        listenerSocket_ = static_cast<NativeSocket>(listener);
        wakeReadSocket_ = static_cast<NativeSocket>(wakeReader);

        {
            std::lock_guard<std::mutex> lock(
                wakeupState_->mutex
            );
            wakeupState_->writeSocket =
                static_cast<NativeSocket>(wakeWriter);
            wakeupState_->closed = false;
            wakeupState_->pending.store(
                false,
                std::memory_order_release
            );
        }
    } catch (...) {
        closeNativeSocket(listener);
        closeNativeSocket(wakeReader);
        closeNativeSocket(wakeWriter);
        boundPort_.store(0, std::memory_order_release);
        cleanupSockets();
        throw;
    }
}

void HttpAsyncServer::closeSockets() noexcept {
    wakeupState_->close();

    const auto listener =
        static_cast<SocketHandle>(listenerSocket_);
    listenerSocket_ = InvalidSocket;
    closeNativeSocket(listener);

    const auto wakeReader =
        static_cast<SocketHandle>(wakeReadSocket_);
    wakeReadSocket_ = InvalidSocket;
    closeNativeSocket(wakeReader);

    boundPort_.store(0, std::memory_order_release);
    cleanupSockets();
}

// ============================================================
// Lifecycle
// ============================================================

void HttpAsyncServer::start() {
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        if (state_ != HttpAsyncServerState::Created) {
            throw HttpAsyncServerError(
                "Async HTTP server can only start from Created"
            );
        }
        state_ = HttpAsyncServerState::Starting;
    }

    shutdownRequested_.store(
        false,
        std::memory_order_release
    );

    try {
        openSockets();

        reactorThread_ = std::thread([this]() noexcept {
            reactorLoop();
        });

        setState(HttpAsyncServerState::Running);
    } catch (...) {
        shutdownRequested_.store(
            true,
            std::memory_order_release
        );
        wakeup();

        if (reactorThread_.joinable()) {
            reactorThread_.join();
        }

        if (listenerSocket_ != InvalidSocket ||
            wakeReadSocket_ != InvalidSocket) {
            closeSockets();
        }

        setState(HttpAsyncServerState::Failed);
        throw;
    }
}

void HttpAsyncServer::shutdown() {
    {
        std::unique_lock<std::mutex> lock(lifecycleMutex_);

        lifecycleCondition_.wait(lock, [this]() {
            return state_ != HttpAsyncServerState::Starting &&
                   state_ != HttpAsyncServerState::Stopping;
        });

        if (state_ == HttpAsyncServerState::Stopped) {
            return;
        }

        if (state_ == HttpAsyncServerState::Created ||
            state_ == HttpAsyncServerState::Failed) {
            state_ = HttpAsyncServerState::Stopped;
            lock.unlock();
            lifecycleCondition_.notify_all();
            return;
        }

        state_ = HttpAsyncServerState::Stopping;
    }

    shutdownRequested_.store(
        true,
        std::memory_order_release
    );
    wakeup();

    if (reactorThread_.joinable()) {
        if (reactorThread_.get_id() ==
            std::this_thread::get_id()) {
            std::terminate();
        }
        reactorThread_.join();
    }

    closeSockets();
    setState(HttpAsyncServerState::Stopped);
}

void HttpAsyncServer::wakeup() noexcept {
    if (wakeupState_) {
        wakeupState_->trigger();
    }
}

std::function<void()>
HttpAsyncServer::wakeupCallback() const noexcept {
    std::weak_ptr<WakeupState> weak = wakeupState_;
    return [weak = std::move(weak)]() noexcept {
        if (auto state = weak.lock()) {
            state->trigger();
        }
    };
}

// ============================================================
// Reactor Loop (Event-Loop Owner Thread)
// ============================================================

void HttpAsyncServer::reactorLoop() noexcept {
    using Clock = std::chrono::steady_clock;

    struct ClientEntry final {
        std::uint64_t id = 0;
        std::uint64_t generation = 0;
        SocketHandle socket = kInvalidSocket;
        std::shared_ptr<HttpAsyncConnection> connection;
        std::string readBuffer;
        bool peerEof = false;
        bool hardError = false;
        Clock::time_point lastActivity = Clock::now();
    };

    std::unordered_map<std::uint64_t, ClientEntry> clients;
    std::vector<char> scratch(config_.readBufferSize);

    auto listener =
        static_cast<SocketHandle>(listenerSocket_);
    const auto wakeReader =
        static_cast<SocketHandle>(wakeReadSocket_);

    bool drainingForShutdown = false;
    Clock::time_point shutdownDeadline{};

    auto flushClientWrites = [&](ClientEntry& client) noexcept {
        while (!client.hardError &&
               client.connection->hasPendingOutput()) {
            const auto view =
                client.connection->pendingOutput();
            if (view.empty()) {
                break;
            }

            const int written = sendNonBlocking(
                client.socket,
                view.data(),
                view.size()
            );

            if (written > 0) {
                client.connection->consumeOutput(
                    static_cast<std::size_t>(written)
                );
                client.lastActivity = Clock::now();
                continue;
            }

            if (written == 0) {
                client.hardError = true;
                break;
            }

            const int err = socketError();
            if (isInterrupted(err)) {
                continue;
            }
            if (isWouldBlock(err)) {
                break;
            }

            client.hardError = true;
            break;
        }
    };

    auto feedBufferedInput = [&](ClientEntry& client) noexcept {
        while (!client.hardError &&
               !client.readBuffer.empty() &&
               client.connection->canReceive()) {
            const auto res = client.connection->receive(
                client.readBuffer
            );

            dispatchedRequests_.fetch_add(
                res.dispatched,
                std::memory_order_relaxed
            );

            if (res.consumed > 0) {
                client.readBuffer.erase(0, res.consumed);
                client.lastActivity = Clock::now();
            }

            if (res.failed() || res.closed) {
                client.readBuffer.clear();
                break;
            }

            if (res.backpressured || res.consumed == 0) {
                break;
            }
        }

        if (client.peerEof && client.readBuffer.empty()) {
            client.connection->stopAccepting();
        }
    };

    auto closeAndEraseClient = [&](ClientEntry& client) noexcept {
        if (callbacks_.onConnectionClosed) {
            try {
                callbacks_.onConnectionClosed(
                    client.id,
                    client.generation
                );
            } catch (...) {
            }
        }

        if (!client.connection->closed()) {
            client.connection->abort();
        }

        closeNativeSocket(client.socket);
        client.socket = kInvalidSocket;

        activeConnections_.fetch_sub(
            1,
            std::memory_order_relaxed
        );
        completedConnections_.fetch_add(
            1,
            std::memory_order_relaxed
        );
    };

    while (true) {
        if (callbacks_.onTick) {
            try {
                callbacks_.onTick();
            } catch (...) {
            }
        }

        const auto now = Clock::now();

        if (shutdownRequested_.load(std::memory_order_acquire) &&
            !drainingForShutdown) {
            drainingForShutdown = true;
            shutdownDeadline = now + config_.shutdownTimeout;

            if (listener != kInvalidSocket) {
                closeNativeSocket(listener);
                listener = kInvalidSocket;
                listenerSocket_ = InvalidSocket;
            }

            for (auto& [id, client] : clients) {
                (void)id;
                client.connection->stopAccepting();
            }
        }

        for (auto it = clients.begin(); it != clients.end();) {
            auto& client = it->second;

            feedBufferedInput(client);
            flushClientWrites(client);

            const bool idleExpired =
                client.connection->inflight() == 0 &&
                !client.connection->hasPendingOutput() &&
                client.readBuffer.empty() &&
                (now - client.lastActivity >= config_.idleTimeout);

            const bool eofDrained =
                client.peerEof &&
                client.readBuffer.empty() &&
                client.connection->inflight() == 0 &&
                !client.connection->hasPendingOutput();

            if (client.hardError ||
                client.connection->shouldClose() ||
                eofDrained ||
                idleExpired) {
                closeAndEraseClient(client);
                it = clients.erase(it);
            } else {
                ++it;
            }
        }

        if (drainingForShutdown) {
            if (clients.empty()) {
                break;
            }

            if (Clock::now() >= shutdownDeadline) {
                for (auto& [id, client] : clients) {
                    (void)id;
                    closeAndEraseClient(client);
                }
                clients.clear();
                break;
            }
        }

        // Build pollfd table
        std::vector<PollFd> pollfds;
        std::vector<std::uint64_t> pollClientIds;
        pollfds.reserve(2 + clients.size());
        pollClientIds.reserve(clients.size());

        PollFd wakePoll{};
        wakePoll.fd = wakeReader;
        wakePoll.events = kPollRead;
        pollfds.push_back(wakePoll);

        std::size_t listenerIndex =
            std::numeric_limits<std::size_t>::max();
        if (listener != kInvalidSocket && !drainingForShutdown) {
            PollFd listenPoll{};
            listenPoll.fd = listener;
            listenPoll.events = kPollRead;
            listenerIndex = pollfds.size();
            pollfds.push_back(listenPoll);
        }

        const std::size_t clientStartIndex = pollfds.size();
        for (const auto& [id, client] : clients) {
            short events = 0;

            if (!client.peerEof &&
                client.connection->canReceive() &&
                client.readBuffer.size() <
                    config_.maxClientReadBufferBytes) {
                events |= kPollRead;
            }

            if (client.connection->hasPendingOutput()) {
                events |= kPollWrite;
            }

            if (events == 0) {
                continue;
            }

            PollFd clientPoll{};
            clientPoll.fd = client.socket;
            clientPoll.events = events;
            pollfds.push_back(clientPoll);
            pollClientIds.push_back(id);
        }

        const auto effectivePoll =
            std::min(config_.pollTimeout, config_.idleTimeout);

        int timeoutMs = static_cast<int>(
            std::clamp<std::int64_t>(
                effectivePoll.count(),
                1,
                60000
            )
        );

        if (drainingForShutdown) {
            const auto remaining =
                std::chrono::duration_cast<
                    std::chrono::milliseconds
                >(shutdownDeadline - Clock::now());
            timeoutMs = static_cast<int>(
                std::clamp<std::int64_t>(
                    remaining.count(),
                    1,
                    timeoutMs
                )
            );
        }

        const int ready = pollSockets(
            pollfds.data(),
            pollfds.size(),
            timeoutMs
        );

        if (ready < 0) {
            if (isInterrupted(socketError())) {
                continue;
            }
            break;
        }

        if (ready == 0) {
            continue;
        }

        // 1. Drain wakeup socket
        if ((pollfds[0].revents & kPollRead) != 0) {
            std::array<char, 256> drainBuf{};
            while (true) {
                const int n = recvNonBlocking(
                    wakeReader,
                    drainBuf.data(),
                    drainBuf.size()
                );
                if (n > 0) {
                    continue;
                }
                break;
            }
            wakeupState_->pending.store(
                false,
                std::memory_order_release
            );
            // Second pass in case a wakeup byte landed right
            // before clearing pending.
            while (true) {
                const int n = recvNonBlocking(
                    wakeReader,
                    drainBuf.data(),
                    drainBuf.size()
                );
                if (n > 0) {
                    continue;
                }
                break;
            }
            wakeups_.fetch_add(1, std::memory_order_relaxed);
        }

        // 2. Accept incoming connections
        if (listenerIndex !=
                std::numeric_limits<std::size_t>::max() &&
            (pollfds[listenerIndex].revents & kPollRead) != 0) {
            while (!shutdownRequested_.load(
                std::memory_order_acquire
            )) {
                sockaddr_in peerAddr{};
#ifdef _WIN32
                int peerLen = sizeof(peerAddr);
#else
                socklen_t peerLen = sizeof(peerAddr);
#endif
                const SocketHandle clientSock = ::accept(
                    listener,
                    reinterpret_cast<sockaddr*>(&peerAddr),
                    &peerLen
                );

                if (clientSock == kInvalidSocket) {
                    const int err = socketError();
                    if (isInterrupted(err)) {
                        continue;
                    }
                    break;
                }

                if (clients.size() >= config_.maxConnections) {
                    rejectedConnections_.fetch_add(
                        1,
                        std::memory_order_relaxed
                    );
                    closeNativeSocket(clientSock);
                    continue;
                }

                try {
                    setNonBlocking(clientSock);
                    setTcpNoDelay(clientSock);

                    const std::uint64_t connId =
                        nextConnectionId_.fetch_add(
                            1,
                            std::memory_order_relaxed
                        );

                    // Indirect dispatcher via a small shared holder
                    // so onAccept receives weak_ptr<HttpAsyncConnection>
                    // cleanly at construction time.
                    auto dispatcherSlot = std::make_shared<
                        HttpAsyncConnection::DispatchCallback
                    >();

                    auto conn =
                        std::make_shared<HttpAsyncConnection>(
                            connId,
                            [dispatcherSlot](
                                const HttpCompletionToken& token,
                                HttpRequest request
                            ) {
                                if (*dispatcherSlot) {
                                    (*dispatcherSlot)(
                                        token,
                                        std::move(request)
                                    );
                                }
                            },
                            config_.connection
                        );

                    *dispatcherSlot = callbacks_.onAccept(conn);
                    if (!*dispatcherSlot) {
                        throw HttpAsyncServerError(
                            "onAccept returned empty dispatcher"
                        );
                    }

                    ClientEntry entry;
                    entry.id = conn->connectionId();
                    entry.generation = conn->generation();
                    entry.socket = clientSock;
                    entry.connection = std::move(conn);
                    entry.lastActivity = Clock::now();

                    clients.emplace(entry.id, std::move(entry));

                    acceptedConnections_.fetch_add(
                        1,
                        std::memory_order_relaxed
                    );
                    activeConnections_.fetch_add(
                        1,
                        std::memory_order_relaxed
                    );
                } catch (...) {
                    rejectedConnections_.fetch_add(
                        1,
                        std::memory_order_relaxed
                    );
                    closeNativeSocket(clientSock);
                }
            }
        }

        // 3. Process ready client sockets
        for (std::size_t i = 0; i < pollClientIds.size(); ++i) {
            const auto& pfd = pollfds[clientStartIndex + i];
            if (pfd.revents == 0) {
                continue;
            }

            const auto clientIt = clients.find(pollClientIds[i]);
            if (clientIt == clients.end()) {
                continue;
            }

            auto& client = clientIt->second;

            if ((pfd.revents & (POLLERR | POLLNVAL)) != 0) {
                client.hardError = true;
                continue;
            }

            if ((pfd.revents & (kPollRead | POLLHUP)) != 0) {
                while (client.readBuffer.size() <
                       config_.maxClientReadBufferBytes) {
                    const std::size_t remainingCap =
                        config_.maxClientReadBufferBytes -
                        client.readBuffer.size();
                    const std::size_t toRead = std::min(
                        scratch.size(),
                        remainingCap
                    );

                    const int n = recvNonBlocking(
                        client.socket,
                        scratch.data(),
                        toRead
                    );

                    if (n > 0) {
                        client.readBuffer.append(
                            scratch.data(),
                            static_cast<std::size_t>(n)
                        );
                        client.lastActivity = Clock::now();
                        continue;
                    }

                    if (n == 0) {
                        client.peerEof = true;
                        break;
                    }

                    const int err = socketError();
                    if (isInterrupted(err)) {
                        continue;
                    }
                    if (isWouldBlock(err)) {
                        break;
                    }

                    client.hardError = true;
                    break;
                }

                feedBufferedInput(client);
            }

            if (!client.hardError &&
                (pfd.revents & kPollWrite) != 0) {
                flushClientWrites(client);
            }
        }
    }

    // Ensure all clients are cleaned up before exit
    for (auto& [id, client] : clients) {
        (void)id;
        closeAndEraseClient(client);
    }
    clients.clear();
}

// ============================================================
// State & Inspection
// ============================================================

void HttpAsyncServer::setState(
    HttpAsyncServerState state
) noexcept {
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        state_ = state;
    }
    lifecycleCondition_.notify_all();
}

HttpAsyncServerState HttpAsyncServer::state() const noexcept {
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    return state_;
}

bool HttpAsyncServer::isRunning() const noexcept {
    return state() == HttpAsyncServerState::Running;
}

bool HttpAsyncServer::isStopping() const noexcept {
    return state() == HttpAsyncServerState::Stopping;
}

bool HttpAsyncServer::isStopped() const noexcept {
    return state() == HttpAsyncServerState::Stopped;
}

bool HttpAsyncServer::hasFailed() const noexcept {
    return state() == HttpAsyncServerState::Failed;
}

std::string HttpAsyncServer::host() const {
    return config_.host;
}

std::uint16_t HttpAsyncServer::port() const noexcept {
    return boundPort_.load(std::memory_order_acquire);
}

HttpAsyncServerConfig HttpAsyncServer::config() const {
    return config_;
}

HttpAsyncServerStats HttpAsyncServer::stats() const noexcept {
    HttpAsyncServerStats result;
    result.acceptedConnections =
        acceptedConnections_.load(std::memory_order_relaxed);
    result.rejectedConnections =
        rejectedConnections_.load(std::memory_order_relaxed);
    result.completedConnections =
        completedConnections_.load(std::memory_order_relaxed);
    result.dispatchedRequests =
        dispatchedRequests_.load(std::memory_order_relaxed);
    result.wakeups =
        wakeups_.load(std::memory_order_relaxed);
    result.activeConnections =
        activeConnections_.load(std::memory_order_relaxed);
    return result;
}

std::size_t
HttpAsyncServer::activeConnectionCount() const noexcept {
    return activeConnections_.load(std::memory_order_relaxed);
}

} // namespace lamp::http
