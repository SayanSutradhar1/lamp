
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace lamp::http {

// ============================================================
// HTTP Completion Queue
//
// Thread-safe, bounded, multi-producer/single-consumer
// completion handoff.
//
// Producers:
//   Execution workers.
//
// Consumer:
//   The HTTP event-loop owner.
//
// A completion contains an opaque request identity and a
// callback. The callback is NEVER invoked by post().
//
// The consumer explicitly invokes callbacks through drain().
//
// This queue does not:
//   - Own sockets or HTTP connections
//   - Schedule worker execution
//   - Guarantee a callback will run after shutdown
//   - Provide cancellation of already published callbacks
//   - Wake an event loop automatically
//
// The event loop must arrange to call drain() after work
// arrives, for example through its existing wake mechanism
// or a bounded polling interval.
//
// Callback exceptions propagate from drain(). Remaining
// callbacks stay queued for a subsequent drain().
//
// Destruction requires external synchronization with all
// producer and consumer threads.
// ============================================================

class HttpCompletionQueueError final
    : public std::runtime_error {
public:
    explicit HttpCompletionQueueError(const char* message)
        : std::runtime_error(message) {}
};

// Opaque identity assigned by the transport integration.
//
// The generation field allows a future connection registry
// to reject stale completions after connection reuse.
struct HttpCompletionToken final {
    std::uint64_t connectionId = 0;
    std::uint64_t generation = 0;
    std::uint64_t requestId = 0;

    [[nodiscard]]
    bool valid() const noexcept {
        return connectionId != 0 &&
               generation != 0 &&
               requestId != 0;
    }

    [[nodiscard]]
    friend bool operator==(
        const HttpCompletionToken&,
        const HttpCompletionToken&
    ) noexcept = default;
};

using HttpCompletionCallback =
    std::function<void(const HttpCompletionToken&)>;

struct HttpCompletion final {
    HttpCompletionToken token;
    HttpCompletionCallback callback;
};

enum class HttpCompletionPostResult : std::uint8_t {
    Accepted,
    Full,
    Closed,
    Invalid
};

struct HttpCompletionQueueConfig final {
    std::size_t capacity = 4096;
};

class HttpCompletionQueue final {
public:
    explicit HttpCompletionQueue(
        HttpCompletionQueueConfig config = {}
    )
        : capacity_(config.capacity) {
        if (capacity_ == 0) {
            throw HttpCompletionQueueError(
                "HTTP completion queue capacity must be positive"
            );
        }
    }

    HttpCompletionQueue(const HttpCompletionQueue&) = delete;
    HttpCompletionQueue& operator=(
        const HttpCompletionQueue&
    ) = delete;

    HttpCompletionQueue(HttpCompletionQueue&&) = delete;
    HttpCompletionQueue& operator=(
        HttpCompletionQueue&&
    ) = delete;

    // --------------------------------------------------------
    // Publish a completion
    //
    // The callback is not executed here.
    //
    // Full and Closed are distinct results so callers can
    // implement overload handling and shutdown behavior.
    //
    // A rejected completion remains owned by the caller
    // when using the lvalue overload.
    // --------------------------------------------------------

    [[nodiscard]]
    HttpCompletionPostResult post(
        const HttpCompletion& completion
    ) {
        if (!completion.token.valid() ||
            !completion.callback) {
            return HttpCompletionPostResult::Invalid;
        }

        std::lock_guard<std::mutex> lock(mutex_);

        if (closed_) {
            return HttpCompletionPostResult::Closed;
        }

        if (queue_.size() >= capacity_) {
            return HttpCompletionPostResult::Full;
        }

        queue_.push_back(completion);
        return HttpCompletionPostResult::Accepted;
    }

    [[nodiscard]]
    HttpCompletionPostResult post(
        HttpCompletion&& completion
    ) {
        if (!completion.token.valid() ||
            !completion.callback) {
            return HttpCompletionPostResult::Invalid;
        }

        std::lock_guard<std::mutex> lock(mutex_);

        if (closed_) {
            return HttpCompletionPostResult::Closed;
        }

        if (queue_.size() >= capacity_) {
            return HttpCompletionPostResult::Full;
        }

        queue_.push_back(std::move(completion));
        return HttpCompletionPostResult::Accepted;
    }

    // --------------------------------------------------------
    // Extract one completion
    //
    // This method transfers ownership of the callback to
    // the caller. It does not execute the callback.
    // --------------------------------------------------------

    [[nodiscard]]
    std::optional<HttpCompletion> tryPop() {
        std::lock_guard<std::mutex> lock(mutex_);

        if (queue_.empty()) {
            return std::nullopt;
        }

        HttpCompletion completion =
            std::move(queue_.front());

        queue_.pop_front();

        return completion;
    }

    // --------------------------------------------------------
    // Execute up to maxCount completions
    //
    // Intended to be called by the HTTP event-loop owner.
    //
    // No queue mutex is held during callback execution.
    //
    // IMPORTANT:
    // The caller must enforce single-consumer ownership.
    // Concurrent drain() calls would violate the intended
    // event-loop callback-affinity contract.
    //
    // If a callback throws, the exception propagates.
    // That completion has already been removed.
    // Remaining completions stay queued.
    // --------------------------------------------------------

    std::size_t drain(
        std::size_t maxCount =
            std::numeric_limits<std::size_t>::max()
    ) {
        std::size_t executed = 0;

        while (executed < maxCount) {
            auto completion = tryPop();

            if (!completion.has_value()) {
                break;
            }

            // Count before invoking user code so the
            // callback cannot cause accidental re-execution.
            ++executed;

            completion->callback(completion->token);
        }

        return executed;
    }

    // --------------------------------------------------------
    // Close the queue
    //
    // Prevents new posts.
    //
    // Existing completions remain available for draining.
    //
    // close() is idempotent.
    // --------------------------------------------------------

    void close() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
    }

    [[nodiscard]]
    bool closed() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

    [[nodiscard]]
    std::size_t size() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    [[nodiscard]]
    bool empty() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

    [[nodiscard]]
    std::size_t capacity() const noexcept {
        return capacity_;
    }

    // --------------------------------------------------------
    // Discard queued completions
    //
    // Use only when callbacks must no longer be executed.
    // This does not cancel work running in execution workers.
    //
    // Destructors of discarded callbacks may run while the
    // queue mutex is held; callback capture destructors
    // must not re-enter this queue.
    // --------------------------------------------------------

    std::size_t discardPending() {
        std::lock_guard<std::mutex> lock(mutex_);

        const auto count = queue_.size();
        queue_.clear();

        return count;
    }

private:
    const std::size_t capacity_;

    mutable std::mutex mutex_;
    std::deque<HttpCompletion> queue_;

    bool closed_ = false;
};

} // namespace lamp::http
