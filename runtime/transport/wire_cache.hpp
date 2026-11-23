#pragma once

// ============================================================
// Lamp Framework
// Native Binary Transport Layer - Full Request-Response Cycle
// Wire Cache (With TTL & Enforced Upper TTL Limit)
//
// File: runtime/transport/wire_cache.hpp
//
// Purpose:
//   Caches the complete serialized binary response payload
//   for an entire request-response cycle (keyed by operation
//   and exact raw binary request payload), clamping any TTL
//   against a hard upper bound (`max_ttl_limit`).
// ============================================================

#include "security/crypto_utils.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lamp::transport {

struct FullCycleWireCacheConfig final {
    std::size_t max_bytes = 16U * 1024U * 1024U; // compact L1 wire cache
    std::size_t max_entries = 4096;
    std::size_t shard_count = 16;
    std::chrono::milliseconds max_ttl_limit{300'000}; // hard upper limit = 5 min
};

class FullCycleWireCache final {
public:
    using Clock = std::chrono::steady_clock;
    using ClockFn = std::function<Clock::time_point()>;

    explicit FullCycleWireCache(
        FullCycleWireCacheConfig config = {},
        ClockFn clock = [] { return Clock::now(); }
    )
        : config_(SanitizeConfig(config)),
          clock_(std::move(clock)) {
        shards_.reserve(config_.shard_count);
        for (std::size_t i = 0; i < config_.shard_count; ++i) {
            shards_.push_back(std::make_unique<Shard>());
        }
    }

    FullCycleWireCache(const FullCycleWireCache&) = delete;
    FullCycleWireCache& operator=(const FullCycleWireCache&) = delete;

    [[nodiscard]]
    static std::string BuildRequestKey(
        std::string_view operation_id,
        std::span<const std::uint8_t> raw_request_payload
    ) {
        const auto digest = security::Sha256(raw_request_payload);
        std::string key;
        key.reserve(operation_id.size() + 65U);
        key.append(operation_id).push_back('|');
        key.append(security::ToHexLower(digest));
        return key;
    }

    [[nodiscard]]
    std::chrono::milliseconds ClampTtl(
        std::chrono::milliseconds requested_ttl
    ) const noexcept {
        if (requested_ttl.count() <= 0) {
            return std::chrono::milliseconds{0};
        }
        return (std::min)(requested_ttl, config_.max_ttl_limit);
    }

    [[nodiscard]]
    std::optional<std::vector<std::uint8_t>> Lookup(
        std::string_view operation_id,
        std::span<const std::uint8_t> raw_request_payload
    ) {
        const std::string key = BuildRequestKey(operation_id, raw_request_payload);
        Shard& shard = ShardFor(key);
        const auto now = clock_();

        std::lock_guard lock(shard.mutex);
        const auto it = shard.map.find(key);
        if (it == shard.map.end()) {
            misses_.fetch_add(1, std::memory_order_relaxed);
            return std::nullopt;
        }

        if (now >= it->second.expires_at) {
            EraseLocked(shard, it);
            misses_.fetch_add(1, std::memory_order_relaxed);
            return std::nullopt;
        }

        shard.lru.splice(shard.lru.begin(), shard.lru, it->second.lru_it);
        hits_.fetch_add(1, std::memory_order_relaxed);
        return it->second.response_bytes;
    }

    // Returns effective clamped TTL stored (zero if not stored).
    std::chrono::milliseconds Store(
        std::string_view operation_id,
        std::span<const std::uint8_t> raw_request_payload,
        std::vector<std::uint8_t> response_bytes,
        std::chrono::milliseconds requested_ttl
    ) {
        const auto effective_ttl = ClampTtl(requested_ttl);
        if (effective_ttl.count() <= 0) {
            return std::chrono::milliseconds{0};
        }

        const std::size_t per_shard_bytes =
            (std::max)(std::size_t{1024}, config_.max_bytes / config_.shard_count);
        const std::size_t per_shard_entries =
            (std::max)(std::size_t{1}, config_.max_entries / config_.shard_count);

        if (response_bytes.size() > per_shard_bytes) {
            return std::chrono::milliseconds{0};
        }

        const std::string key = BuildRequestKey(operation_id, raw_request_payload);
        Shard& shard = ShardFor(key);
        const auto expires_at = clock_() + effective_ttl;

        std::lock_guard lock(shard.mutex);
        if (const auto existing = shard.map.find(key); existing != shard.map.end()) {
            EraseLocked(shard, existing);
        }

        while ((!shard.lru.empty()) &&
               (shard.map.size() >= per_shard_entries ||
                shard.bytes + response_bytes.size() > per_shard_bytes)) {
            const std::string& victim_key = shard.lru.back();
            const auto vit = shard.map.find(victim_key);
            if (vit != shard.map.end()) {
                EraseLocked(shard, vit);
            } else {
                shard.lru.pop_back();
            }
        }

        shard.lru.push_front(key);
        Entry entry;
        entry.bytes_size = response_bytes.size();
        entry.response_bytes = std::move(response_bytes);
        entry.expires_at = expires_at;
        entry.lru_it = shard.lru.begin();

        shard.bytes += entry.bytes_size;
        shard.map.emplace(key, std::move(entry));
        return effective_ttl;
    }

    void InvalidateAll() {
        for (auto& shard : shards_) {
            std::lock_guard lock(shard->mutex);
            shard->map.clear();
            shard->lru.clear();
            shard->bytes = 0;
        }
    }

    [[nodiscard]]
    std::uint64_t Hits() const noexcept {
        return hits_.load(std::memory_order_relaxed);
    }

    [[nodiscard]]
    std::uint64_t Misses() const noexcept {
        return misses_.load(std::memory_order_relaxed);
    }

    [[nodiscard]]
    std::chrono::milliseconds MaxTtlLimit() const noexcept {
        return config_.max_ttl_limit;
    }

private:
    struct Entry final {
        std::vector<std::uint8_t> response_bytes;
        std::size_t bytes_size = 0;
        Clock::time_point expires_at{};
        std::list<std::string>::iterator lru_it;
    };

    struct Shard final {
        mutable std::mutex mutex;
        std::list<std::string> lru;
        std::unordered_map<std::string, Entry> map;
        std::size_t bytes = 0;
    };

    [[nodiscard]]
    static FullCycleWireCacheConfig SanitizeConfig(FullCycleWireCacheConfig c) noexcept {
        if (c.max_bytes < 4096U) c.max_bytes = 4096U;
        if (c.max_entries == 0) c.max_entries = 1;
        if (c.shard_count == 0) c.shard_count = 1;
        if (c.max_ttl_limit.count() <= 0) {
            c.max_ttl_limit = std::chrono::milliseconds{60'000};
        }
        return c;
    }

    Shard& ShardFor(std::string_view key) const {
        const std::size_t idx = std::hash<std::string_view>{}(key) % shards_.size();
        return *shards_[idx];
    }

    static void EraseLocked(
        Shard& shard,
        std::unordered_map<std::string, Entry>::iterator it
    ) {
        shard.bytes -= it->second.bytes_size;
        shard.lru.erase(it->second.lru_it);
        shard.map.erase(it);
    }

    FullCycleWireCacheConfig config_;
    ClockFn clock_;
    std::vector<std::unique_ptr<Shard>> shards_;
    std::atomic<std::uint64_t> hits_{0};
    std::atomic<std::uint64_t> misses_{0};
};

} // namespace lamp::transport
