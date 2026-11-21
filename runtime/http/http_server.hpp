
#pragma once

#include "http/http_connection.hpp"
#include "http/http_router.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace lamp::http {

// ============================================================
// HTTP Server Error
// ============================================================

class HttpServerError final : public std::runtime_error {
public:
    explicit HttpServerError(const std::string& message)
        : std::runtime_error(message) {}
};

// ============================================================
// HTTP Server Configuration
// ============================================================
//
// The server is designed for HTTP/1.0 and HTTP/1.1.
//
// Initial transport implementation:
//
// - TCP over IPv4
// - Blocking accept loop
// - One connection worker per accepted client
// - Configurable connection limits
//
// A future event-driven transport can replace the underlying
// implementation without changing the public API.
//
// ============================================================

struct HttpServerConfig final {
    // Network interface to bind.
    //
    // "127.0.0.1" = localhost only
    // "0.0.0.0"   = all IPv4 interfaces
    std::string host = "127.0.0.1";

    // TCP listening port.
    //
    // Port 0 requests an ephemeral OS-assigned port.
    std::uint16_t port = 3000;

    // Maximum pending TCP connections.
    int listenBacklog = 128;

    // Maximum simultaneous client connections.
    std::size_t maxConnections = 1024;

    // Maximum bytes read per socket operation.
    std::size_t readBufferSize = 16 * 1024;

    // Socket receive timeout.
    std::chrono::milliseconds readTimeout{30000};

    // Socket send timeout.
    std::chrono::milliseconds writeTimeout{30000};

    // Maximum time allowed for graceful shutdown.
    std::chrono::milliseconds shutdownTimeout{5000};

    // HTTP protocol configuration.
    HttpConnectionConfig connection{};
};

// ============================================================
// HTTP Server State
// ============================================================

enum class HttpServerState : std::uint8_t {
    Created,
    Starting,
    Running,
    Stopping,
    Stopped,
    Failed
};

// ============================================================
// HTTP Server Statistics
// ============================================================

struct HttpServerStats final {
    std::uint64_t acceptedConnections = 0;
    std::uint64_t rejectedConnections = 0;
    std::uint64_t completedConnections = 0;
    std::uint64_t completedRequests = 0;

    std::size_t activeConnections = 0;
};

// ============================================================
// HTTP Server
// ============================================================
//
// Ownership:
//
// - Owns its listener socket
// - Owns accepted client connections
// - Owns connection worker threads
// - Shares ownership of HttpRouter
//
// Lifecycle:
//
// Created
//    |
//    v
// Starting
//    |
//    v
// Running
//    |
//    v
// Stopping
//    |
//    v
// Stopped
//
// Startup failures transition to Failed.
//
// Thread safety:
//
// - Lifecycle methods are synchronized.
// - Route registration is thread-safe through HttpRouter.
// - Statistics use atomic counters.
// - Worker ownership is protected by a mutex.
//
// ============================================================

class HttpServer final {
public:
    explicit HttpServer(
        HttpServerConfig config = {}
    );

    ~HttpServer() noexcept;

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    HttpServer(HttpServer&&) = delete;
    HttpServer& operator=(HttpServer&&) = delete;

    // ========================================================
    // Lifecycle
    // ========================================================

    // Binds the listener and starts accepting connections.
    //
    // Throws HttpServerError on startup failure.
    //
    // The method returns once the listener is ready.

    void start();

    // Requests graceful shutdown.
    //
    // Stops accepting new clients, then allows existing
    // connections to finish within shutdownTimeout.
    //
    // May be called concurrently from multiple threads.
    //
    // Implementations must not join the calling thread.

    void shutdown();

    // ========================================================
    // State Inspection
    // ========================================================

    [[nodiscard]]
    HttpServerState state() const noexcept;

    [[nodiscard]]
    bool isRunning() const noexcept;

    [[nodiscard]]
    bool isStopping() const noexcept;

    [[nodiscard]]
    bool isStopped() const noexcept;

    [[nodiscard]]
    bool hasFailed() const noexcept;

    // ========================================================
    // Network Information
    // ========================================================

    [[nodiscard]]
    std::string host() const;

    // Returns the bound TCP port when running.
    //
    // This may differ from config.port when port 0 was used.

    [[nodiscard]]
    std::uint16_t port() const noexcept;

    [[nodiscard]]
    HttpServerConfig config() const;

    // ========================================================
    // Router Access
    // ========================================================

    [[nodiscard]]
    std::shared_ptr<HttpRouter> router() const noexcept;

    // ========================================================
    // Route Registration
    // ========================================================

    void route(
        HttpMethod method,
        std::string pattern,
        HttpRouteHandler handler
    );

    void get(
        std::string pattern,
        HttpRouteHandler handler
    );

    void post(
        std::string pattern,
        HttpRouteHandler handler
    );

    void put(
        std::string pattern,
        HttpRouteHandler handler
    );

    void patch(
        std::string pattern,
        HttpRouteHandler handler
    );

    void del(
        std::string pattern,
        HttpRouteHandler handler
    );

    void head(
        std::string pattern,
        HttpRouteHandler handler
    );

    void options(
        std::string pattern,
        HttpRouteHandler handler
    );

    // ========================================================
    // Statistics
    // ========================================================

    [[nodiscard]]
    HttpServerStats stats() const noexcept;

    [[nodiscard]]
    std::size_t activeConnectionCount() const noexcept;

private:
    // ========================================================
    // Native Socket Representation
    // ========================================================
    //
    // intptr_t can represent the supported native socket
    // handle values without exposing platform headers here.
    //
    // Windows SOCKET and POSIX file descriptors are converted
    // by the platform-specific implementation.
    //
    // ========================================================

    using NativeSocket = std::intptr_t;

    static constexpr NativeSocket InvalidSocket =
        static_cast<NativeSocket>(-1);

    // ========================================================
    // Client Connection Record
    // ========================================================

    struct ClientConnection final {
        std::uint64_t id = 0;

        NativeSocket socket = InvalidSocket;

        std::shared_ptr<HttpConnection> protocol;

        std::thread worker;

        std::atomic<bool> finished{false};

        ClientConnection() = default;

        ClientConnection(const ClientConnection&) = delete;
        ClientConnection& operator=(const ClientConnection&) =
            delete;
    };

    // ========================================================
    // Configuration Validation
    // ========================================================

    void validateConfig() const;

    // ========================================================
    // Listener Management
    // ========================================================

    void openListener();

    void closeListener() noexcept;

    // ========================================================
    // Accept Loop
    // ========================================================

    void acceptLoop() noexcept;

    void acceptClient(NativeSocket clientSocket);

    // ========================================================
    // Client Processing
    // ========================================================

    void clientLoop(
        const std::shared_ptr<ClientConnection>& client
    ) noexcept;

    // ========================================================
    // Worker Management
    // ========================================================

    void reapFinishedConnections();

    void stopActiveConnections() noexcept;

    void joinConnectionWorkers() noexcept;

    // ========================================================
    // Socket Helpers
    // ========================================================

    static void closeSocket(
        NativeSocket socket
    ) noexcept;

    static void shutdownSocket(
        NativeSocket socket
    ) noexcept;

    // ========================================================
    // Lifecycle Helpers
    // ========================================================

    void setState(
        HttpServerState state
    ) noexcept;

    [[nodiscard]]
    bool isShutdownRequested() const noexcept;

    // ========================================================
    // Configuration
    // ========================================================

    HttpServerConfig config_;

    // ========================================================
    // Router
    // ========================================================

    std::shared_ptr<HttpRouter> router_;

    // ========================================================
    // Lifecycle Synchronization
    // ========================================================

    mutable std::mutex lifecycleMutex_;
    std::condition_variable lifecycleCondition_;

    HttpServerState state_ =
        HttpServerState::Created;

    // ========================================================
    // Listener
    // ========================================================

    NativeSocket listenerSocket_ = InvalidSocket;

    std::thread acceptThread_;

    std::atomic<bool> shutdownRequested_{false};

    std::atomic<std::uint16_t> boundPort_{0};

    // ========================================================
    // Connection Ownership
    // ========================================================

    mutable std::mutex connectionsMutex_;

    std::unordered_map<
        std::uint64_t,
        std::shared_ptr<ClientConnection>
    > connections_;

    std::atomic<std::uint64_t> nextConnectionId_{1};

    // ========================================================
    // Statistics
    // ========================================================

    std::atomic<std::uint64_t> acceptedConnections_{0};
    std::atomic<std::uint64_t> rejectedConnections_{0};
    std::atomic<std::uint64_t> completedConnections_{0};
    std::atomic<std::uint64_t> completedRequests_{0};

    std::atomic<std::size_t> activeConnections_{0};
};

} // namespace lamp::http
