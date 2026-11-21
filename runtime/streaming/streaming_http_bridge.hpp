#pragma once

// ============================================================
// Lamp Framework
// Phase 9 - Streaming and Real-Time
//
// File: runtime/streaming/streaming_http_bridge.hpp
//
// Purpose:
//   Nonblocking HTTP/1.1 chunked stream pump pulling from
//   StreamChannel with write-buffer backpressure, partial
//   write consumption, and disconnect cancellation.
// ============================================================

#include "streaming/chunked_http_codec.hpp"
#include "streaming/stream_channel.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace lamp::streaming {

class HttpChunkedStreamPump final {
public:
    HttpChunkedStreamPump(
        std::shared_ptr<StreamChannel> channel,
        http::HttpStatus status = http::HttpStatus::Ok,
        http::HttpHeaders headers = {},
        http::HttpVersion version = {},
        std::size_t max_pending_write_bytes = 256U * 1024U
    )
        : channel_(std::move(channel)),
          max_pending_write_bytes_(max_pending_write_bytes) {
        if (!channel_) {
            throw std::invalid_argument(
                "HttpChunkedStreamPump requires a StreamChannel"
            );
        }
        if (max_pending_write_bytes_ == 0) {
            throw std::invalid_argument(
                "max_pending_write_bytes must be positive"
            );
        }

        output_ = ChunkedHttpEncoder::EncodeHeader(
            status,
            headers,
            version
        );
    }

    // Pulls available chunks from StreamChannel up to
    // max_pending_write_bytes_. Leaves any excess chunks inside
    // StreamChannel so producer backpressure works end-to-end.
    std::size_t PollChunks() {
        if (finished_ || cancelled_) {
            return 0;
        }

        std::size_t pulled = 0;

        while (PendingOutputBytes() < max_pending_write_bytes_) {
            auto chunk = channel_->TryPop();
            if (!chunk.has_value()) {
                break;
            }
            ++pulled;
            const std::string encoded =
                ChunkedHttpEncoder::EncodeChunk(chunk->data);
            if (!encoded.empty()) {
                CompactOutput();
                output_.append(encoded);
            }
        }

        if (channel_->IsDrainedAndClosed() && !finished_) {
            CompactOutput();
            output_.append(ChunkedHttpEncoder::EncodeFinalChunk());
            finished_ = true;
        }

        return pulled;
    }

    [[nodiscard]]
    std::string_view PendingOutput() const noexcept {
        if (output_offset_ >= output_.size()) {
            return {};
        }
        return std::string_view(output_).substr(output_offset_);
    }

    [[nodiscard]]
    bool HasPendingOutput() const noexcept {
        return output_offset_ < output_.size();
    }

    [[nodiscard]]
    std::size_t PendingOutputBytes() const noexcept {
        return output_.size() - output_offset_;
    }

    void ConsumeOutput(std::size_t count) {
        const std::size_t available = PendingOutputBytes();
        if (count > available) {
            throw std::out_of_range(
                "Cannot consume more bytes than pending"
            );
        }
        output_offset_ += count;
        if (output_offset_ == output_.size()) {
            output_.clear();
            output_offset_ = 0;
        } else if (output_offset_ > output_.size() / 2) {
            CompactOutput();
        }
    }

    void Cancel() noexcept {
        cancelled_ = true;
        channel_->Cancel();
        output_.clear();
        output_offset_ = 0;
    }

    [[nodiscard]]
    bool ShouldClose() const noexcept {
        return cancelled_ || (finished_ && !HasPendingOutput());
    }

    [[nodiscard]]
    bool IsFinished() const noexcept {
        return finished_;
    }

private:
    void CompactOutput() {
        if (output_offset_ > 0) {
            output_.erase(0, output_offset_);
            output_offset_ = 0;
        }
    }

    std::shared_ptr<StreamChannel> channel_;
    std::size_t max_pending_write_bytes_;
    std::string output_;
    std::size_t output_offset_ = 0;
    bool finished_ = false;
    bool cancelled_ = false;
};

} // namespace lamp::streaming
