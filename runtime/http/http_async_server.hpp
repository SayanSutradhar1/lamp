
#pragma once

#include "http/http_async_connection.hpp"
#include "http/http_router.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace lamp::http {

// ============================================================
// Async HTTP Server Error
// ============================================================

class HttpAsyncServerError final : public std::runtime_error {
public:
    explicit HttpAsyncServerError(const std::string& message)
        : std::runtime_error(message) {}
};

// ============================================================
// Async HTTP Server Configuration
// ============================================================

struct HttpAsyncServerConfig final {
    std::string host = "127.0.0.1";
    std::uint16_t port = 3000;

    int listenBacklog = 128;
    std::size_t maxConnections = 1024;
    std::size_t readBufferSize = 16 * 1024;
    std::size_t maxClientReadBufferBytes = 256 * 1024;

    std::chrono::milliseconds idleTimeout{30000};
    std::chrono::milliseconds shutdownTimeout{5000};
    std::chrono::milliseconds pollTimeout{20};

    HttpAsyncConnectionConfig connection{};
};

// ============================================================
// Async HTTP Server State & Stats
// ============================================================

enum class HttpAsyncServerState : std::uint8_t {
    Created,
    Starting,
    Running,
    Stopping,
    Stopped,
    Failed
};

struct HttpAsyncServerStats final {
    std::uint64_t acceptedConnections = 0;
    std::uint64_t rejectedConnections = 0;
    std::uint64_t completedConnections = 0;
    std::uint64_t dispatchedRequests = 0;
    std::uint64_t wakeups = 0;

    std::size_t activeConnections = 0;
};

// ============================================================
// Event-Loop Owner Callbacks
//
// All callbacks execute exclusively on the server's reactor
// owner thread. They must never block.
// ============================================================

struct HttpAsyncServerCallbacks final {
    using RequestDispatcherFactory = std::function<
        HttpAsyncConnection::DispatchCallback(
            std::weak_ptr<HttpAsyncConnection>
        )
    >;

    using EventLoopTick = std::function<void()>;

    using ConnectionClosedCallback = std::function<void(
        std::uint64_t connectionId,
        std::uint64_t generation
    )>;

    RequestDispatcherFactory onAccept;
    EventLoopTick onTick;
    ConnectionClosedCallback onConnectionClosed;
};

// ============================================================
// Nonblocking Async HTTP Server (Phase 6C)
//
// Owns:
//   - Nonblocking TCP listener socket
//   - Wakeup socket pair for cross-thread reactor notification
//   - Single event-loop owner thread (reactorThread_)
//   - Connection registry keyed by connectionId + generation
//   - HttpAsyncConnection protocol instances (owner-thread only)
// ============================================================

class HttpAsyncServer final {
public:
    explicit HttpAsyncServer(
        HttpAsyncServerCallbacks callbacks,
        HttpAsyncServerConfig config = {}
    );

    explicit HttpAsyncServer(
        std::shared_ptr<const HttpRouter> router,
        HttpAsyncServerConfig config = {}
    );

    ~HttpAsyncServer() noexcept;

    HttpAsyncServer(const HttpAsyncServer&) = delete;
    HttpAsyncServer& operator=(const HttpAsyncServer&) = delete;

    HttpAsyncServer(HttpAsyncServer&&) = delete;
    HttpAsyncServer& operator=(HttpAsyncServer&&) = delete;

    // ========================================================
    // Lifecycle
    // ========================================================

    void start();
    void shutdown();

    // Thread-safe coalesced wakeup of the reactor thread.
    void wakeup() noexcept;

    // Returns a lifetime-safe wakeup callback suitable for
    // AsyncOperationCoordinator::SetWakeupCallback().
    [[nodiscard]]
    std::function<void()> wakeupCallback() const noexcept;

    // ========================================================
    // Inspection
    // ========================================================

    [[nodiscard]]
    HttpAsyncServerState state() const noexcept;

    [[nodiscard]]
    bool isRunning() const noexcept;

    [[nodiscard]]
    bool isStopping() const noexcept;

    [[nodiscard]]
    bool isStopped() const noexcept;

    [[nodiscard]]
    bool hasFailed() const noexcept;

    [[nodiscard]]
    std::string host() const;

    [[nodiscard]]
    std::uint16_t port() const noexcept;

    [[nodiscard]]
    HttpAsyncServerConfig config() const;

    [[nodiscard]]
    HttpAsyncServerStats stats() const noexcept;

    [[nodiscard]]
    std::size_t activeConnectionCount() const noexcept;

private:
    using NativeSocket = std::intptr_t;

    static constexpr NativeSocket InvalidSocket =
        static_cast<NativeSocket>(-1);

    struct WakeupState final {
        mutable std::mutex mutex;
        NativeSocket writeSocket = InvalidSocket;
        std::atomic<bool> pending{false};
        bool closed = false;

        void trigger() noexcept;
        void close() noexcept;
    };

    void validateConfig() const;
    void openSockets();
    void closeSockets() noexcept;
    void reactorLoop() noexcept;

    void setState(HttpAsyncServerState state) noexcept;

    HttpAsyncServerCallbacks callbacks_;
    HttpAsyncServerConfig config_;

    mutable std::mutex lifecycleMutex_;
    std::condition_variable lifecycleCondition_;
    HttpAsyncServerState state_ =
        HttpAsyncServerState::Created;

    NativeSocket listenerSocket_ = InvalidSocket;
    NativeSocket wakeReadSocket_ = InvalidSocket;
    std::shared_ptr<WakeupState> wakeupState_;

    std::thread reactorThread_;
    std::atomic<bool> shutdownRequested_{false};
    std::atomic<std::uint16_t> boundPort_{0};
    std::atomic<std::uint64_t> nextConnectionId_{1};

    std::atomic<std::uint64_t> acceptedConnections_{0};
    std::atomic<std::uint64_t> rejectedConnections_{0};
    std::atomic<std::uint64_t> completedConnections_{0};
    std::atomic<std::uint64_t> dispatchedRequests_{0};
    std::atomic<std::uint64_t> wakeups_{0};
    std::atomic<std::size_t> activeConnections_{0};
};

} // namespace lamp::http
