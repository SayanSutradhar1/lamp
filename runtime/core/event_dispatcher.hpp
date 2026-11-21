
#pragma once

#include <concepts>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <type_traits>
#include <utility>
#include <vector>

namespace lamp::core {

// ============================================================
// Event Dispatcher
//
// Thread-safe, synchronous FIFO event dispatcher.
//
// Guarantees:
// - Events are queued in publication order.
// - Only one thread drains the queue at a time.
// - Callbacks execute without holding the queue mutex.
// - Callbacks may publish additional events.
// - Callback exceptions are isolated.
// - Subscribers are invoked in registration order.
//
// Limitations:
// - Concurrent publish() calls may return before their
//   events have been dispatched.
// - Callbacks should not destroy the dispatcher.
// - Event payloads must be copyable.
// - No dedicated worker thread is created.
// ============================================================

template <typename Event>
    requires std::copy_constructible<Event>
class EventDispatcher final {
public:
    using Callback = std::function<void(const Event&)>;

    EventDispatcher() = default;

    ~EventDispatcher() = default;

    EventDispatcher(const EventDispatcher&) = delete;

    EventDispatcher& operator=(
        const EventDispatcher&
    ) = delete;

    EventDispatcher(EventDispatcher&&) = delete;

    EventDispatcher& operator=(
        EventDispatcher&&
    ) = delete;

    // ========================================================
    // Subscription
    // ========================================================

    // Returns false when an empty callback is supplied.
    //
    // Subscriptions are append-only for this initial
    // implementation. Registration is thread-safe.

    [[nodiscard]]
    bool subscribe(Callback callback) {
        if (!callback) {
            return false;
        }

        std::lock_guard<std::mutex> lock(mutex_);

        callbacks_.push_back(std::move(callback));

        return true;
    }

    // ========================================================
    // Event Publication
    // ========================================================

    // Events are enqueued under mutex protection.
    //
    // The first publisher becomes the dispatcher.
    // Concurrent publishers append to the queue and
    // return without invoking callbacks themselves.
    //
    // This also permits callbacks to publish new events
    // without recursively dispatching them.

    void publish(Event event) {
        {
            std::lock_guard<std::mutex> lock(mutex_);

            queue_.push_back(std::move(event));

            if (dispatching_) {
                return;
            }

            dispatching_ = true;
        }

        drain();
    }

    // ========================================================
    // Inspection
    // ========================================================

    [[nodiscard]]
    std::size_t subscriberCount() const {
        std::lock_guard<std::mutex> lock(mutex_);

        return callbacks_.size();
    }

    [[nodiscard]]
    std::size_t pendingCount() const {
        std::lock_guard<std::mutex> lock(mutex_);

        return queue_.size();
    }

    [[nodiscard]]
    bool isDispatching() const {
        std::lock_guard<std::mutex> lock(mutex_);

        return dispatching_;
    }

private:
    // ========================================================
    // Queue Draining
    // ========================================================

    void drain() {
        for (;;) {
            Event event;
            std::vector<Callback> callbacks;

            {
                std::lock_guard<std::mutex> lock(mutex_);

                if (queue_.empty()) {
                    dispatching_ = false;
                    return;
                }

                event = std::move(queue_.front());
                queue_.pop_front();

                // Snapshot the subscribers so registration
                // can proceed independently of callbacks.
                callbacks = callbacks_;
            }

            for (const auto& callback : callbacks) {
                try {
                    callback(event);
                } catch (...) {
                    // A failing subscriber must not prevent
                    // other subscribers or queued events
                    // from being processed.
                }
            }
        }
    }

    // ========================================================
    // Internal State
    // ========================================================

    mutable std::mutex mutex_;

    std::deque<Event> queue_;

    std::vector<Callback> callbacks_;

    bool dispatching_ = false;
};

} // namespace lamp::core
