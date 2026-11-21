#pragma once

// ============================================================
// Lamp Framework
// Phase 9 - Streaming and Real-Time
//
// File: runtime/streaming/stream_hub.hpp
//
// Purpose:
//   Thread-safe topic pub/sub hub for real-time fan-out to
//   bounded subscriber StreamChannels with automatic slow/closed
//   consumer accounting and pruning.
// ============================================================

#include "streaming/stream_channel.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lamp::streaming {

struct PublishReport final {
    std::size_t delivered = 0;
    std::size_t backpressured = 0;
    std::size_t pruned = 0;
};

class StreamHub final {
public:
    explicit StreamHub(std::size_t max_subscribers_per_topic = 1024)
        : max_subscribers_per_topic_(max_subscribers_per_topic) {}

    [[nodiscard]]
    std::shared_ptr<StreamChannel> Subscribe(
        std::string_view topic,
        StreamConfig config = {},
        bridge::RequestCancellationToken cancel = {}
    ) {
        auto channel = std::make_shared<StreamChannel>(
            config,
            std::move(cancel)
        );

        std::lock_guard lock(mutex_);
        auto& list = topics_[std::string(topic)];

        // Prune closed/cancelled weak subscribers before checking capacity
        PruneLocked(list);

        if (list.size() >= max_subscribers_per_topic_) {
            channel->Cancel();
            return channel;
        }

        list.push_back(channel);
        return channel;
    }

    PublishReport Publish(
        std::string_view topic,
        const StreamChunk& chunk
    ) {
        std::vector<std::shared_ptr<StreamChannel>> targets;
        PublishReport report;

        {
            std::lock_guard lock(mutex_);
            const auto it = topics_.find(std::string(topic));
            if (it == topics_.end()) {
                return report;
            }

            auto& list = it->second;
            for (auto vecIt = list.begin(); vecIt != list.end();) {
                auto sub = vecIt->lock();
                if (!sub || sub->State() != StreamState::Open) {
                    vecIt = list.erase(vecIt);
                    ++report.pruned;
                } else {
                    targets.push_back(std::move(sub));
                    ++vecIt;
                }
            }
            if (list.empty()) {
                topics_.erase(it);
            }
        }

        for (auto& sub : targets) {
            const auto status = sub->TryPush(chunk);
            if (status == StreamPushStatus::Accepted) {
                ++report.delivered;
            } else if (status == StreamPushStatus::Full) {
                ++report.backpressured;
            }
        }

        return report;
    }

    void CloseTopic(std::string_view topic) {
        std::vector<std::shared_ptr<StreamChannel>> targets;
        {
            std::lock_guard lock(mutex_);
            const auto it = topics_.find(std::string(topic));
            if (it == topics_.end()) {
                return;
            }
            for (const auto& weakSub : it->second) {
                if (auto sub = weakSub.lock()) {
                    targets.push_back(std::move(sub));
                }
            }
            topics_.erase(it);
        }

        for (auto& sub : targets) {
            sub->Complete();
        }
    }

    void CloseAll() {
        std::vector<std::shared_ptr<StreamChannel>> targets;
        {
            std::lock_guard lock(mutex_);
            for (auto& [topic, list] : topics_) {
                (void)topic;
                for (const auto& weakSub : list) {
                    if (auto sub = weakSub.lock()) {
                        targets.push_back(std::move(sub));
                    }
                }
            }
            topics_.clear();
        }

        for (auto& sub : targets) {
            sub->Complete();
        }
    }

    [[nodiscard]]
    std::size_t SubscriberCount(std::string_view topic) {
        std::lock_guard lock(mutex_);
        const auto it = topics_.find(std::string(topic));
        if (it == topics_.end()) {
            return 0;
        }
        PruneLocked(it->second);
        const std::size_t count = it->second.size();
        if (count == 0) {
            topics_.erase(it);
        }
        return count;
    }

private:
    static void PruneLocked(
        std::vector<std::weak_ptr<StreamChannel>>& list
    ) {
        for (auto it = list.begin(); it != list.end();) {
            auto sub = it->lock();
            if (!sub || sub->State() != StreamState::Open) {
                it = list.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::size_t max_subscribers_per_topic_;
    mutable std::mutex mutex_;
    std::unordered_map<
        std::string,
        std::vector<std::weak_ptr<StreamChannel>>
    > topics_;
};

} // namespace lamp::streaming
