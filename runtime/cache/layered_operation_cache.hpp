#pragma once

// ============================================================
// Lamp Framework
// Phase 8 - Caching and Performance
//
// File: runtime/cache/layered_operation_cache.hpp
//
// Purpose:
//   L1 (LocalOperationCache) + optional L2 (ISharedCacheBackend)
//   layered cache implementing bridge::IOperationCache.
// ============================================================

#include "bridge/operation_cache_interface.hpp"
#include "cache/cache_key.hpp"
#include "cache/local_operation_cache.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lamp::cache {

struct SharedCacheEntry final {
    bridge::ResultEnvelope result;
    std::chrono::milliseconds remaining_ttl{0};
};

class ISharedCacheBackend {
public:
    virtual ~ISharedCacheBackend() = default;

    [[nodiscard]]
    virtual std::optional<SharedCacheEntry> Get(
        const CacheKey& key
    ) = 0;

    virtual void Put(
        const CacheKey& key,
        const bridge::ResultEnvelope& result,
        std::chrono::milliseconds ttl
    ) = 0;

    virtual void InvalidateKey(const CacheKey& key) = 0;

    virtual void InvalidateOperation(
        const bridge::OperationId& operation_id
    ) = 0;
};

// Thread-safe in-memory L2 reference backend for testing and
// single-process shared simulation.
class InMemorySharedCacheBackend final
    : public ISharedCacheBackend {
public:
    using Clock = std::chrono::steady_clock;
    using ClockFn = LocalOperationCache::ClockFn;

    explicit InMemorySharedCacheBackend(ClockFn clock = {})
        : clock_(clock ? std::move(clock) : []() { return Clock::now(); }) {}

    [[nodiscard]]
    std::optional<SharedCacheEntry> Get(
        const CacheKey& key
    ) override {
        const auto now = clock_();
        std::lock_guard lock(mutex_);
        const auto it = entries_.find(key);
        if (it == entries_.end()) {
            return std::nullopt;
        }
        if (now >= it->second.expires_at) {
            entries_.erase(it);
            return std::nullopt;
        }
        const auto rem =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                it->second.expires_at - now
            );
        if (rem.count() <= 0) {
            entries_.erase(it);
            return std::nullopt;
        }
        return SharedCacheEntry{it->second.result, rem};
    }

    void Put(
        const CacheKey& key,
        const bridge::ResultEnvelope& result,
        std::chrono::milliseconds ttl
    ) override {
        if (ttl.count() <= 0 || !result.IsSuccess()) {
            return;
        }
        const auto expiresAt = clock_() + ttl;
        std::lock_guard lock(mutex_);
        entries_.insert_or_assign(
            key,
            Stored{result, expiresAt}
        );
    }

    void InvalidateKey(const CacheKey& key) override {
        std::lock_guard lock(mutex_);
        entries_.erase(key);
    }

    void InvalidateOperation(
        const bridge::OperationId& operation_id
    ) override {
        std::lock_guard lock(mutex_);
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (it->first.operation_id == operation_id) {
                it = entries_.erase(it);
            } else {
                ++it;
            }
        }
    }

    [[nodiscard]]
    std::size_t Size() const {
        std::lock_guard lock(mutex_);
        return entries_.size();
    }

private:
    struct Stored final {
        bridge::ResultEnvelope result;
        Clock::time_point expires_at{};
    };

    ClockFn clock_;
    mutable std::mutex mutex_;
    std::unordered_map<CacheKey, Stored, CacheKeyHash> entries_;
};

class LayeredOperationCache final : public bridge::IOperationCache {
public:
    explicit LayeredOperationCache(
        std::shared_ptr<LocalOperationCache> local,
        std::shared_ptr<ISharedCacheBackend> shared = {},
        std::vector<std::string> metadata_key_fields = {}
    )
        : local_(std::move(local)),
          shared_(std::move(shared)),
          metadata_key_fields_(std::move(metadata_key_fields)) {
        if (!local_) {
            throw std::invalid_argument(
                "LayeredOperationCache requires a local cache"
            );
        }
    }

    [[nodiscard]]
    bridge::OperationCacheLookup Lookup(
        const bridge::OperationDescriptor& descriptor,
        const bridge::RequestEnvelope& request
    ) override {
        if (!descriptor.IsCacheEligible()) {
            return {};
        }

        auto localHit = local_->Lookup(descriptor, request);
        if (localHit.status == bridge::CacheResult::HitLocal ||
            localHit.status == bridge::CacheResult::Stale) {
            return localHit;
        }

        if (descriptor.cache.mode == bridge::CacheMode::Layered &&
            shared_) {
            const CacheKey key = CacheKeyBuilder::Build(
                descriptor,
                request,
                metadata_key_fields_
            );
            if (auto sharedEntry = shared_->Get(key)) {
                local_->StoreByKey(
                    key,
                    sharedEntry->result,
                    sharedEntry->remaining_ttl,
                    descriptor.cache.max_entry_bytes
                );
                shared_hits_.fetch_add(1, std::memory_order_relaxed);
                return {
                    bridge::CacheResult::HitShared,
                    std::move(sharedEntry->result),
                    false
                };
            }
        }

        return {};
    }

    void Store(
        const bridge::OperationDescriptor& descriptor,
        const bridge::RequestEnvelope& request,
        const bridge::ResultEnvelope& result
    ) override {
        if (!descriptor.IsCacheEligible() ||
            !result.IsSuccess() ||
            descriptor.cache.ttl.count() <= 0) {
            return;
        }

        local_->Store(descriptor, request, result);

        if (descriptor.cache.mode == bridge::CacheMode::Layered &&
            shared_) {
            const CacheKey key = CacheKeyBuilder::Build(
                descriptor,
                request,
                metadata_key_fields_
            );
            shared_->Put(key, result, descriptor.cache.ttl);
        }
    }

    void InvalidateOperation(
        const bridge::OperationId& operation_id
    ) override {
        local_->InvalidateOperation(operation_id);
        if (shared_) {
            shared_->InvalidateOperation(operation_id);
        }
    }

    [[nodiscard]]
    std::uint64_t SharedHits() const noexcept {
        return shared_hits_.load(std::memory_order_relaxed);
    }

private:
    std::shared_ptr<LocalOperationCache> local_;
    std::shared_ptr<ISharedCacheBackend> shared_;
    std::vector<std::string> metadata_key_fields_;
    std::atomic<std::uint64_t> shared_hits_{0};
};

} // namespace lamp::cache
