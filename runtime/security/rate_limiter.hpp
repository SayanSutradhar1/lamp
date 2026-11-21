#pragma once

// ============================================================
// Lamp Framework
// Phase 10 - Security and Reliability
//
// File: runtime/security/rate_limiter.hpp
//
// Purpose:
//   Sharded token-bucket rate limiting with bounded state
//   tracking and per-operation resilience circuit breakers.
// ============================================================

#include "bridge/operation_descriptor.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lamp::security {

// ============================================================
// Token-Bucket Rate Limiter
// ============================================================

struct RateLimitPolicy final {
    double capacity = 60.0;
    double refill_per_second = 10.0;
    std::size_t max_tracked_keys = 16384;
    std::size_t shard_count = 32;
};

struct RateLimitDecision final {
    bool allowed = true;
    std::uint64_t remaining_tokens = 0;
    std::uint64_t limit = 0;
    std::chrono::milliseconds retry_after{0};
};

class TokenBucketRateLimiter final {
public:
    using Clock = std::chrono::steady_clock;
    using ClockFn = std::function<Clock::time_point()>;

    explicit TokenBucketRateLimiter(
        RateLimitPolicy default_policy = {},
        ClockFn clock = [] { return Clock::now(); }
    )
        : default_policy_(SanitizePolicy(default_policy)),
          clock_(std::move(clock)) {
        const std::size_t count = default_policy_.shard_count;
        shards_.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            shards_.push_back(std::make_unique<Shard>());
        }
    }

    TokenBucketRateLimiter(const TokenBucketRateLimiter&) = delete;
    TokenBucketRateLimiter& operator=(const TokenBucketRateLimiter&) = delete;

    void SetOperationPolicy(
        std::string operation_id,
        RateLimitPolicy policy
    ) {
        std::unique_lock lock(policy_mutex_);
        operation_policies_[std::move(operation_id)] = SanitizePolicy(policy);
    }

    bool RemoveOperationPolicy(const std::string& operation_id) {
        std::unique_lock lock(policy_mutex_);
        return operation_policies_.erase(operation_id) > 0;
    }

    [[nodiscard]]
    RateLimitDecision TryAcquire(
        std::string_view subject_key,
        std::string_view operation_id = {},
        double cost = 1.0
    ) {
        if (cost <= 0.0) {
            cost = 1.0;
        }

        const RateLimitPolicy policy = EffectivePolicy(operation_id);

        std::string composite;
        composite.reserve(operation_id.size() + subject_key.size() + 1U);
        composite.append(operation_id);
        composite.push_back('|');
        composite.append(subject_key);

        const std::size_t hash_val = std::hash<std::string_view>{}(composite);
        Shard& shard = *shards_[hash_val % shards_.size()];
        const std::size_t per_shard_cap = (std::max)(
            std::size_t{1},
            (policy.max_tracked_keys + shards_.size() - 1U) / shards_.size()
        );

        const auto now = clock_();
        std::lock_guard lock(shard.mutex);

        auto it = shard.entries.find(composite);
        if (it == shard.entries.end()) {
            // Bounded LRU eviction prevents memory exhaustion under key-flooding.
            while (shard.entries.size() >= per_shard_cap && !shard.lru.empty()) {
                const std::string& oldest_key = shard.lru.back();
                shard.entries.erase(oldest_key);
                shard.lru.pop_back();
            }

            shard.lru.push_front(composite);
            BucketState state;
            state.tokens = policy.capacity;
            state.last_refill = now;
            state.lru_it = shard.lru.begin();
            it = shard.entries.emplace(std::move(composite), std::move(state)).first;
        } else {
            shard.lru.splice(shard.lru.begin(), shard.lru, it->second.lru_it);
            const auto elapsed = now > it->second.last_refill
                ? std::chrono::duration<double>(now - it->second.last_refill).count()
                : 0.0;
            it->second.tokens = (std::min)(
                policy.capacity,
                it->second.tokens + elapsed * policy.refill_per_second
            );
            it->second.last_refill = now;
        }

        RateLimitDecision decision;
        decision.limit = static_cast<std::uint64_t>(std::floor(policy.capacity));

        if (it->second.tokens >= cost) {
            it->second.tokens -= cost;
            decision.allowed = true;
            decision.remaining_tokens =
                static_cast<std::uint64_t>(std::floor(it->second.tokens));
            decision.retry_after = std::chrono::milliseconds{0};
            return decision;
        }

        decision.allowed = false;
        decision.remaining_tokens =
            static_cast<std::uint64_t>(std::floor((std::max)(0.0, it->second.tokens)));
        if (policy.refill_per_second > 0.0) {
            const double deficit = cost - it->second.tokens;
            const double wait_ms = std::ceil((deficit / policy.refill_per_second) * 1000.0);
            decision.retry_after = std::chrono::milliseconds{
                static_cast<std::int64_t>((std::max)(1.0, wait_ms))
            };
        } else {
            decision.retry_after = std::chrono::milliseconds{1000};
        }

        return decision;
    }

    [[nodiscard]]
    std::size_t TrackedKeyCount() const {
        std::size_t total = 0;
        for (const auto& shard : shards_) {
            std::lock_guard lock(shard->mutex);
            total += shard->entries.size();
        }
        return total;
    }

    void Clear() {
        for (auto& shard : shards_) {
            std::lock_guard lock(shard->mutex);
            shard->entries.clear();
            shard->lru.clear();
        }
    }

private:
    struct BucketState final {
        double tokens = 0.0;
        Clock::time_point last_refill{};
        std::list<std::string>::iterator lru_it;
    };

    struct Shard final {
        mutable std::mutex mutex;
        std::list<std::string> lru;
        std::unordered_map<std::string, BucketState> entries;
    };

    [[nodiscard]]
    static RateLimitPolicy SanitizePolicy(RateLimitPolicy p) noexcept {
        if (p.capacity < 1.0) p.capacity = 1.0;
        if (p.refill_per_second < 0.0) p.refill_per_second = 0.0;
        if (p.max_tracked_keys == 0) p.max_tracked_keys = 1;
        if (p.shard_count == 0) p.shard_count = 1;
        return p;
    }

    [[nodiscard]]
    RateLimitPolicy EffectivePolicy(std::string_view operation_id) const {
        if (operation_id.empty()) {
            return default_policy_;
        }
        std::shared_lock lock(policy_mutex_);
        const auto it = operation_policies_.find(std::string(operation_id));
        if (it != operation_policies_.end()) {
            return it->second;
        }
        return default_policy_;
    }

    RateLimitPolicy default_policy_;
    ClockFn clock_;
    mutable std::shared_mutex policy_mutex_;
    std::unordered_map<std::string, RateLimitPolicy> operation_policies_;
    std::vector<std::unique_ptr<Shard>> shards_;
};

// ============================================================
// Resilience Circuit Breaker
// ============================================================

enum class CircuitState : std::uint8_t {
    Closed = 0,
    Open = 1,
    HalfOpen = 2
};

struct CircuitBreakerConfig final {
    std::uint32_t failure_threshold = 5;
    std::chrono::milliseconds open_duration{5000};
    std::uint32_t half_open_max_probes = 1;
    std::uint32_t success_threshold = 2;
};

class CircuitBreaker final {
public:
    using Clock = std::chrono::steady_clock;
    using ClockFn = std::function<Clock::time_point()>;

    explicit CircuitBreaker(
        CircuitBreakerConfig config = {},
        ClockFn clock = [] { return Clock::now(); }
    )
        : config_(SanitizeConfig(config)),
          clock_(std::move(clock)) {}

    [[nodiscard]]
    bool TryAcquire() {
        const auto now = clock_();
        std::lock_guard lock(mutex_);

        if (state_ == CircuitState::Closed) {
            return true;
        }

        if (state_ == CircuitState::Open) {
            if (now - opened_at_ >= config_.open_duration) {
                state_ = CircuitState::HalfOpen;
                half_open_in_flight_ = 1;
                half_open_successes_ = 0;
                return true;
            }
            return false;
        }

        // HalfOpen: allow limited trial probes.
        if (half_open_in_flight_ < config_.half_open_max_probes) {
            ++half_open_in_flight_;
            return true;
        }
        return false;
    }

    void RecordSuccess() {
        std::lock_guard lock(mutex_);
        if (state_ == CircuitState::Closed) {
            consecutive_failures_ = 0;
            return;
        }

        if (state_ == CircuitState::HalfOpen) {
            if (half_open_in_flight_ > 0) {
                --half_open_in_flight_;
            }
            ++half_open_successes_;
            if (half_open_successes_ >= config_.success_threshold) {
                state_ = CircuitState::Closed;
                consecutive_failures_ = 0;
                half_open_successes_ = 0;
                half_open_in_flight_ = 0;
            }
        }
    }

    void RecordFailure() {
        const auto now = clock_();
        std::lock_guard lock(mutex_);

        if (state_ == CircuitState::HalfOpen) {
            state_ = CircuitState::Open;
            opened_at_ = now;
            half_open_in_flight_ = 0;
            half_open_successes_ = 0;
            consecutive_failures_ = config_.failure_threshold;
            return;
        }

        if (state_ == CircuitState::Closed) {
            ++consecutive_failures_;
            if (consecutive_failures_ >= config_.failure_threshold) {
                state_ = CircuitState::Open;
                opened_at_ = now;
            }
        }
    }

    [[nodiscard]]
    CircuitState State() const {
        const auto now = clock_();
        std::lock_guard lock(mutex_);
        if (state_ == CircuitState::Open &&
            now - opened_at_ >= config_.open_duration) {
            return CircuitState::HalfOpen;
        }
        return state_;
    }

    void Reset() {
        std::lock_guard lock(mutex_);
        state_ = CircuitState::Closed;
        consecutive_failures_ = 0;
        half_open_successes_ = 0;
        half_open_in_flight_ = 0;
    }

private:
    [[nodiscard]]
    static CircuitBreakerConfig SanitizeConfig(CircuitBreakerConfig c) noexcept {
        if (c.failure_threshold == 0) c.failure_threshold = 1;
        if (c.half_open_max_probes == 0) c.half_open_max_probes = 1;
        if (c.success_threshold == 0) c.success_threshold = 1;
        return c;
    }

    CircuitBreakerConfig config_;
    ClockFn clock_;
    mutable std::mutex mutex_;
    CircuitState state_ = CircuitState::Closed;
    std::uint32_t consecutive_failures_ = 0;
    std::uint32_t half_open_successes_ = 0;
    std::uint32_t half_open_in_flight_ = 0;
    Clock::time_point opened_at_{};
};

class CircuitBreakerRegistry final {
public:
    using ClockFn = CircuitBreaker::ClockFn;

    explicit CircuitBreakerRegistry(
        CircuitBreakerConfig default_config = {},
        ClockFn clock = [] { return CircuitBreaker::Clock::now(); }
    )
        : default_config_(default_config),
          clock_(std::move(clock)) {}

    [[nodiscard]]
    std::shared_ptr<CircuitBreaker> GetOrCreate(
        const std::string& operation_id
    ) {
        {
            std::shared_lock lock(mutex_);
            if (const auto it = breakers_.find(operation_id);
                it != breakers_.end()) {
                return it->second;
            }
        }

        std::unique_lock lock(mutex_);
        auto& slot = breakers_[operation_id];
        if (!slot) {
            slot = std::make_shared<CircuitBreaker>(default_config_, clock_);
        }
        return slot;
    }

private:
    CircuitBreakerConfig default_config_;
    ClockFn clock_;
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<CircuitBreaker>> breakers_;
};

} // namespace lamp::security
