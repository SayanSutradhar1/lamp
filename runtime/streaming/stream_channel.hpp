#pragma once

// ============================================================
// Lamp Framework
// Phase 9 - Streaming and Real-Time
//
// File: runtime/streaming/stream_channel.hpp
//
// Purpose:
//   Thread-safe, bounded, backpressure-aware producer/consumer
//   streaming channel with cooperative cancellation and
//   event-loop wakeup support.
// ============================================================

#include "bridge/operation_descriptor.hpp"
#include "bridge/request_envelope.hpp"
#include "bridge/result_envelope.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace lamp::streaming {

struct StreamConfig final {
    std::size_t max_buffered_chunks = 64;
    std::size_t max_buffered_bytes = 4U * 1024U * 1024U;
    std::size_t max_chunk_bytes = 1U * 1024U * 1024U;
};

enum class StreamState : std::uint8_t {
    Open = 0,
    Completed = 1,
    Failed = 2,
    Cancelled = 3
};

enum class StreamPushStatus : std::uint8_t {
    Accepted = 0,
    Full = 1,
    Oversized = 2,
    Closed = 3,
    Cancelled = 4
};

struct StreamChunk final {
    std::vector<std::uint8_t> data;
    bridge::SerializationFormat format =
        bridge::SerializationFormat::Binary;
    std::uint64_t sequence = 0;

    [[nodiscard]]
    static StreamChunk FromString(
        std::string_view text,
        bridge::SerializationFormat fmt =
            bridge::SerializationFormat::Json
    ) {
        StreamChunk chunk;
        chunk.format = fmt;
        chunk.data.assign(text.begin(), text.end());
        return chunk;
    }

    [[nodiscard]]
    static StreamChunk FromBytes(
        std::span<const std::uint8_t> bytes,
        bridge::SerializationFormat fmt =
            bridge::SerializationFormat::Binary
    ) {
        StreamChunk chunk;
        chunk.format = fmt;
        chunk.data.assign(bytes.begin(), bytes.end());
        return chunk;
    }
};

struct StreamStats final {
    std::uint64_t chunks_pushed = 0;
    std::uint64_t chunks_popped = 0;
    std::uint64_t bytes_pushed = 0;
    std::uint64_t bytes_popped = 0;
    std::uint64_t backpressure_events = 0;
    std::size_t buffered_chunks = 0;
    std::size_t buffered_bytes = 0;
};

class StreamChannel final {
public:
    explicit StreamChannel(
        StreamConfig config = {},
        bridge::RequestCancellationToken external_cancel = {}
    )
        : config_(config),
          external_cancel_(std::move(external_cancel)),
          combined_cancel_(
              bridge::RequestCancellationToken::Combine(
                  external_cancel_,
                  local_cancel_.Token()
              )
          ) {
        if (config_.max_buffered_chunks == 0 ||
            config_.max_buffered_bytes == 0 ||
            config_.max_chunk_bytes == 0) {
            throw std::invalid_argument(
                "StreamChannel buffer limits must be positive"
            );
        }
    }

    StreamChannel(const StreamChannel&) = delete;
    StreamChannel& operator=(const StreamChannel&) = delete;

    void SetWakeupCallback(std::function<void()> wakeup) {
        std::lock_guard lock(mutex_);
        wakeup_ = std::move(wakeup);
    }

    // ========================================================
    // Producer Operations
    // ========================================================

    [[nodiscard]]
    StreamPushStatus TryPush(StreamChunk chunk) {
        if (chunk.data.size() > config_.max_chunk_bytes) {
            return StreamPushStatus::Oversized;
        }

        if (combined_cancel_.IsCancelled()) {
            Cancel();
            return StreamPushStatus::Cancelled;
        }

        std::function<void()> wakeup;
        {
            std::lock_guard lock(mutex_);

            if (state_ == StreamState::Cancelled) {
                return StreamPushStatus::Cancelled;
            }
            if (state_ != StreamState::Open) {
                return StreamPushStatus::Closed;
            }

            if (!HasCapacityLocked(chunk.data.size())) {
                ++stats_.backpressure_events;
                return StreamPushStatus::Full;
            }

            chunk.sequence = next_sequence_++;
            buffered_bytes_ += chunk.data.size();
            ++stats_.chunks_pushed;
            stats_.bytes_pushed += chunk.data.size();
            queue_.push_back(std::move(chunk));
            wakeup = wakeup_;
        }

        data_cv_.notify_one();
        if (wakeup) {
            try {
                wakeup();
            } catch (...) {
            }
        }
        return StreamPushStatus::Accepted;
    }

    [[nodiscard]]
    StreamPushStatus Push(
        StreamChunk chunk,
        std::chrono::milliseconds timeout =
            std::chrono::milliseconds{5000}
    ) {
        if (chunk.data.size() > config_.max_chunk_bytes) {
            return StreamPushStatus::Oversized;
        }

        const auto deadline =
            std::chrono::steady_clock::now() + timeout;

        std::function<void()> wakeup;
        {
            std::unique_lock lock(mutex_);

            while (true) {
                if (combined_cancel_.IsCancelled()) {
                    TransitionCancelLocked();
                    return StreamPushStatus::Cancelled;
                }
                if (state_ == StreamState::Cancelled) {
                    return StreamPushStatus::Cancelled;
                }
                if (state_ != StreamState::Open) {
                    return StreamPushStatus::Closed;
                }
                if (HasCapacityLocked(chunk.data.size())) {
                    break;
                }

                ++stats_.backpressure_events;
                if (space_cv_.wait_until(lock, deadline) ==
                    std::cv_status::timeout) {
                    if (combined_cancel_.IsCancelled()) {
                        TransitionCancelLocked();
                        return StreamPushStatus::Cancelled;
                    }
                    if (!HasCapacityLocked(chunk.data.size())) {
                        return StreamPushStatus::Full;
                    }
                }
            }

            chunk.sequence = next_sequence_++;
            buffered_bytes_ += chunk.data.size();
            ++stats_.chunks_pushed;
            stats_.bytes_pushed += chunk.data.size();
            queue_.push_back(std::move(chunk));
            wakeup = wakeup_;
        }

        data_cv_.notify_one();
        if (wakeup) {
            try {
                wakeup();
            } catch (...) {
            }
        }
        return StreamPushStatus::Accepted;
    }

    void Complete() noexcept {
        std::function<void()> wakeup;
        {
            std::lock_guard lock(mutex_);
            if (state_ != StreamState::Open) {
                return;
            }
            state_ = StreamState::Completed;
            wakeup = wakeup_;
        }
        data_cv_.notify_all();
        space_cv_.notify_all();
        if (wakeup) {
            try {
                wakeup();
            } catch (...) {
            }
        }
    }

    void Fail(bridge::OperationError err) noexcept {
        std::function<void()> wakeup;
        {
            std::lock_guard lock(mutex_);
            if (state_ != StreamState::Open) {
                return;
            }
            state_ = StreamState::Failed;
            error_ = std::move(err);
            wakeup = wakeup_;
        }
        data_cv_.notify_all();
        space_cv_.notify_all();
        if (wakeup) {
            try {
                wakeup();
            } catch (...) {
            }
        }
    }

    // ========================================================
    // Consumer / Shared Lifecycle Operations
    // ========================================================

    [[nodiscard]]
    std::optional<StreamChunk> TryPop() {
        std::optional<StreamChunk> out;
        {
            std::lock_guard lock(mutex_);
            if (queue_.empty()) {
                return std::nullopt;
            }
            out = std::move(queue_.front());
            queue_.pop_front();
            buffered_bytes_ -= out->data.size();
            ++stats_.chunks_popped;
            stats_.bytes_popped += out->data.size();
        }
        space_cv_.notify_all();
        return out;
    }

    [[nodiscard]]
    std::optional<StreamChunk> Pop(
        std::chrono::milliseconds timeout =
            std::chrono::milliseconds{5000}
    ) {
        const auto deadline =
            std::chrono::steady_clock::now() + timeout;

        std::optional<StreamChunk> out;
        {
            std::unique_lock lock(mutex_);
            data_cv_.wait_until(lock, deadline, [&]() {
                return !queue_.empty() ||
                       state_ != StreamState::Open ||
                       combined_cancel_.IsCancelled();
            });
            if (queue_.empty()) {
                return std::nullopt;
            }
            out = std::move(queue_.front());
            queue_.pop_front();
            buffered_bytes_ -= out->data.size();
            ++stats_.chunks_popped;
            stats_.bytes_popped += out->data.size();
        }
        space_cv_.notify_all();
        return out;
    }

    void Cancel() noexcept {
        std::function<void()> wakeup;
        {
            std::lock_guard lock(mutex_);
            if (state_ == StreamState::Completed ||
                state_ == StreamState::Failed ||
                state_ == StreamState::Cancelled) {
                local_cancel_.Cancel();
                return;
            }
            TransitionCancelLocked();
            wakeup = wakeup_;
        }
        data_cv_.notify_all();
        space_cv_.notify_all();
        if (wakeup) {
            try {
                wakeup();
            } catch (...) {
            }
        }
    }

    // ========================================================
    // Inspection
    // ========================================================

    [[nodiscard]]
    bool IsCancelled() const noexcept {
        if (combined_cancel_.IsCancelled()) {
            return true;
        }
        std::lock_guard lock(mutex_);
        return state_ == StreamState::Cancelled;
    }

    [[nodiscard]]
    StreamState State() const noexcept {
        std::lock_guard lock(mutex_);
        if (state_ == StreamState::Open &&
            combined_cancel_.IsCancelled()) {
            return StreamState::Cancelled;
        }
        return state_;
    }

    [[nodiscard]]
    bool IsDrainedAndClosed() const noexcept {
        std::lock_guard lock(mutex_);
        const bool terminal =
            state_ != StreamState::Open ||
            combined_cancel_.IsCancelled();
        return terminal && queue_.empty();
    }

    [[nodiscard]]
    std::optional<bridge::OperationError> Error() const {
        std::lock_guard lock(mutex_);
        return error_;
    }

    [[nodiscard]]
    StreamStats Stats() const noexcept {
        std::lock_guard lock(mutex_);
        StreamStats s = stats_;
        s.buffered_chunks = queue_.size();
        s.buffered_bytes = buffered_bytes_;
        return s;
    }

private:
    [[nodiscard]]
    bool HasCapacityLocked(std::size_t incomingBytes) const noexcept {
        if (queue_.size() >= config_.max_buffered_chunks) {
            return false;
        }
        if (queue_.empty()) {
            // Single chunk <= max_chunk_bytes is allowed even if
            // max_chunk_bytes <= max_buffered_bytes; guard against
            // config_.max_buffered_bytes < incomingBytes.
            return incomingBytes <= config_.max_buffered_bytes;
        }
        return buffered_bytes_ + incomingBytes <=
               config_.max_buffered_bytes;
    }

    void TransitionCancelLocked() noexcept {
        local_cancel_.Cancel();
        state_ = StreamState::Cancelled;
        queue_.clear();
        buffered_bytes_ = 0;
    }

    const StreamConfig config_;
    bridge::RequestCancellationToken external_cancel_;
    bridge::RequestCancellationSource local_cancel_;
    bridge::RequestCancellationToken combined_cancel_;

    mutable std::mutex mutex_;
    std::condition_variable data_cv_;
    std::condition_variable space_cv_;

    std::deque<StreamChunk> queue_;
    std::size_t buffered_bytes_ = 0;
    std::uint64_t next_sequence_ = 1;
    StreamState state_ = StreamState::Open;
    std::optional<bridge::OperationError> error_;
    std::function<void()> wakeup_;
    StreamStats stats_{};
};

} // namespace lamp::streaming
