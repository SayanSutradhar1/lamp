
#include "cache/cache_key.hpp"
#include "cache/layered_operation_cache.hpp"
#include "cache/local_operation_cache.hpp"

#include "bridge/execution_dispatcher.hpp"
#include "bridge/operation_registry.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

namespace bridge = lamp::bridge;
namespace cache = lamp::cache;

void Require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bridge::OperationDescriptor MakeDesc(
    std::string id,
    bridge::CacheMode mode = bridge::CacheMode::Local,
    std::chrono::milliseconds ttl = 1000ms
) {
    bridge::OperationDescriptor d;
    d.id = std::move(id);
    d.execution.mode = bridge::ExecutionMode::Native;
    d.input_format = bridge::SerializationFormat::Json;
    d.output_format = bridge::SerializationFormat::Json;
    d.cache.mode = mode;
    d.cache.ttl = ttl;
    if (mode != bridge::CacheMode::Disabled) {
        d.capabilities.read_only = true;
        d.capabilities.idempotent = true;
    }
    return d;
}

bridge::RequestEnvelope MakeReq(
    std::uint64_t reqId,
    std::string opId,
    std::vector<std::uint8_t> payload
) {
    bridge::RequestEnvelope r;
    r.request_id = reqId;
    r.operation_id = std::move(opId);
    r.transport = bridge::TransportKind::Internal;
    r.format = bridge::SerializationFormat::Json;
    r.payload = std::move(payload);
    return r;
}

bridge::ResultEnvelope MakeOk(
    const bridge::RequestEnvelope& req,
    std::string body
) {
    bridge::ResultEnvelope::Payload p(body.begin(), body.end());
    return bridge::ResultEnvelope::Success(
        req,
        std::move(p),
        bridge::SerializationFormat::Json,
        bridge::ExecutionBackend::Native
    );
}

class FakeClock final {
public:
    using time_point = std::chrono::steady_clock::time_point;

    FakeClock() : now_(std::chrono::steady_clock::now()) {}

    [[nodiscard]] time_point Now() const noexcept {
        return now_.load(std::memory_order_acquire);
    }

    void Advance(std::chrono::milliseconds d) noexcept {
        now_.store(Now() + d, std::memory_order_release);
    }

    [[nodiscard]] cache::LocalOperationCache::ClockFn Fn() {
        return [this]() { return Now(); };
    }

private:
    std::atomic<time_point> now_;
};

class CountingNativeHandler final
    : public bridge::INativeOperationHandler {
public:
    using Fn = std::function<
        bridge::ResultEnvelope(const bridge::RequestEnvelope&)
    >;

    explicit CountingNativeHandler(Fn fn) : fn_(std::move(fn)) {}

    bridge::ResultEnvelope Execute(
        const bridge::RequestEnvelope& request
    ) override {
        calls_.fetch_add(1, std::memory_order_relaxed);
        return fn_(request);
    }

    std::uint64_t Calls() const noexcept {
        return calls_.load(std::memory_order_relaxed);
    }

private:
    Fn fn_;
    std::atomic<std::uint64_t> calls_{0};
};

void TestCacheKeyBuilderDeterminismAndBinarySafety() {
    const auto desc = MakeDesc("users.get");

    auto req1 = MakeReq(1, "users.get", {0x01, 0x00, 0x02});
    req1.trace_id = "0123456789abcdef0123456789abcdef";
    req1.span_id = "0123456789abcdef";
    req1.AddMetadata("tenant", "alpha");

    auto req2 = MakeReq(999, "users.get", {0x01, 0x00, 0x02});
    req2.trace_id = "fedcba9876543210fedcba9876543210";
    req2.span_id = "fedcba9876543210";
    req2.AddMetadata("tenant", "alpha");

    const auto k1 = cache::CacheKeyBuilder::Build(desc, req1, {"tenant"});
    const auto k2 = cache::CacheKeyBuilder::Build(desc, req2, {"tenant"});
    Require(k1 == k2, "Keys match across distinct request_id/trace_id");

    // Embedded null boundary difference
    const auto reqShort = MakeReq(3, "users.get", {0x01});
    const auto kShort = cache::CacheKeyBuilder::Build(desc, reqShort, {"tenant"});
    Require(!(k1 == kShort), "Binary length/null bytes distinguished");

    // Different metadata value
    auto reqBeta = MakeReq(4, "users.get", {0x01, 0x00, 0x02});
    reqBeta.AddMetadata("tenant", "beta");
    const auto kBeta = cache::CacheKeyBuilder::Build(desc, reqBeta, {"tenant"});
    Require(!(k1 == kBeta), "Allowlisted metadata value distinguished");

    // Different schema version
    auto descV2 = desc;
    descV2.input_schema = {"users.GetReq", 2};
    const auto kV2 = cache::CacheKeyBuilder::Build(descV2, req1, {"tenant"});
    Require(!(k1 == kV2), "Schema version change invalidates key");
}

void TestLocalCacheHitMissAndTtlExpiration() {
    FakeClock clock;
    cache::LocalCacheConfig cfg;
    cfg.shard_count = 1;
    cache::LocalOperationCache c(cfg, clock.Fn());

    const auto desc = MakeDesc("op.ttl", bridge::CacheMode::Local, 500ms);
    const auto req = MakeReq(1, "op.ttl", {'a'});

    Require(
        c.Lookup(desc, req).status == bridge::CacheResult::Miss,
        "Initial lookup is Miss"
    );

    c.Store(desc, req, MakeOk(req, "v1"));
    clock.Advance(250ms);

    const auto hit = c.Lookup(desc, req);
    Require(
        hit.status == bridge::CacheResult::HitLocal &&
            hit.result.has_value(),
        "Fresh hit before TTL"
    );

    clock.Advance(300ms); // 550ms >= 500ms TTL
    const auto expired = c.Lookup(desc, req);
    Require(
        expired.status == bridge::CacheResult::Miss,
        "Expired entry is Miss"
    );

    const auto stats = c.Stats();
    Require(stats.hits == 1, "1 hit");
    Require(stats.misses == 2, "2 misses");
    Require(stats.expirations == 1, "1 expiration");
    Require(stats.entry_count == 0, "Expired entry evicted");
}

void TestLocalCacheLruAndByteCapacityEviction() {
    FakeClock clock;
    cache::LocalCacheConfig cfg;
    cfg.max_entries = 2;
    cfg.max_entry_bytes = 256;
    cfg.max_total_bytes = 2048;
    cfg.shard_count = 1;
    cache::LocalOperationCache c(cfg, clock.Fn());

    const auto desc = MakeDesc("op.lru", bridge::CacheMode::Local, 1000ms);
    const auto r1 = MakeReq(1, "op.lru", {'1'});
    const auto r2 = MakeReq(2, "op.lru", {'2'});
    const auto r3 = MakeReq(3, "op.lru", {'3'});

    c.Store(desc, r1, MakeOk(r1, "one"));
    c.Store(desc, r2, MakeOk(r2, "two"));

    // Touch r1 so r2 becomes LRU
    Require(c.Lookup(desc, r1).status == bridge::CacheResult::HitLocal, "r1 hit");

    // Store r3 -> should evict r2
    c.Store(desc, r3, MakeOk(r3, "three"));

    Require(c.Lookup(desc, r1).status == bridge::CacheResult::HitLocal, "r1 retained");
    Require(c.Lookup(desc, r3).status == bridge::CacheResult::HitLocal, "r3 retained");
    Require(c.Lookup(desc, r2).status == bridge::CacheResult::Miss, "r2 LRU evicted");

    // Oversized entry rejected
    c.Store(desc, r1, MakeOk(r1, std::string(512, 'X')));
    const auto st = c.Stats();
    Require(st.rejected_oversized == 1, "Oversized entry rejected");
    Require(st.evictions >= 1, "Eviction counter incremented");
}

void TestStaleWhileRevalidateSingleWinnerAndGraceExpiry() {
    FakeClock clock;
    cache::LocalCacheConfig cfg;
    cfg.stale_grace_period = 1000ms;
    cfg.shard_count = 1;
    cache::LocalOperationCache c(cfg, clock.Fn());

    auto desc = MakeDesc("op.swr", bridge::CacheMode::Local, 200ms);
    desc.cache.stale_while_revalidate = true;
    const auto req = MakeReq(1, "op.swr", {'x'});

    c.Store(desc, req, MakeOk(req, "initial"));
    clock.Advance(250ms); // past 200ms TTL, within 1000ms stale grace

    const auto firstStale = c.Lookup(desc, req);
    Require(
        firstStale.status == bridge::CacheResult::Stale &&
            firstStale.needs_revalidation == true,
        "First stale caller gets needs_revalidation=true"
    );

    const auto secondStale = c.Lookup(desc, req);
    Require(
        secondStale.status == bridge::CacheResult::Stale &&
            secondStale.needs_revalidation == false,
        "Concurrent stale caller does not duplicate revalidation"
    );

    // Revalidation completes and refreshes entry
    c.Store(desc, req, MakeOk(req, "refreshed"));
    const auto fresh = c.Lookup(desc, req);
    Require(
        fresh.status == bridge::CacheResult::HitLocal &&
            std::string(fresh.result->payload.begin(), fresh.result->payload.end()) ==
                "refreshed",
        "Refreshed entry returns HitLocal"
    );

    // Advance past TTL + stale grace (200 + 1000 = 1200ms)
    clock.Advance(1300ms);
    Require(
        c.Lookup(desc, req).status == bridge::CacheResult::Miss,
        "Entry past stale grace expires to Miss"
    );
}

void TestOperationAndTagInvalidation() {
    FakeClock clock;
    cache::LocalCacheConfig cfg;
    cfg.shard_count = 4;
    cache::LocalOperationCache c(cfg, clock.Fn());

    const auto descA = MakeDesc("op.a", bridge::CacheMode::Local, 5000ms);
    const auto descB = MakeDesc("op.b", bridge::CacheMode::Local, 5000ms);

    const auto reqA1 = MakeReq(1, "op.a", {'1'});
    const auto reqA2 = MakeReq(2, "op.a", {'2'});
    const auto reqB1 = MakeReq(3, "op.b", {'1'});

    const auto keyA1 = cache::CacheKeyBuilder::Build(descA, reqA1);
    const auto keyA2 = cache::CacheKeyBuilder::Build(descA, reqA2);
    const auto keyB1 = cache::CacheKeyBuilder::Build(descB, reqB1);

    c.StoreByKey(keyA1, MakeOk(reqA1, "a1"), 5000ms, 0, {"user:7"});
    c.StoreByKey(keyA2, MakeOk(reqA2, "a2"), 5000ms, 0, {"user:8"});
    c.StoreByKey(keyB1, MakeOk(reqB1, "b1"), 5000ms, 0, {"user:7"});

    Require(c.InvalidateByTag("user:7") == 2, "Tag invalidated 2 entries");
    Require(c.Lookup(descA, reqA1).status == bridge::CacheResult::Miss, "a1 gone");
    Require(c.Lookup(descB, reqB1).status == bridge::CacheResult::Miss, "b1 gone");
    Require(c.Lookup(descA, reqA2).status == bridge::CacheResult::HitLocal, "a2 kept");

    c.InvalidateOperation("op.a");
    Require(c.Lookup(descA, reqA2).status == bridge::CacheResult::Miss, "op.a invalidated");
}

void TestLayeredOperationCacheL1AndL2Promotion() {
    FakeClock clock;
    cache::LocalCacheConfig cfg;
    cfg.shard_count = 1;
    auto l1 = std::make_shared<cache::LocalOperationCache>(cfg, clock.Fn());
    auto l2 = std::make_shared<cache::InMemorySharedCacheBackend>(clock.Fn());
    cache::LayeredOperationCache layered(l1, l2);

    const auto desc = MakeDesc("op.layered", bridge::CacheMode::Layered, 1000ms);
    const auto req = MakeReq(1, "op.layered", {'k'});
    const auto key = cache::CacheKeyBuilder::Build(desc, req);

    // Pre-populate L2 only (simulating another node)
    l2->Put(key, MakeOk(req, "from_l2"), 1000ms);

    const auto first = layered.Lookup(desc, req);
    Require(
        first.status == bridge::CacheResult::HitShared &&
            first.result.has_value(),
        "L1 miss promoted from L2 HitShared"
    );

    const auto second = layered.Lookup(desc, req);
    Require(
        second.status == bridge::CacheResult::HitLocal,
        "Subsequent lookup hits promoted L1 HitLocal"
    );

    layered.InvalidateOperation("op.layered");
    Require(l2->Size() == 0, "L2 invalidated");
    Require(
        layered.Lookup(desc, req).status == bridge::CacheResult::Miss,
        "L1 invalidated"
    );
}

void TestExecutionDispatcherCacheIntegration() {
    FakeClock clock;
    cache::LocalCacheConfig cfg;
    cfg.stale_grace_period = 1000ms;
    cfg.shard_count = 4;
    auto opCache = std::make_shared<cache::LocalOperationCache>(cfg, clock.Fn());

    auto reg = std::make_shared<bridge::OperationRegistry>();

    std::atomic<int> version{1};
    auto cachedHandler = std::make_shared<CountingNativeHandler>(
        [&](const bridge::RequestEnvelope& req) {
            if (req.payload == std::vector<std::uint8_t>{'f', 'a', 'i', 'l'}) {
                return bridge::ResultEnvelope::Failure(
                    req,
                    {bridge::OperationErrorCode::InvalidArgument, "bad", "BAD", false},
                    bridge::ExecutionBackend::Native
                );
            }
            return MakeOk(req, "val_" + std::to_string(version.load()));
        }
    );

    auto swrDesc = MakeDesc("cached.op", bridge::CacheMode::Local, 500ms);
    swrDesc.cache.stale_while_revalidate = true;
    bridge::OperationBackends b1;
    b1.native = cachedHandler;
    Require(reg->Register(swrDesc, std::move(b1)).Ok(), "Register cached.op");

    auto uncachedHandler = std::make_shared<CountingNativeHandler>(
        [](const bridge::RequestEnvelope& req) {
            return MakeOk(req, "uncached");
        }
    );
    bridge::OperationBackends b2;
    b2.native = uncachedHandler;
    Require(
        reg->Register(
            MakeDesc("uncached.op", bridge::CacheMode::Disabled, 0ms),
            std::move(b2)
        ).Ok(),
        "Register uncached.op"
    );

    bridge::ExecutionDispatcher dispatcher(reg, {}, {}, opCache);
    bool revalidationTriggered = false;
    dispatcher.SetRevalidationCallback([&](bridge::RequestEnvelope req) {
        revalidationTriggered = true;
        version.store(2);
        (void)dispatcher.DispatchFresh(req);
    });

    // 1. Disabled cache -> NotChecked every time
    const auto u1 = dispatcher.Dispatch(MakeReq(1, "uncached.op", {'a'}));
    const auto u2 = dispatcher.Dispatch(MakeReq(2, "uncached.op", {'a'}));
    Require(
        u1.cache_result == bridge::CacheResult::NotChecked &&
            u2.cache_result == bridge::CacheResult::NotChecked &&
            uncachedHandler->Calls() == 2,
        "Disabled cache does not cache"
    );

    // 2. First cached call -> Miss, populates cache
    const auto c1 = dispatcher.Dispatch(MakeReq(10, "cached.op", {'o', 'k'}));
    Require(
        c1.IsSuccess() &&
            c1.cache_result == bridge::CacheResult::Miss &&
            c1.backend == bridge::ExecutionBackend::Native &&
            cachedHandler->Calls() == 1,
        "First call is Miss"
    );

    // 3. Second cached call -> HitLocal with new request_id
    const auto c2 = dispatcher.Dispatch(MakeReq(20, "cached.op", {'o', 'k'}));
    Require(
        c2.IsSuccess() &&
            c2.request_id == 20 &&
            c2.cache_result == bridge::CacheResult::HitLocal &&
            c2.backend == bridge::ExecutionBackend::Cache &&
            cachedHandler->Calls() == 1,
        "Second call is HitLocal without calling handler"
    );

    // 4. Failures are never cached
    const auto f1 = dispatcher.Dispatch(MakeReq(30, "cached.op", {'f', 'a', 'i', 'l'}));
    const auto f2 = dispatcher.Dispatch(MakeReq(31, "cached.op", {'f', 'a', 'i', 'l'}));
    Require(
        f1.IsFailure() && f2.IsFailure() && cachedHandler->Calls() == 3,
        "Failures are not cached"
    );

    // 5. Stale-while-revalidate refreshes cleanly via DispatchFresh
    clock.Advance(600ms);
    const auto staleRes = dispatcher.Dispatch(MakeReq(40, "cached.op", {'o', 'k'}));
    Require(
        staleRes.cache_result == bridge::CacheResult::Stale &&
            std::string(staleRes.payload.begin(), staleRes.payload.end()) == "val_1" &&
            revalidationTriggered,
        "Stale hit returned old value and triggered revalidation"
    );

    const auto refreshedRes =
        dispatcher.Dispatch(MakeReq(41, "cached.op", {'o', 'k'}));
    Require(
        refreshedRes.cache_result == bridge::CacheResult::HitLocal &&
            std::string(refreshedRes.payload.begin(), refreshedRes.payload.end()) ==
                "val_2",
        "Subsequent call gets refreshed HitLocal"
    );

    // 6. Explicit operation invalidation forces Miss
    opCache->InvalidateOperation("cached.op");
    const auto afterInv =
        dispatcher.Dispatch(MakeReq(42, "cached.op", {'o', 'k'}));
    Require(
        afterInv.cache_result == bridge::CacheResult::Miss,
        "After invalidation returns Miss"
    );
}

void TestConcurrentCacheStress() {
    cache::LocalCacheConfig cfg;
    cfg.max_entries = 128;
    cfg.max_total_bytes = 1024 * 1024;
    cfg.shard_count = 8;
    cache::LocalOperationCache c(cfg);

    const auto desc = MakeDesc("stress.op", bridge::CacheMode::Local, 5000ms);
    constexpr int kThreads = 8;
    constexpr int kIterations = 400;

    std::vector<std::thread> workers;
    workers.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&, t]() {
            for (int i = 0; i < kIterations; ++i) {
                const auto bucket = static_cast<std::uint8_t>(i % 64);
                const auto req = MakeReq(
                    static_cast<std::uint64_t>(t * kIterations + i + 1),
                    "stress.op",
                    {bucket}
                );
                const auto res = c.Lookup(desc, req);
                if (res.status == bridge::CacheResult::Miss) {
                    c.Store(desc, req, MakeOk(req, "payload"));
                }
                if (i % 97 == 0) {
                    c.InvalidateOperation("stress.op");
                }
            }
        });
    }

    for (auto& w : workers) {
        w.join();
    }

    const auto stats = c.Stats();
    Require(stats.entry_count <= cfg.max_entries, "Bounded entry_count");
    Require(stats.total_bytes <= cfg.max_total_bytes, "Bounded total_bytes");
}

} // namespace

int main() {
    struct TestCase {
        const char* name;
        std::function<void()> run;
    };

    const std::vector<TestCase> tests = {
        {"CacheKeyBuilderDeterminismAndBinarySafety",
         TestCacheKeyBuilderDeterminismAndBinarySafety},
        {"LocalCacheHitMissAndTtlExpiration",
         TestLocalCacheHitMissAndTtlExpiration},
        {"LocalCacheLruAndByteCapacityEviction",
         TestLocalCacheLruAndByteCapacityEviction},
        {"StaleWhileRevalidateSingleWinnerAndGraceExpiry",
         TestStaleWhileRevalidateSingleWinnerAndGraceExpiry},
        {"OperationAndTagInvalidation", TestOperationAndTagInvalidation},
        {"LayeredOperationCacheL1AndL2Promotion",
         TestLayeredOperationCacheL1AndL2Promotion},
        {"ExecutionDispatcherCacheIntegration",
         TestExecutionDispatcherCacheIntegration},
        {"ConcurrentCacheStress", TestConcurrentCacheStress}
    };

    std::size_t passed = 0;
    for (const auto& test : tests) {
        try {
            test.run();
            ++passed;
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception& error) {
            std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n';
            return 1;
        } catch (...) {
            std::cerr << "[FAIL] " << test.name << ": unknown exception\n";
            return 1;
        }
    }

    std::cout
        << "Lamp cache: "
        << passed << "/" << tests.size()
        << " tests passed\n";
    return 0;
}
