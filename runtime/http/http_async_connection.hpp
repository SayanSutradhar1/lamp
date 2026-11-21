
#pragma once

#include "http/http_completion_queue.hpp"
#include "http/http_parser.hpp"
#include "http/http_request.hpp"
#include "http/http_response.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace lamp::http {

// ============================================================
// Async HTTP Connection
//
// Owns the HTTP protocol state for one logical connection.
//
// This class does not own a socket, scheduler or worker.
// The transport owner is responsible for:
//   - Feeding received bytes
//   - Dispatching parsed requests
//   - Delivering completed responses
//   - Sending and consuming buffered output
//   - Closing the underlying socket
//
// Thread affinity:
//   All methods except construction must be called by the
//   same connection-owner thread.
//
// Worker threads must publish completions through an
//   external handoff mechanism, such as HttpCompletionQueue.
// They must not call complete() directly.
//
// HTTP/1.x responses are emitted in request order even
// when execution completes out of order.
// ============================================================

class HttpAsyncConnectionError final
    : public std::runtime_error {
public:
    explicit HttpAsyncConnectionError(
        const std::string& message
    )
        : std::runtime_error(message) {}
};

struct HttpAsyncConnectionConfig final {
    HttpParserConfig parser{};

    std::size_t maxInflightRequests = 64;

    std::size_t maxPendingWriteBytes =
        4 * 1024 * 1024;

    std::size_t maxRetainedResponseBytes =
        8 * 1024 * 1024;
};

struct HttpAsyncReceiveResult final {
    std::size_t consumed = 0;
    std::size_t dispatched = 0;

    HttpParseError parseError = HttpParseError::None;

    bool backpressured = false;
    bool closed = false;

    [[nodiscard]]
    bool failed() const noexcept {
        return parseError != HttpParseError::None;
    }
};

enum class HttpAsyncCompleteResult : std::uint8_t {
    Accepted,
    UnknownRequest,
    StaleConnection,
    AlreadyCompleted,
    Closed
};

class HttpAsyncConnection final {
public:
    using DispatchCallback = std::function<void(
        const HttpCompletionToken&,
        HttpRequest
    )>;

    explicit HttpAsyncConnection(
        std::uint64_t connectionId,
        DispatchCallback dispatcher,
        HttpAsyncConnectionConfig config = {}
    )
        : connectionId_(connectionId),
          generation_(nextGeneration()),
          parser_(config.parser),
          dispatcher_(std::move(dispatcher)),
          maxInflight_(config.maxInflightRequests),
          maxPendingWriteBytes_(
              config.maxPendingWriteBytes
          ),
          maxRetainedResponseBytes_(
              config.maxRetainedResponseBytes
          ) {
        if (connectionId_ == 0) {
            throw HttpAsyncConnectionError(
                "Connection ID must be nonzero"
            );
        }

        if (!dispatcher_) {
            throw HttpAsyncConnectionError(
                "Async request dispatcher is required"
            );
        }

        if (maxInflight_ == 0 ||
            maxPendingWriteBytes_ == 0 ||
            maxRetainedResponseBytes_ == 0) {
            throw HttpAsyncConnectionError(
                "Connection limits must be positive"
            );
        }
    }

    HttpAsyncConnection(
        const HttpAsyncConnection&
    ) = delete;

    HttpAsyncConnection& operator=(
        const HttpAsyncConnection&
    ) = delete;

    HttpAsyncConnection(
        HttpAsyncConnection&&
    ) = delete;

    HttpAsyncConnection& operator=(
        HttpAsyncConnection&&
    ) = delete;

    // ========================================================
    // Receive HTTP Bytes
    //
    // May stop before consuming all input when:
    //   - In-flight capacity is reached
    //   - A Connection: close request is received
    //   - A parse error occurs
    //
    // The caller must retain unconsumed bytes and retry
    // after capacity becomes available.
    //
    // Dispatch callbacks run synchronously, but must not
    // wait for the operation to finish.
    // ========================================================

    [[nodiscard]]
    HttpAsyncReceiveResult receive(
        std::string_view input
    ) {
        HttpAsyncReceiveResult result;

        if (closed_ || !accepting_) {
            result.closed = true;
            return result;
        }

        while (!input.empty()) {
            flushReady();

            if (slots_.size() >= maxInflight_ ||
                pendingOutputBytes() >= maxPendingWriteBytes_ ||
                retainedResponseBytes_ >=
                    maxRetainedResponseBytes_) {
                result.backpressured = true;
                break;
            }

            const auto parsed = parser_.feed(input);

            result.consumed += parsed.consumed;
            input.remove_prefix(parsed.consumed);

            if (parsed.failed()) {
                result.parseError = parsed.error;
                handleParseError(parsed.error);
                flushReady();
                result.closed = true;
                return result;
            }

            if (!parsed.completed) {
                break;
            }

            HttpRequest request =
                parser_.takeRequest();

            const bool keepAlive =
                request.isKeepAlive();

            const HttpCompletionToken token{
                connectionId_,
                generation_,
                nextRequestId_++
            };

            slots_.push_back(Slot{
                token.requestId,
                request.method() == HttpMethod::Head,
                request.version(),
                keepAlive,
                std::nullopt,
                0
            });

            ++result.dispatched;

            try {
                dispatcher_(
                    token,
                    std::move(request)
                );
            } catch (...) {
                // A dispatch failure must not leave the
                // connection waiting indefinitely.
                HttpResponse failure(
                    HttpStatus::InternalServerError
                );

                failure.setBody(
                    "Internal Server Error"
                );

                (void)complete(
                    token,
                    std::move(failure)
                );
            }

            if (!keepAlive) {
                accepting_ = false;
                result.closed = true;
                break;
            }
        }

        flushReady();

        if (closed_ || !accepting_) {
            result.closed = true;
        }

        return result;
    }

    // ========================================================
    // Complete an Asynchronous Request
    //
    // Must execute on the connection-owner thread.
    //
    // A completion token from a previous connection
    // generation cannot modify this connection.
    // ========================================================

    [[nodiscard]]
    HttpAsyncCompleteResult complete(
        const HttpCompletionToken& token,
        HttpResponse response
    ) {
        if (closed_) {
            return HttpAsyncCompleteResult::Closed;
        }

        if (token.connectionId != connectionId_ ||
            token.generation != generation_) {
            return HttpAsyncCompleteResult::StaleConnection;
        }

        for (auto& slot : slots_) {
            if (slot.requestId != token.requestId) {
                continue;
            }

            if (slot.response.has_value()) {
                return HttpAsyncCompleteResult::AlreadyCompleted;
            }

            response.setVersion(slot.version);

            std::size_t estimated = estimateResponseBytes(response);

            if (slots_.front().requestId != token.requestId &&
                retainedResponseBytes_ + estimated >
                    maxRetainedResponseBytes_) {
                accepting_ = false;
                response = HttpResponse(
                    HttpStatus::ServiceUnavailable,
                    slot.version
                );
                response.setHeader("Connection", "close");
                response.setBody("Retained response limit exceeded");
                estimated = estimateResponseBytes(response);

                if (retainedResponseBytes_ + estimated >
                    maxRetainedResponseBytes_) {
                    response = HttpResponse(
                        HttpStatus::ServiceUnavailable,
                        slot.version
                    );
                    response.setHeader("Connection", "close");
                    estimated = estimateResponseBytes(response);
                }
            }

            slot.retainedBytes = estimated;
            retainedResponseBytes_ += estimated;

            slot.response.emplace(
                std::move(response)
            );

            flushReady();

            return HttpAsyncCompleteResult::Accepted;
        }

        return HttpAsyncCompleteResult::UnknownRequest;
    }

    // ========================================================
    // Buffered Output
    //
    // The returned view remains valid until the next
    // mutating call on this connection.
    //
    // The socket owner must handle partial sends.
    // ========================================================

    [[nodiscard]]
    std::string_view pendingOutput() const noexcept {
        if (outputOffset_ >= output_.size()) {
            return {};
        }

        return std::string_view(output_).substr(
            outputOffset_
        );
    }

    [[nodiscard]]
    bool hasPendingOutput() const noexcept {
        return outputOffset_ < output_.size();
    }

    [[nodiscard]]
    std::size_t pendingOutputBytes() const noexcept {
        return output_.size() - outputOffset_;
    }

    void consumeOutput(std::size_t count) {
        const auto available =
            output_.size() - outputOffset_;

        if (count > available) {
            throw HttpAsyncConnectionError(
                "Cannot consume more bytes than pending"
            );
        }

        outputOffset_ += count;

        if (outputOffset_ == output_.size()) {
            output_.clear();
            outputOffset_ = 0;
        } else if (
            outputOffset_ > output_.size() / 2
        ) {
            output_.erase(0, outputOffset_);
            outputOffset_ = 0;
        }

        flushReady();
    }

    // ========================================================
    // Lifecycle
    // ========================================================

    // Stops accepting requests, but preserves outstanding
    // responses and buffered output for graceful draining.

    void stopAccepting() noexcept {
        accepting_ = false;
    }

    // Discards pending protocol state. The transport owner
    // must separately cancel outstanding execution tasks.

    void abort() noexcept {
        closed_ = true;
        accepting_ = false;

        slots_.clear();
        retainedResponseBytes_ = 0;
        output_.clear();
        outputOffset_ = 0;

        parser_.reset();
    }

    [[nodiscard]]
    bool shouldClose() const noexcept {
        return closed_ ||
            (!accepting_ &&
             slots_.empty() &&
             !hasPendingOutput());
    }

    [[nodiscard]]
    bool closed() const noexcept {
        return closed_;
    }

    [[nodiscard]]
    bool accepting() const noexcept {
        return accepting_ && !closed_;
    }

    [[nodiscard]]
    bool canReceive() const noexcept {
        return accepting() &&
            slots_.size() < maxInflight_ &&
            pendingOutputBytes() < maxPendingWriteBytes_ &&
            retainedResponseBytes_ < maxRetainedResponseBytes_;
    }

    [[nodiscard]]
    std::size_t retainedResponseBytes() const noexcept {
        return retainedResponseBytes_;
    }

    [[nodiscard]]
    std::size_t inflight() const noexcept {
        return slots_.size();
    }

    [[nodiscard]]
    std::size_t capacity() const noexcept {
        return maxInflight_;
    }

    [[nodiscard]]
    std::uint64_t connectionId() const noexcept {
        return connectionId_;
    }

    [[nodiscard]]
    std::uint64_t generation() const noexcept {
        return generation_;
    }

private:
    struct Slot final {
        std::uint64_t requestId;
        bool headRequest;
        HttpVersion version;
        bool keepAlive;
        std::optional<HttpResponse> response;
        std::size_t retainedBytes = 0;
    };

    [[nodiscard]]
    static std::size_t estimateResponseBytes(
        const HttpResponse& response
    ) noexcept {
        std::size_t total = response.bodySize() + 64;
        for (const auto& header : response.headers()) {
            total += header.name.size() +
                     header.value.size() + 4;
        }
        return total;
    }

    // ========================================================
    // Parse Error Handling
    // ========================================================

    void handleParseError(HttpParseError error) {
        accepting_ = false;
        parser_.reset();

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
        response.setHeader("Connection", "close");

        const std::size_t estimated =
            estimateResponseBytes(response);
        retainedResponseBytes_ += estimated;

        slots_.push_back(Slot{
            nextRequestId_++,
            false,
            HttpVersion{},
            false,
            std::move(response),
            estimated
        });
    }

    // ========================================================
    // Ordered Response Publication
    //
    // Completed later requests remain buffered until all
    // earlier requests have been serialized.
    // ========================================================

    [[nodiscard]]
    static std::string serializeForSlot(
        HttpResponse& response,
        const Slot& slot
    ) {
        response.setVersion(slot.version);

        if (!slot.keepAlive) {
            response.setHeader("Connection", "close");
        } else if (slot.version.isHttp10() &&
                   !response.header("Connection").has_value()) {
            response.setHeader("Connection", "keep-alive");
        }

        // HEAD must preserve the representation length
        // but suppress the transmitted body.
        //
        // HttpResponse::serialize() does not expose
        // a HEAD-specific serialization mode.
        // Serialize normally, then omit body bytes.

        std::string serialized;

        try {
            serialized = response.serialize();
        } catch (...) {
            HttpResponse failure(
                HttpStatus::InternalServerError,
                slot.version
            );

            if (!slot.keepAlive) {
                failure.setHeader("Connection", "close");
            }

            failure.setBody("Internal Server Error");

            serialized = failure.serialize();
        }

        if (slot.headRequest) {
            const auto headerEnd =
                serialized.find("\r\n\r\n");

            if (headerEnd != std::string::npos) {
                serialized.resize(headerEnd + 4);
            }
        }

        return serialized;
    }

    void flushReady() {
        while (!slots_.empty()) {
            auto& slot = slots_.front();

            if (!slot.response.has_value()) {
                break;
            }

            const auto pending = pendingOutputBytes();

            if (pending >= maxPendingWriteBytes_) {
                break;
            }

            std::string serialized = serializeForSlot(
                *slot.response,
                slot
            );

            const auto available =
                maxPendingWriteBytes_ - pending;

            if (serialized.size() > available) {
                if (pending > 0) {
                    break;
                }

                // No pending bytes remain to drain, yet this
                // single response exceeds maxPendingWriteBytes_.
                // Emit a compact 500 Connection: close error
                // if it fits; otherwise abort to avoid stall.
                accepting_ = false;

                HttpResponse fallback(
                    HttpStatus::InternalServerError,
                    slot.version
                );
                fallback.setHeader("Connection", "close");
                fallback.setBody("Response exceeds buffer limit");

                Slot fallbackSlot{
                    slot.requestId,
                    slot.headRequest,
                    slot.version,
                    false,
                    std::nullopt
                };

                std::string fallbackSerialized =
                    serializeForSlot(fallback, fallbackSlot);

                if (fallbackSerialized.size() >
                    maxPendingWriteBytes_) {
                    HttpResponse minimal(
                        HttpStatus::InternalServerError,
                        slot.version
                    );
                    minimal.setHeader("Connection", "close");
                    fallbackSerialized =
                        serializeForSlot(minimal, fallbackSlot);
                }

                if (fallbackSerialized.size() <=
                    maxPendingWriteBytes_) {
                    if (outputOffset_ != 0) {
                        output_.clear();
                        outputOffset_ = 0;
                    }
                    output_.append(fallbackSerialized);
                    slots_.clear();
                    retainedResponseBytes_ = 0;
                } else {
                    abort();
                }

                break;
            }

            if (outputOffset_ != 0) {
                output_.erase(0, outputOffset_);
                outputOffset_ = 0;
            }

            output_.append(serialized);

            const bool keepAlive = slot.keepAlive;
            if (retainedResponseBytes_ >= slot.retainedBytes) {
                retainedResponseBytes_ -= slot.retainedBytes;
            } else {
                retainedResponseBytes_ = 0;
            }

            slots_.pop_front();

            if (!keepAlive) {
                accepting_ = false;
                slots_.clear();
                retainedResponseBytes_ = 0;
                break;
            }
        }
    }

    [[nodiscard]]
    static std::uint64_t nextGeneration() noexcept {
        static std::atomic<std::uint64_t> counter{1};

        return counter.fetch_add(
            1,
            std::memory_order_relaxed
        );
    }

    const std::uint64_t connectionId_;
    const std::uint64_t generation_;

    HttpParser parser_;
    DispatchCallback dispatcher_;

    const std::size_t maxInflight_;
    const std::size_t maxPendingWriteBytes_;
    const std::size_t maxRetainedResponseBytes_;

    std::deque<Slot> slots_;
    std::size_t retainedResponseBytes_ = 0;

    std::string output_;
    std::size_t outputOffset_ = 0;

    std::uint64_t nextRequestId_ = 1;

    bool accepting_ = true;
    bool closed_ = false;
};

} // namespace lamp::http
