#pragma once

// ============================================================
// Lamp Framework
// Native Binary Transport Layer (`LampBinaryServer`)
//
// File: runtime/transport/lamp_binary_server.hpp
//
// Purpose:
//   High-speed nonblocking persistent multiplexed TCP server
//   supporting raw LMP1 binary frames (Node / C++ clients) and
//   RFC 6455 binary WebSocket upgrade (browser clients) with
//   zero REST routing, integrated FullCycleWireCache, and
//   Core DynamicWorkerPool parallel dispatch.
// ============================================================

#include "core/dynamic_worker_pool.hpp"
#include "security/security_context.hpp"
#include "transport/lamp_binary_protocol.hpp"
#include "transport/wire_cache.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
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

namespace lamp::transport {

struct BinaryRpcDispatchResult final {
    bool ok = true;
    std::vector<std::uint8_t> encoded_lcb_response;
    std::chrono::milliseconds cache_ttl{0}; // >0 enables full-cycle caching (clamped by WireCache)
};

using BinaryOperationHandler = std::function<
    BinaryRpcDispatchResult(const DecodedRpcInvocation&, std::uint64_t request_id)
>;

struct LampBinaryServerConfig final {
    std::string host = "127.0.0.1";
    std::uint16_t port = 3000;
    int listen_backlog = 256;
    std::size_t max_connections = 2048;
    std::size_t max_frame_bytes = 32U * 1024U * 1024U;
    FullCycleWireCacheConfig wire_cache{};
    core::DynamicWorkerPoolConfig worker_pool{};
};

class LampBinaryServer final {
public:
    LampBinaryServer(
        BinaryOperationHandler handler,
        LampBinaryServerConfig config = {},
        std::shared_ptr<core::DynamicWorkerPool> pool = {},
        std::shared_ptr<FullCycleWireCache> wire_cache = {}
    )
        : handler_(std::move(handler)),
          config_(std::move(config)),
          pool_(pool ? std::move(pool)
                     : std::make_shared<core::DynamicWorkerPool>(config_.worker_pool)),
          wire_cache_(wire_cache ? std::move(wire_cache)
                                 : std::make_shared<FullCycleWireCache>(config_.wire_cache)) {
        if (!handler_) {
            throw std::invalid_argument("LampBinaryServer requires an operation handler");
        }
    }

    ~LampBinaryServer() noexcept {
        Shutdown();
    }

    LampBinaryServer(const LampBinaryServer&) = delete;
    LampBinaryServer& operator=(const LampBinaryServer&) = delete;

    void Start() {
        bool expected = false;
        if (!started_.compare_exchange_strong(expected, true)) {
            return;
        }

#ifdef _WIN32
        WSADATA wsa{};
        if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            throw std::runtime_error("WSAStartup failed");
        }
        wsa_initialized_ = true;
#endif

        InitListener();
        InitWakeupPair();

        running_.store(true, std::memory_order_release);
        reactor_thread_ = std::thread([this]() { ReactorLoop(); });
    }

    void Shutdown() noexcept {
        bool expected = true;
        if (!running_.compare_exchange_strong(expected, false)) {
            if (!started_.load(std::memory_order_acquire)) {
                return;
            }
        }

        Wakeup();
        if (reactor_thread_.joinable()) {
            reactor_thread_.join();
        }

        {
            std::lock_guard lock(conn_mutex_);
            for (auto& [id, conn] : connections_) {
                (void)id;
                CloseNativeSocket(conn->sock);
                conn->sock = kInvalidSock;
            }
            connections_.clear();
        }

        CloseNativeSocket(listen_sock_);
        listen_sock_ = kInvalidSock;
        CloseNativeSocket(wakeup_read_);
        wakeup_read_ = kInvalidSock;
        CloseNativeSocket(wakeup_write_);
        wakeup_write_ = kInvalidSock;

#ifdef _WIN32
        if (wsa_initialized_) {
            ::WSACleanup();
            wsa_initialized_ = false;
        }
#endif
    }

    [[nodiscard]]
    bool IsRunning() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    [[nodiscard]]
    std::uint16_t Port() const noexcept {
        return bound_port_;
    }

    [[nodiscard]]
    std::string Host() const {
        return config_.host;
    }

    [[nodiscard]]
    std::shared_ptr<core::DynamicWorkerPool> WorkerPool() const noexcept {
        return pool_;
    }

    [[nodiscard]]
    std::shared_ptr<FullCycleWireCache> WireCache() const noexcept {
        return wire_cache_;
    }

private:
#ifdef _WIN32
    using Sock = SOCKET;
    using PollEntry = WSAPOLLFD;
    static constexpr Sock kInvalidSock = INVALID_SOCKET;
    static constexpr short kReadEvt = POLLRDNORM;
    static constexpr short kWriteEvt = POLLWRNORM;
#else
    using Sock = int;
    using PollEntry = struct pollfd;
    static constexpr Sock kInvalidSock = -1;
    static constexpr short kReadEvt = POLLIN;
    static constexpr short kWriteEvt = POLLOUT;
#endif

    enum class ConnProtocol : std::uint8_t {
        Detecting = 0,
        RawLmp1 = 1,
        WebSocketLmp1 = 2
    };

    struct Connection final {
        std::uint64_t id = 0;
        Sock sock = kInvalidSock;
        ConnProtocol protocol = ConnProtocol::Detecting;
        bool close_after_flush = false;

        std::vector<std::uint8_t> raw_in;
        LmpFrameDecoder lmp_decoder;

        std::mutex write_mutex;
        std::deque<std::vector<std::uint8_t>> write_queue;
        std::size_t write_offset = 0;

        bool HasPendingWrites() {
            std::lock_guard lock(write_mutex);
            return !write_queue.empty();
        }

        void EnqueueWireBytes(std::vector<std::uint8_t> bytes) {
            if (bytes.empty()) return;
            std::lock_guard lock(write_mutex);
            write_queue.push_back(std::move(bytes));
        }

        void EnqueueLmpFrame(std::vector<std::uint8_t> lmp_bytes) {
            if (protocol == ConnProtocol::WebSocketLmp1) {
                EnqueueWireBytes(EncodeWebSocketBinaryFrame(lmp_bytes));
            } else {
                EnqueueWireBytes(std::move(lmp_bytes));
            }
        }
    };

    static void CloseNativeSocket(Sock s) noexcept {
        if (s == kInvalidSock) return;
#ifdef _WIN32
        ::closesocket(s);
#else
        ::close(s);
#endif
    }

    static void SetNonBlocking(Sock s) {
#ifdef _WIN32
        u_long mode = 1;
        if (::ioctlsocket(s, FIONBIO, &mode) != 0) {
            throw std::runtime_error("Failed to set nonblocking socket");
        }
#else
        const int flags = ::fcntl(s, F_GETFL, 0);
        if (flags < 0 || ::fcntl(s, F_SETFL, flags | O_NONBLOCK) != 0) {
            throw std::runtime_error("Failed to set nonblocking socket");
        }
#endif
    }

    static void SetTcpNoDelay(Sock s) noexcept {
        int flag = 1;
#ifdef _WIN32
        ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&flag), sizeof(flag));
#else
        ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
#endif
    }

    static bool WouldBlock() noexcept {
#ifdef _WIN32
        return ::WSAGetLastError() == WSAEWOULDBLOCK;
#else
        return errno == EWOULDBLOCK || errno == EAGAIN;
#endif
    }

    void InitListener() {
        listen_sock_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listen_sock_ == kInvalidSock) {
            throw std::runtime_error("Failed to create listener socket");
        }

        int reuse = 1;
#ifdef _WIN32
        ::setsockopt(listen_sock_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#else
        ::setsockopt(listen_sock_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(config_.port);
        if (::inet_pton(AF_INET, config_.host.c_str(), &addr.sin_addr) != 1) {
            CloseNativeSocket(listen_sock_);
            throw std::runtime_error("Invalid bind host address: " + config_.host);
        }

        if (::bind(listen_sock_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 ||
            ::listen(listen_sock_, config_.listen_backlog) != 0) {
            CloseNativeSocket(listen_sock_);
            throw std::runtime_error("Failed to bind/listen on binary server port");
        }

        sockaddr_in bound{};
#ifdef _WIN32
        int len = sizeof(bound);
#else
        socklen_t len = sizeof(bound);
#endif
        if (::getsockname(listen_sock_, reinterpret_cast<sockaddr*>(&bound), &len) == 0) {
            bound_port_ = ntohs(bound.sin_port);
        } else {
            bound_port_ = config_.port;
        }

        SetNonBlocking(listen_sock_);
    }

    void InitWakeupPair() {
        Sock srv = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (srv == kInvalidSock) {
            throw std::runtime_error("Failed to create wakeup socket");
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;

        if (::bind(srv, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 ||
            ::listen(srv, 1) != 0) {
            CloseNativeSocket(srv);
            throw std::runtime_error("Failed to bind wakeup socket");
        }

        sockaddr_in bound{};
#ifdef _WIN32
        int len = sizeof(bound);
#else
        socklen_t len = sizeof(bound);
#endif
        ::getsockname(srv, reinterpret_cast<sockaddr*>(&bound), &len);

        wakeup_write_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (wakeup_write_ == kInvalidSock ||
            ::connect(wakeup_write_, reinterpret_cast<const sockaddr*>(&bound), sizeof(bound)) != 0) {
            CloseNativeSocket(srv);
            throw std::runtime_error("Failed to connect wakeup socket");
        }

        wakeup_read_ = ::accept(srv, nullptr, nullptr);
        CloseNativeSocket(srv);
        if (wakeup_read_ == kInvalidSock) {
            throw std::runtime_error("Failed to accept wakeup socket");
        }

        SetNonBlocking(wakeup_read_);
        SetNonBlocking(wakeup_write_);
        SetTcpNoDelay(wakeup_write_);
    }

    void Wakeup() noexcept {
        bool expected = false;
        if (wakeup_pending_.compare_exchange_strong(expected, true) &&
            wakeup_write_ != kInvalidSock) {
            const char byte = 1;
            ::send(wakeup_write_, &byte, 1, 0);
        }
    }

    void DrainWakeup() noexcept {
        char buf[128];
        while (true) {
            const int n = ::recv(wakeup_read_, buf, sizeof(buf), 0);
            if (n <= 0) break;
        }
        wakeup_pending_.store(false, std::memory_order_release);
    }

    void AcceptPending() {
        while (true) {
            Sock client = ::accept(listen_sock_, nullptr, nullptr);
            if (client == kInvalidSock) {
                break;
            }
            std::lock_guard lock(conn_mutex_);
            if (connections_.size() >= config_.max_connections) {
                CloseNativeSocket(client);
                continue;
            }
            SetNonBlocking(client);
            SetTcpNoDelay(client);
            auto conn = std::make_shared<Connection>();
            conn->id = next_conn_id_++;
            conn->sock = client;
            connections_.emplace(conn->id, std::move(conn));
        }
    }

    void ReactorLoop() {
        std::vector<PollEntry> poll_fds;
        std::vector<std::shared_ptr<Connection>> poll_conns;

        while (running_.load(std::memory_order_acquire)) {
            poll_fds.clear();
            poll_conns.clear();

            poll_fds.push_back(PollEntry{listen_sock_, kReadEvt, 0});
            poll_fds.push_back(PollEntry{wakeup_read_, kReadEvt, 0});

            {
                std::lock_guard lock(conn_mutex_);
                poll_conns.reserve(connections_.size());
                for (const auto& [id, conn] : connections_) {
                    (void)id;
                    short ev = kReadEvt;
                    if (conn->HasPendingWrites()) {
                        ev = static_cast<short>(ev | kWriteEvt);
                    }
                    poll_fds.push_back(PollEntry{conn->sock, ev, 0});
                    poll_conns.push_back(conn);
                }
            }

#ifdef _WIN32
            const int ready = ::WSAPoll(poll_fds.data(), static_cast<ULONG>(poll_fds.size()), 15);
#else
            const int ready = ::poll(poll_fds.data(), poll_fds.size(), 15);
#endif
            if (ready < 0) {
                continue;
            }

            if ((poll_fds[0].revents & kReadEvt) != 0) {
                AcceptPending();
            }
            if ((poll_fds[1].revents & kReadEvt) != 0) {
                DrainWakeup();
            }

            std::vector<std::uint64_t> to_close;
            for (std::size_t i = 0; i < poll_conns.size(); ++i) {
                auto& conn = poll_conns[i];
                const short rev = poll_fds[i + 2U].revents;
                bool alive = true;

                if ((rev & (POLLERR | POLLHUP | POLLNVAL)) != 0 && (rev & kReadEvt) == 0) {
                    alive = false;
                }
                if (alive && (rev & kReadEvt) != 0) {
                    alive = HandleRead(conn);
                }
                if (alive && ((rev & kWriteEvt) != 0 || conn->HasPendingWrites())) {
                    alive = HandleWrite(conn);
                }
                if (!alive || (conn->close_after_flush && !conn->HasPendingWrites())) {
                    to_close.push_back(conn->id);
                }
            }

            if (!to_close.empty()) {
                std::lock_guard lock(conn_mutex_);
                for (const auto cid : to_close) {
                    if (const auto it = connections_.find(cid); it != connections_.end()) {
                        CloseNativeSocket(it->second->sock);
                        it->second->sock = kInvalidSock;
                        connections_.erase(it);
                    }
                }
            }
        }
    }

    bool HandleWrite(const std::shared_ptr<Connection>& conn) {
        std::lock_guard lock(conn->write_mutex);
        while (!conn->write_queue.empty()) {
            const auto& front = conn->write_queue.front();
            const std::size_t remaining = front.size() - conn->write_offset;
            const int sent = ::send(
                conn->sock,
                reinterpret_cast<const char*>(front.data() + conn->write_offset),
                static_cast<int>(remaining),
                0
            );
            if (sent < 0) {
                return WouldBlock();
            }
            if (sent == 0) {
                return false;
            }
            conn->write_offset += static_cast<std::size_t>(sent);
            if (conn->write_offset == front.size()) {
                conn->write_queue.pop_front();
                conn->write_offset = 0;
            }
        }
        return true;
    }

    bool HandleRead(const std::shared_ptr<Connection>& conn) {
        std::uint8_t buf[32768];
        while (true) {
            const int n = ::recv(conn->sock, reinterpret_cast<char*>(buf), sizeof(buf), 0);
            if (n == 0) {
                return false;
            }
            if (n < 0) {
                if (WouldBlock()) break;
                return false;
            }
            conn->raw_in.insert(conn->raw_in.end(), buf, buf + n);
            if (conn->raw_in.size() > config_.max_frame_bytes * 2U) {
                return false;
            }
        }

        if (conn->protocol == ConnProtocol::Detecting) {
            if (conn->raw_in.size() < 4U) {
                return true;
            }
            if (conn->raw_in[0] == 'L' && conn->raw_in[1] == 'M' &&
                conn->raw_in[2] == 'P' && conn->raw_in[3] == '1') {
                conn->protocol = ConnProtocol::RawLmp1;
                conn->lmp_decoder.Append(conn->raw_in);
                conn->raw_in.clear();
            } else if (conn->raw_in[0] == 'G' && conn->raw_in[1] == 'E' &&
                       conn->raw_in[2] == 'T' && conn->raw_in[3] == ' ') {
                return TryWebSocketUpgrade(conn);
            } else {
                static constexpr std::string_view kReject =
                    "HTTP/1.1 426 Upgrade Required\r\n"
                    "Connection: close\r\n"
                    "Content-Type: text/plain\r\n"
                    "Content-Length: 42\r\n\r\n"
                    "Lamp requires LMP1 binary or WebSocket RPC";
                conn->EnqueueWireBytes(
                    std::vector<std::uint8_t>(kReject.begin(), kReject.end())
                );
                conn->close_after_flush = true;
                return true;
            }
        } else if (conn->protocol == ConnProtocol::RawLmp1) {
            if (!conn->raw_in.empty()) {
                conn->lmp_decoder.Append(conn->raw_in);
                conn->raw_in.clear();
            }
        } else if (conn->protocol == ConnProtocol::WebSocketLmp1) {
            if (!ProcessWebSocketFrames(conn)) {
                return false;
            }
        }

        return DrainLmpFrames(conn);
    }

    bool TryWebSocketUpgrade(const std::shared_ptr<Connection>& conn) {
        const std::string_view view(
            reinterpret_cast<const char*>(conn->raw_in.data()),
            conn->raw_in.size()
        );
        const auto header_end = view.find("\r\n\r\n");
        if (header_end == std::string_view::npos) {
            return conn->raw_in.size() <= 16384U;
        }

        const std::string_view headers = view.substr(0, header_end);
        std::string_view ws_key;
        bool has_upgrade_ws = false;

        std::size_t pos = 0;
        while (pos < headers.size()) {
            const auto nl = headers.find("\r\n", pos);
            const auto line = nl == std::string_view::npos
                ? headers.substr(pos)
                : headers.substr(pos, nl - pos);
            const auto colon = line.find(':');
            if (colon != std::string_view::npos) {
                auto name = line.substr(0, colon);
                auto val = line.substr(colon + 1);
                while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) {
                    val.remove_prefix(1);
                }
                if (security::detail::StartsWithIgnoreCase(name, "sec-websocket-key")) {
                    ws_key = val;
                } else if (security::detail::StartsWithIgnoreCase(name, "upgrade") &&
                           security::detail::ContainsIgnoreCase(val, "websocket")) {
                    has_upgrade_ws = true;
                }
            }
            if (nl == std::string_view::npos) break;
            pos = nl + 2;
        }

        if (!has_upgrade_ws || ws_key.empty()) {
            static constexpr std::string_view kNoRest =
                "HTTP/1.1 404 Not Found\r\n"
                "Connection: close\r\n"
                "Content-Length: 36\r\n\r\n"
                "Lamp does not expose HTTP REST endpoints";
            conn->EnqueueWireBytes(
                std::vector<std::uint8_t>(kNoRest.begin(), kNoRest.end())
            );
            conn->close_after_flush = true;
            return true;
        }

        const std::string accept = ComputeWebSocketAcceptKey(ws_key);
        std::string handshake =
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";

        conn->protocol = ConnProtocol::WebSocketLmp1;
        conn->EnqueueWireBytes(
            std::vector<std::uint8_t>(handshake.begin(), handshake.end())
        );

        const std::size_t consumed = header_end + 4U;
        conn->raw_in.erase(
            conn->raw_in.begin(),
            conn->raw_in.begin() + static_cast<std::ptrdiff_t>(consumed)
        );
        return ProcessWebSocketFrames(conn);
    }

    bool ProcessWebSocketFrames(const std::shared_ptr<Connection>& conn) {
        std::size_t pos = 0;
        while (conn->raw_in.size() - pos >= 2U) {
            const std::uint8_t b0 = conn->raw_in[pos];
            const std::uint8_t b1 = conn->raw_in[pos + 1U];
            const std::uint8_t opcode = b0 & 0x0FU;
            const bool masked = (b1 & 0x80U) != 0;
            std::uint64_t payload_len = b1 & 0x7FU;
            std::size_t hdr_len = 2U;

            if (payload_len == 126U) {
                if (conn->raw_in.size() - pos < hdr_len + 2U) break;
                payload_len = detail::ReadU16BE(conn->raw_in.data() + pos + hdr_len);
                hdr_len += 2U;
            } else if (payload_len == 127U) {
                if (conn->raw_in.size() - pos < hdr_len + 8U) break;
                payload_len = detail::ReadU64BE(conn->raw_in.data() + pos + hdr_len);
                hdr_len += 8U;
            }

            if (payload_len > config_.max_frame_bytes) {
                return false;
            }

            const std::size_t mask_bytes = masked ? 4U : 0U;
            if (conn->raw_in.size() - pos < hdr_len + mask_bytes + payload_len) {
                break;
            }

            const std::uint8_t* mask_ptr = masked ? (conn->raw_in.data() + pos + hdr_len) : nullptr;
            const std::size_t data_start = pos + hdr_len + mask_bytes;

            std::vector<std::uint8_t> payload(
                conn->raw_in.begin() + static_cast<std::ptrdiff_t>(data_start),
                conn->raw_in.begin() + static_cast<std::ptrdiff_t>(data_start + payload_len)
            );
            if (masked) {
                for (std::size_t i = 0; i < payload.size(); ++i) {
                    payload[i] ^= mask_ptr[i & 3U];
                }
            }

            pos = data_start + static_cast<std::size_t>(payload_len);

            if (opcode == 0x08U) { // close
                return false;
            }
            if (opcode == 0x09U) { // ping -> pong
                std::vector<std::uint8_t> pong;
                pong.push_back(0x8AU);
                if (payload.size() <= 125U) {
                    pong.push_back(static_cast<std::uint8_t>(payload.size()));
                    pong.insert(pong.end(), payload.begin(), payload.end());
                    conn->EnqueueWireBytes(std::move(pong));
                }
                continue;
            }
            if (opcode == 0x02U || opcode == 0x00U) {
                conn->lmp_decoder.Append(payload);
            }
        }

        if (pos > 0) {
            conn->raw_in.erase(
                conn->raw_in.begin(),
                conn->raw_in.begin() + static_cast<std::ptrdiff_t>(pos)
            );
        }
        return true;
    }

    bool DrainLmpFrames(const std::shared_ptr<Connection>& conn) {
        LmpFrame frame;
        while (true) {
            const auto st = conn->lmp_decoder.TryNextFrame(frame);
            if (st == LmpFrameDecoder::FeedStatus::NeedMoreData) {
                return true;
            }
            if (st == LmpFrameDecoder::FeedStatus::ProtocolError) {
                return false;
            }

            if (frame.header.type == FrameType::Ping) {
                conn->EnqueueLmpFrame(EncodeLmpFrame(
                    FrameType::Pong,
                    frame.header.request_id,
                    frame.payload
                ));
                continue;
            }

            if (frame.header.type != FrameType::Request) {
                continue;
            }

            HandleRpcRequestFrame(conn, std::move(frame));
        }
    }

    void HandleRpcRequestFrame(
        const std::shared_ptr<Connection>& conn,
        LmpFrame frame
    ) {
        DecodedRpcInvocation decoded;
        if (!LcbCodec::DecodeRpcRequest(frame.payload, decoded)) {
            const auto err_payload = LcbCodec::EncodeRpcResponse(
                false,
                serialization::Value::MakeNull(),
                "INVALID_ARGUMENT",
                "MALFORMED_LCB1_FRAME",
                "Unable to decode binary request payload"
            );
            conn->EnqueueLmpFrame(EncodeLmpFrame(
                FrameType::Error,
                frame.header.request_id,
                err_payload
            ));
            return;
        }

        // Fast full-cycle wire cache check BEFORE entering worker dispatch or handlers.
        if (wire_cache_) {
            if (const auto cached_resp = wire_cache_->Lookup(decoded.operation_id, frame.payload)) {
                conn->EnqueueLmpFrame(EncodeLmpFrame(
                    FrameType::Response,
                    frame.header.request_id,
                    *cached_resp,
                    FrameFlags::EndOfStream | FrameFlags::CacheHit
                ));
                return;
            }
        }

        // Dispatch asynchronously on dynamic scaling worker pool.
        std::weak_ptr<Connection> conn_weak = conn;
        const std::uint64_t req_id = frame.header.request_id;
        std::vector<std::uint8_t> raw_request_key = std::move(frame.payload);

        (void)pool_->Submit([
            this,
            conn_weak = std::move(conn_weak),
            req_id,
            decoded = std::move(decoded),
            raw_request_key = std::move(raw_request_key)
        ]() mutable {
            BinaryRpcDispatchResult res;
            try {
                res = handler_(decoded, req_id);
            } catch (...) {
                res.ok = false;
                res.encoded_lcb_response = LcbCodec::EncodeRpcResponse(
                    false,
                    serialization::Value::MakeNull(),
                    "INTERNAL",
                    "UNHANDLED_BINARY_HANDLER_EXCEPTION",
                    "Internal server error"
                );
            }

            if (res.ok && wire_cache_ && res.cache_ttl.count() > 0) {
                (void)wire_cache_->Store(
                    decoded.operation_id,
                    raw_request_key,
                    res.encoded_lcb_response,
                    res.cache_ttl
                );
            }

            if (auto target = conn_weak.lock()) {
                target->EnqueueLmpFrame(EncodeLmpFrame(
                    res.ok ? FrameType::Response : FrameType::Error,
                    req_id,
                    res.encoded_lcb_response,
                    static_cast<std::uint8_t>(FrameFlags::EndOfStream)
                ));
                Wakeup();
            }
        });
    }

    BinaryOperationHandler handler_;
    LampBinaryServerConfig config_;
    std::shared_ptr<core::DynamicWorkerPool> pool_;
    std::shared_ptr<FullCycleWireCache> wire_cache_;

    Sock listen_sock_ = kInvalidSock;
    Sock wakeup_read_ = kInvalidSock;
    Sock wakeup_write_ = kInvalidSock;
    std::uint16_t bound_port_ = 0;

    std::atomic<bool> started_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> wakeup_pending_{false};
    std::uint64_t next_conn_id_ = 1;

    std::mutex conn_mutex_;
    std::unordered_map<std::uint64_t, std::shared_ptr<Connection>> connections_;
    std::thread reactor_thread_;

#ifdef _WIN32
    bool wsa_initialized_ = false;
#endif
};

} // namespace lamp::transport
