
#pragma once

#include "http/http_parser.hpp"
#include "http/http_router.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace lamp::http {

// ============================================================
// HTTP Connection Configuration
// ============================================================

struct HttpConnectionConfig final {
    HttpParserConfig parser{};

    // Maximum number of queued response bytes.
    std::size_t maxPendingWriteBytes = 16 * 1024 * 1024;

    // Maximum requests handled on one connection.
    std::size_t maxRequestsPerConnection = 1000;

    // Whether persistent connections are permitted.
    bool enableKeepAlive = true;
};

// ============================================================
// HTTP Connection Errors
// ============================================================

class HttpConnectionError final : public std::runtime_error {
public:
    explicit HttpConnectionError(const std::string& message)
        : std::runtime_error(message) {}
};

// ============================================================
// HTTP Connection State
// ============================================================

enum class HttpConnectionState : std::uint8_t {
    Open,
    Closing,
    Closed,
    Failed
};

// ============================================================
// HTTP Connection Input Result
// ============================================================

struct HttpConnectionInputResult final {
    std::size_t consumed = 0;
    std::size_t requestsCompleted = 0;

    HttpParseError parseError = HttpParseError::None;

    bool shouldClose = false;

    [[nodiscard]]
    bool failed() const noexcept {
        return parseError != HttpParseError::None;
    }
};

// ============================================================
// HTTP Connection
// ============================================================
//
// Responsibilities:
//
// - Incrementally parse incoming HTTP requests
// - Dispatch completed requests through HttpRouter
// - Serialize HTTP responses
// - Preserve response ordering
// - Manage HTTP/1.0 and HTTP/1.1 persistence
// - Buffer responses for partial socket writes
// - Enforce per-connection request and buffer limits
//
// Transport independence:
//
// This class does not create sockets or perform I/O.
//
// The native HTTP server is responsible for:
//
// - Reading bytes from sockets
// - Calling receive()
// - Sending bytes from pendingOutput()
// - Calling consumeOutput() after successful writes
// - Closing sockets when shouldClose() becomes true
//
// Thread safety:
//
// All public operations are synchronized.
//
// A connection should nevertheless be owned by one I/O
// event loop or a serialized connection executor.
//
// ============================================================

class HttpConnection final {
public:
    explicit HttpConnection(
        std::shared_ptr<const HttpRouter> router,
        HttpConnectionConfig config = {}
    )
        : router_(std::move(router)),
          config_(std::move(config)),
          parser_(config_.parser) {
        if (!router_) {
            throw HttpConnectionError(
                "HTTP connection requires a router"
            );
        }

        if (config_.maxPendingWriteBytes == 0) {
            throw HttpConnectionError(
                "Maximum pending write bytes must be positive"
            );
        }

        if (config_.maxRequestsPerConnection == 0) {
            throw HttpConnectionError(
                "Maximum requests per connection must be positive"
            );
        }
    }

    HttpConnection(const HttpConnection&) = delete;
    HttpConnection& operator=(const HttpConnection&) = delete;

    HttpConnection(HttpConnection&&) = delete;
    HttpConnection& operator=(HttpConnection&&) = delete;

    // ========================================================
    // Receive Incoming Bytes
    // ========================================================
    //
    // Processes as many complete requests as possible.
    //
    // Parsing stops when:
    //
    // - All input bytes are consumed
    // - The connection must close
    // - A parsing error occurs
    // - A response buffer limit is reached
    //
    // ========================================================

    [[nodiscard]]
    HttpConnectionInputResult receive(
        std::string_view input
    ) {
        std::lock_guard<std::mutex> lock(mutex_);

        HttpConnectionInputResult result;

        if (state_ != HttpConnectionState::Open) {
            result.shouldClose = shouldCloseLocked();
            return result;
        }

        while (result.consumed < input.size()) {
            const auto remaining =
                input.substr(result.consumed);

            const auto parseResult =
                parser_.feed(remaining);

            result.consumed += parseResult.consumed;

            if (parseResult.failed()) {
                result.parseError = parseResult.error;

                handleParseErrorLocked(
                    parseResult.error
                );

                break;
            }

            if (!parseResult.completed) {
                break;
            }

            HttpRequest request = parser_.takeRequest();

            ++requestsHandled_;
            ++result.requestsCompleted;

            processRequestLocked(request);

            if (state_ != HttpConnectionState::Open) {
                break;
            }

            // Defensive progress guard.
            if (parseResult.consumed == 0) {
                break;
            }
        }

        result.shouldClose = shouldCloseLocked();

        return result;
    }

    // ========================================================
    // Pending Output
    // ========================================================
    //
    // Returns a copy of the next pending output segment.
    //
    // The transport may write fewer bytes than returned.
    // It must report the actual number written using
    // consumeOutput().
    //
    // Copying avoids exposing references into mutable
    // buffers after releasing the connection mutex.
    //
    // ========================================================

    [[nodiscard]]
    std::string pendingOutput(
        std::size_t maxBytes =
            std::numeric_limits<std::size_t>::max()
    ) const {
        std::lock_guard<std::mutex> lock(mutex_);

        if (outputQueue_.empty() || maxBytes == 0) {
            return {};
        }

        const auto& front = outputQueue_.front();

        const auto available =
            front.size() - outputOffset_;

        const auto amount =
            std::min(available, maxBytes);

        return front.substr(outputOffset_, amount);
    }

    // ========================================================
    // Consume Written Bytes
    // ========================================================
    //
    // Called only after the transport confirms that bytes
    // were successfully written to the socket.
    //
    // ========================================================

    void consumeOutput(std::size_t bytesWritten) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (bytesWritten > pendingWriteBytes_) {
            throw HttpConnectionError(
                "Cannot consume more bytes than pending"
            );
        }

        while (bytesWritten > 0) {
            auto& front = outputQueue_.front();

            const auto available =
                front.size() - outputOffset_;

            const auto amount =
                std::min(available, bytesWritten);

            outputOffset_ += amount;
            pendingWriteBytes_ -= amount;
            bytesWritten -= amount;

            if (outputOffset_ == front.size()) {
                outputQueue_.pop_front();
                outputOffset_ = 0;
            }
        }

        if (state_ == HttpConnectionState::Closing &&
            outputQueue_.empty()) {
            state_ = HttpConnectionState::Closed;
        }
    }

    // ========================================================
    // Output Inspection
    // ========================================================

    [[nodiscard]]
    bool hasPendingOutput() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return pendingWriteBytes_ != 0;
    }

    [[nodiscard]]
    std::size_t pendingWriteBytes() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return pendingWriteBytes_;
    }

    // ========================================================
    // Connection State
    // ========================================================

    [[nodiscard]]
    HttpConnectionState state() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return state_;
    }

    [[nodiscard]]
    bool isOpen() const noexcept {
        return state() == HttpConnectionState::Open;
    }

    [[nodiscard]]
    bool shouldClose() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return shouldCloseLocked();
    }

    [[nodiscard]]
    std::size_t requestsHandled() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return requestsHandled_;
    }

    // ========================================================
    // Graceful Close
    // ========================================================
    //
    // Stops accepting additional requests while allowing
    // queued responses to be flushed.
    //
    // ========================================================

    void close() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);

        if (state_ == HttpConnectionState::Open) {
            state_ = outputQueue_.empty()
                ? HttpConnectionState::Closed
                : HttpConnectionState::Closing;
        }
    }

    // ========================================================
    // Abort
    // ========================================================
    //
    // Immediately discards pending output.
    //
    // ========================================================

    void abort() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);

        outputQueue_.clear();
        outputOffset_ = 0;
        pendingWriteBytes_ = 0;

        state_ = HttpConnectionState::Failed;
    }

private:

    void abortOutputLocked() noexcept {
        outputQueue_.clear();
        outputOffset_ = 0;
        pendingWriteBytes_ = 0;
        state_ = HttpConnectionState::Failed;
    }

    // ========================================================
    // Request Processing
    // ========================================================

    void processRequestLocked(
        const HttpRequest& request
    ) {
        const bool keepAlive =
            config_.enableKeepAlive &&
            request.isKeepAlive() &&
            requestsHandled_ <
                config_.maxRequestsPerConnection;

        HttpResponse response;

        try {
            response = router_->dispatch(request);
        } catch (const std::exception&) {
            response = HttpResponse(
                HttpStatus::InternalServerError
            );
        } catch (...) {
            response = HttpResponse(
                HttpStatus::InternalServerError
            );
        }

        response.setVersion(request.version());

        if (!keepAlive) {
            response.setHeader(
                "Connection",
                "close"
            );
        } else if (request.version().isHttp10()) {
            response.setHeader(
                "Connection",
                "keep-alive"
            );
        }

        // HEAD responses must not transmit body bytes.
        //
        // The Content-Length should describe the body
        // that a corresponding GET response would send.
        //
        // The existing HttpResponse serializer does not
        // provide request-aware body suppression.
        //
        // Therefore, serialize first, then remove only
        // the payload bytes for HEAD.

        std::string serialized;

        try {
            serialized = response.serialize();

            if (request.method() == HttpMethod::Head &&
                response.allowsBody() &&
                response.bodySize() > 0) {
                const auto bodySize = response.bodySize();

                if (serialized.size() < bodySize) {
                    throw HttpConnectionError(
                        "Invalid serialized HEAD response"
                    );
                }

                serialized.resize(
                    serialized.size() - bodySize
                );
            }
        } catch (const std::exception&) {
            HttpResponse fallback(
                HttpStatus::InternalServerError,
                request.version()
            );

            fallback.setHeader(
                "Connection",
                "close"
            );

            serialized = fallback.serialize();

            const bool queued = enqueueResponseLocked(
                std::move(serialized)
            );

            if (!queued) {
                // Response queue is full.
                // The connection must be closed.
                abortOutputLocked();
                return;
            }

            beginClosingLocked();
            return;
        }

        if (!enqueueResponseLocked(
                std::move(serialized)
            )) {
            beginClosingLocked();
            return;
        }

        if (!keepAlive) {
            beginClosingLocked();
        }
    }

    // ========================================================
    // Parse Error Handling
    // ========================================================

    void handleParseErrorLocked(
        HttpParseError error
    ) {
        HttpStatus status = HttpStatus::BadRequest;

        switch (error) {
            case HttpParseError::RequestLineTooLarge:
                status = HttpStatus::UriTooLong;
                break;

            case HttpParseError::HeadersTooLarge:
            case HttpParseError::TooManyHeaders:
                status =
                    HttpStatus::RequestHeaderFieldsTooLarge;
                break;

            case HttpParseError::BodyTooLarge:
                status = HttpStatus::PayloadTooLarge;
                break;

            case HttpParseError::UnsupportedExpectation:
                status = HttpStatus::ExpectationFailed;
                break;

            default:
                status = HttpStatus::BadRequest;
                break;
        }

        HttpResponse response(status);

        response.setHeader(
            "Connection",
            "close"
        );

        const auto serialized = response.serialize();

        if (!enqueueResponseLocked(std::move(serialized))) {
            abortOutputLocked();
            return;
        }

        beginClosingLocked();
    }

    // ========================================================
    // Response Queue
    // ========================================================

    [[nodiscard]]
    bool enqueueResponseLocked(
        std::string response
    ) {
        if (response.size() >
            config_.maxPendingWriteBytes -
                pendingWriteBytes_) {
            return false;
        }

        pendingWriteBytes_ += response.size();

        outputQueue_.push_back(
            std::move(response)
        );

        return true;
    }

    // ========================================================
    // Closing
    // ========================================================

    void beginClosingLocked() noexcept {
        if (state_ == HttpConnectionState::Open) {
            state_ = outputQueue_.empty()
                ? HttpConnectionState::Closed
                : HttpConnectionState::Closing;
        }
    }

    [[nodiscard]]
    bool shouldCloseLocked() const noexcept {
        return state_ == HttpConnectionState::Closed ||
               state_ == HttpConnectionState::Failed;
    }

    std::shared_ptr<const HttpRouter> router_;

    HttpConnectionConfig config_;
    HttpParser parser_;

    mutable std::mutex mutex_;

    HttpConnectionState state_ =
        HttpConnectionState::Open;

    std::deque<std::string> outputQueue_;

    std::size_t outputOffset_ = 0;
    std::size_t pendingWriteBytes_ = 0;

    std::size_t requestsHandled_ = 0;
};

} // namespace lamp::http
