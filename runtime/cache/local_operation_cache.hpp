#pragma once

// ============================================================
// Lamp Framework
// Phase 8 - Caching and Performance
//
// File: runtime/cache/local_operation_cache.hpp
//
// Purpose:
//   Thread-safe, bounded, sharded LRU + TTL in-memory cache
//   implementing bridge::IOperationCache with
//   stale-while-revalidate and tag/operation invalidation.
// ============================================================

#include "bridge/operation_cache_interface.hpp"
#include "cache/cache_key.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lamp::cache {

struct LocalCacheConfig final {
    std::size_t max_entries = 10000;
    std::size_t max_total_bytes = 64U * 1024U * 1024U;
    std::size_t max_entry_bytes = 4U * 1024U * 1024U;
    std::chrono::milliseconds stale_grace_period{60000};
    std::size_t shard_count = 16;
    std::vector<std::string> metadata_key_fields;
};

struct CacheStats final {
    std::uint64_t hits = 0;
    std::uint64_t shared_hits = 0;
    std::uint64_t stale_hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t stores = 0;
    std::uint64_t evictions = 0;
    std::uint64_t expirations = 0;
    std::uint64_t invalidations = 0;
    std::uint64_t rejected_oversized = 0;
    std::size_t entry_count = 0;
    std::size_t total_bytes = 0;
};

class LocalOperationCache final : public bridge::IOperationCache {
public:
    using Clock = std::chrono::steady_clock;
    using ClockFn = std::function<Clock::time_point()>;

    explicit LocalOperationCache(
        LocalCacheConfig config = {},
        ClockFn clock = {}
    )
        : config_(std::move(config)),
          clock_(clock ? std::move(clock) : []() { return Clock::now(); }) {
        if (config_.max_entries == 0 ||
            config_.max_total_bytes == 0 ||
            config_.max_entry_bytes == 0) {
            throw std::invalid_argument(
                "LocalOperationCache capacity limits must be positive"
            );
        }
        if (config_.shard_count == 0) {
            config_.shard_count = 1;
        }

        const std::size_t effectiveShards =
            config_.max_entries < config_.shard_count
                ? 1
                : config_.shard_count;

        const std::size_t perShardEntries =
            std::max<std::size_t>(
                1,
                config_.max_entries / effectiveShards
            );
        const std::size_t perShardBytes =
            std::max<std::size_t>(
                config_.max_entry_bytes,
                config_.max_total_bytes / effectiveShards
            );

        shards_.reserve(effectiveShards);
        for (std::size_t i = 0; i < effectiveShards; ++i) {
            shards_.push_back(
                std::make_unique<Shard>(perShardEntries, perShardBytes)
            );
        }
    }

    LocalOperationCache(const LocalOperationCache&) = delete;
    LocalOperationCache& operator=(const LocalOperationCache&) = delete;

    // ========================================================
    // bridge::IOperationCache
    // ========================================================

    [[nodiscard]]
    bridge::OperationCacheLookup Lookup(
        const bridge::OperationDescriptor& descriptor,
        const bridge::RequestEnvelope& request
    ) override {
        if (!descriptor.IsCacheEligible()) {
            return {};
        }

        const CacheKey key = CacheKeyBuilder::Build(
            descriptor,
            request,
            config_.metadata_key_fields
        );
        return LookupByKey(
            key,
            descriptor.cache.stale_while_revalidate
        );
    }

    void Store(
        const bridge::OperationDescriptor& descriptor,
        const bridge::RequestEnvelope& request,
        const bridge::ResultEnvelope& result
    ) override {
        if (!descriptor.IsCacheEligible() || !result.IsSuccess()) {
            return;
        }
        if (descriptor.cache.ttl.count() <= 0) {
            return;
        }

        const CacheKey key = CacheKeyBuilder::Build(
            descriptor,
            request,
            config_.metadata_key_fields
        );

        const std::size_t maxEntryLimit =
            descriptor.cache.max_entry_bytes > 0
                ? std::min(
                      descriptor.cache.max_entry_bytes,
                      config_.max_entry_bytes
                  )
                : config_.max_entry_bytes;

        StoreByKey(
            key,
            result,
            descriptor.cache.ttl,
            maxEntryLimit,
            {}
        );
    }

    void InvalidateOperation(
        const bridge::OperationId& operation_id
    ) override {
        std::uint64_t removedCount = 0;

        for (auto& shard : shards_) {
            std::lock_guard lock(shard->mutex);
            for (auto it = shard->lru.begin();
                 it != shard->lru.end();) {
                if (it->operation_id == operation_id) {
                    const auto mapIt = shard->entries.find(*it);
                    if (mapIt != shard->entries.end()) {
                        shard->total_bytes -= mapIt->second.byte_size;
                        shard->entries.erase(mapIt);
                        ++removedCount;
                    }
                    it = shard->lru.erase(it);
                } else {
                    ++it;
                }
            }
        }

        if (removedCount > 0) {
            invalidations_.fetch_add(
                removedCount,
                std::memory_order_relaxed
            );
        }
    }

    // ========================================================
    // Direct Cache Controls
    // ========================================================

    [[nodiscard]]
    bridge::OperationCacheLookup LookupByKey(
        const CacheKey& key,
        bool allow_stale_while_revalidate = false
    ) {
        auto& shard = ShardFor(key);
        const auto now = clock_();

        std::lock_guard lock(shard.mutex);

        const auto it = shard.entries.find(key);
        if (it == shard.entries.end()) {
            misses_.fetch_add(1, std::memory_order_relaxed);
            return {};
        }

        Entry& entry = it->second;

        if (now < entry.expires_at) {
            // Fresh hit -> promote to front of LRU
            shard.lru.splice(
                shard.lru.begin(),
                shard.lru,
                entry.lru_it
            );
            hits_.fetch_add(1, std::memory_order_relaxed);
            return {
                bridge::CacheResult::HitLocal,
                entry.result,
                false
            };
        }

        // Expired by TTL
        if (allow_stale_while_revalidate &&
            now < entry.stale_until) {
            shard.lru.splice(
                shard.lru.begin(),
                shard.lru,
                entry.lru_it
            );
            bool triggerRevalidation = false;
            if (!entry.revalidating) {
                entry.revalidating = true;
                triggerRevalidation = true;
            }
            stale_hits_.fetch_add(1, std::memory_order_relaxed);
            return {
                bridge::CacheResult::Stale,
                entry.result,
                triggerRevalidation
            };
        }

        // Hard expired -> evict
        shard.total_bytes -= entry.byte_size;
        shard.lru.erase(entry.lru_it);
        shard.entries.erase(it);
        expirations_.fetch_add(1, std::memory_order_relaxed);
        misses_.fetch_add(1, std::memory_order_relaxed);
        return {};
    }

    void StoreByKey(
        const CacheKey& key,
        const bridge::ResultEnvelope& result,
        std::chrono::milliseconds ttl,
        std::size_t max_entry_bytes = 0,
        std::vector<std::string> tags = {}
    ) {
        if (ttl.count() <= 0 || !result.IsSuccess()) {
            return;
        }

        const std::size_t effectiveLimit =
            max_entry_bytes > 0
                ? std::min(max_entry_bytes, config_.max_entry_bytes)
                : config_.max_entry_bytes;

        const std::size_t entryBytes =
            EstimateEntryBytes(key, result);

        if (entryBytes > effectiveLimit) {
            rejected_oversized_.fetch_add(
                1,
                std::memory_order_relaxed
            );
            return;
        }

        auto& shard = ShardFor(key);
        const auto now = clock_();
        const auto expiresAt = now + ttl;
        const auto staleUntil =
            expiresAt + config_.stale_grace_period;

        std::lock_guard lock(shard.mutex);

        if (entryBytes > shard.max_bytes) {
            rejected_oversized_.fetch_add(
                1,
                std::memory_order_relaxed
            );
            return;
        }

        const auto existing = shard.entries.find(key);
        if (existing != shard.entries.end()) {
            shard.total_bytes -= existing->second.byte_size;
            existing->second.result = result;
            existing->second.byte_size = entryBytes;
            existing->second.expires_at = expiresAt;
            existing->second.stale_until = staleUntil;
            existing->second.revalidating = false;
            existing->second.tags = std::move(tags);
            shard.total_bytes += entryBytes;
            shard.lru.splice(
                shard.lru.begin(),
                shard.lru,
                existing->second.lru_it
            );
        } else {
            while (!shard.lru.empty() &&
                   (shard.entries.size() >= shard.max_entries ||
                    shard.total_bytes + entryBytes >
                        shard.max_bytes)) {
                EvictTailLocked(shard);
            }

            shard.lru.push_front(key);
            Entry entry;
            entry.result = result;
            entry.byte_size = entryBytes;
            entry.expires_at = expiresAt;
            entry.stale_until = staleUntil;
            entry.revalidating = false;
            entry.tags = std::move(tags);
            entry.lru_it = shard.lru.begin();

            shard.entries.emplace(key, std::move(entry));
            shard.total_bytes += entryBytes;
        }

        // Enforce byte limit even after an in-place update grew
        while (shard.total_bytes > shard.max_bytes &&
               shard.lru.size() > 1) {
            EvictTailLocked(shard);
        }

        stores_.fetch_add(1, std::memory_order_relaxed);
    }

    bool InvalidateKey(const CacheKey& key) {
        auto& shard = ShardFor(key);
        std::lock_guard lock(shard.mutex);

        const auto it = shard.entries.find(key);
        if (it == shard.entries.end()) {
            return false;
        }

        shard.total_bytes -= it->second.byte_size;
        shard.lru.erase(it->second.lru_it);
        shard.entries.erase(it);
        invalidations_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    std::size_t InvalidateByTag(std::string_view tag) {
        std::size_t removed = 0;

        for (auto& shard : shards_) {
            std::lock_guard lock(shard->mutex);
            for (auto it = shard->entries.begin();
                 it != shard->entries.end();) {
                const auto& tags = it->second.tags;
                if (std::find(tags.begin(), tags.end(), tag) !=
                    tags.end()) {
                    shard->total_bytes -= it->second.byte_size;
                    shard->lru.erase(it->second.lru_it);
                    it = shard->entries.erase(it);
                    ++removed;
                } else {
                    ++it;
                }
            }
        }

        if (removed > 0) {
            invalidations_.fetch_add(
                removed,
                std::memory_order_relaxed
            );
        }
        return removed;
    }

    std::size_t PurgeExpired(bool include_stale = true) {
        const auto now = clock_();
        std::size_t purged = 0;

        for (auto& shard : shards_) {
            std::lock_guard lock(shard->mutex);
            for (auto it = shard->entries.begin();
                 it != shard->entries.end();) {
                const auto cutoff = include_stale
                    ? it->second.expires_at
                    : it->second.stale_until;
                if (now >= cutoff) {
                    shard->total_bytes -= it->second.byte_size;
                    shard->lru.erase(it->second.lru_it);
                    it = shard->entries.erase(it);
                    ++purged;
                } else {
                    ++it;
                }
            }
        }

        if (purged > 0) {
            expirations_.fetch_add(
                purged,
                std::memory_order_relaxed
            );
        }
        return purged;
    }

    void Clear() {
        for (auto& shard : shards_) {
            std::lock_guard lock(shard->mutex);
            shard->entries.clear();
            shard->lru.clear();
            shard->total_bytes = 0;
        }
    }

    [[nodiscard]]
    CacheStats Stats() const {
        CacheStats s;
        s.hits = hits_.load(std::memory_order_relaxed);
        s.shared_hits = shared_hits_.load(std::memory_order_relaxed);
        s.stale_hits = stale_hits_.load(std::memory_order_relaxed);
        s.misses = misses_.load(std::memory_order_relaxed);
        s.stores = stores_.load(std::memory_order_relaxed);
        s.evictions = evictions_.load(std::memory_order_relaxed);
        s.expirations = expirations_.load(std::memory_order_relaxed);
        s.invalidations =
            invalidations_.load(std::memory_order_relaxed);
        s.rejected_oversized =
            rejected_oversized_.load(std::memory_order_relaxed);

        for (const auto& shard : shards_) {
            std::lock_guard lock(shard->mutex);
            s.entry_count += shard->entries.size();
            s.total_bytes += shard->total_bytes;
        }
        return s;
    }

private:
    struct Entry final {
        bridge::ResultEnvelope result;
        std::size_t byte_size = 0;
        Clock::time_point expires_at{};
        Clock::time_point stale_until{};
        bool revalidating = false;
        std::vector<std::string> tags;
        std::list<CacheKey>::iterator lru_it;
    };

    struct Shard final {
        Shard(std::size_t maxE, std::size_t maxB)
            : max_entries(maxE), max_bytes(maxB) {}

        mutable std::mutex mutex;
        std::size_t max_entries;
        std::size_t max_bytes;
        std::size_t total_bytes = 0;
        std::list<CacheKey> lru;
        std::unordered_map<CacheKey, Entry, CacheKeyHash> entries;
    };

    [[nodiscard]]
    Shard& ShardFor(const CacheKey& key) noexcept {
        return *shards_[key.hash % shards_.size()];
    }

    void EvictTailLocked(Shard& shard) {
        if (shard.lru.empty()) {
            return;
        }
        const CacheKey victim = shard.lru.back();
        const auto it = shard.entries.find(victim);
        if (it != shard.entries.end()) {
            shard.total_bytes -= it->second.byte_size;
            shard.entries.erase(it);
        }
        shard.lru.pop_back();
        evictions_.fetch_add(1, std::memory_order_relaxed);
    }

    [[nodiscard]]
    static std::size_t EstimateEntryBytes(
        const CacheKey& key,
        const bridge::ResultEnvelope& result
    ) noexcept {
        std::size_t total =
            key.bytes.size() +
            result.payload.size() +
            result.operation_id.size() +
            128;
        for (const auto& m : result.metadata) {
            total += m.key.size() + m.value.size();
        }
        return total;
    }

    LocalCacheConfig config_;
    ClockFn clock_;
    std::vector<std::unique_ptr<Shard>> shards_;

    std::atomic<std::uint64_t> hits_{0};
    std::atomic<std::uint64_t> shared_hits_{0};
    std::atomic<std::uint64_t> stale_hits_{0};
    std::atomic<std::uint64_t> misses_{0};
    std::atomic<std::uint64_t> stores_{0};
    std::atomic<std::uint64_t> evictions_{0};
    std::atomic<std::uint64_t> expirations_{0};
    std::atomic<std::uint64_t> invalidations_{0};
    std::atomic<std::uint64_t> rejected_oversized_{0};
};

} // namespace lamp::cache
