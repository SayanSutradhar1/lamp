#include "telemetry/health_check.hpp"
#include "telemetry/metrics_registry.hpp"
#include "telemetry/telemetry_dispatcher.hpp"
#include "telemetry/tracing.hpp"

#include "bridge/execution_dispatcher.hpp"
#include "bridge/operation_registry.hpp"
#include "cache/local_operation_cache.hpp"
#include "http/http_router.hpp"

#include <atomic>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace lamp;
using namespace lamp::bridge;
using namespace lamp::cache;
using namespace lamp::http;
using namespace lamp::telemetry;

int g_failed = 0;

void Expect(bool cond, const char* expr, const char* file, int line) {
    if (!cond) {
        std::cerr << "[FAIL] " << file << ":" << line << " (" << expr << ")\n";
        ++g_failed;
    }
}

#define EXPECT(expr) Expect(static_cast<bool>(expr), #expr, __FILE__, __LINE__)

class FnHandler final : public INativeOperationHandler {
public:
    using Fn = std::function<ResultEnvelope(const RequestEnvelope&)>;
    explicit FnHandler(Fn fn) : fn_(std::move(fn)) {}
    ResultEnvelope Execute(const RequestEnvelope& req) override { return fn_(req); }
private:
    Fn fn_;
};

bool TestMetricsRegistryAndCardinalityBound() {
    MetricsRegistry reg(MetricsConfig{.max_series = 5});

    auto c1 = reg.GetOrCreateCounter("req_total", {{"op", "users.get"}});
    auto c2 = reg.GetOrCreateCounter("req_total", {{"op", "users.get"}});
    EXPECT(c1.get() == c2.get());
    c1->Increment(3);

    auto g1 = reg.GetOrCreateGauge("in_flight");
    g1->Increment(2);
    g1->Decrement(1);
    EXPECT(g1->Value() == 1);

    auto h1 = reg.GetOrCreateHistogram("lat_ms", {}, {1.0, 5.0, 20.0});
    h1->Observe(0.5);
    h1->Observe(3.0);
    h1->Observe(10.0);
    h1->Observe(50.0);
    const auto snap = h1->Snapshot();
    EXPECT(snap.total_count == 4);
    EXPECT(snap.QuantileEstimate(0.5) == 5.0);

    // Exhaust remaining series cap (3 used -> +2 allowed -> overflow)
    (void)reg.GetOrCreateCounter("extra_1");
    (void)reg.GetOrCreateCounter("extra_2");
    (void)reg.GetOrCreateCounter("overflow_1");
    EXPECT(reg.SeriesCount() == 5);
    EXPECT(reg.SeriesOverflowCount() == 1);

    const std::string prom = reg.RenderPrometheusText();
    EXPECT(prom.find("req_total{op=\"users.get\"} 3") != std::string::npos);
    EXPECT(prom.find("lat_ms_bucket{le=\"+Inf\"} 4") != std::string::npos);

    const std::string json = reg.RenderJsonSnapshot();
    EXPECT(json.find("\"series_count\":5") != std::string::npos);
    return true;
}

bool TestW3cTraceparentAndBoundedSpans() {
    const std::string valid_tp = "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01";
    const auto parsed = ParseTraceparent(valid_tp);
    EXPECT(parsed.has_value());
    EXPECT(parsed->trace_id == "4bf92f3577b34da6a3ce929d0e0e4736");
    EXPECT(parsed->span_id == "00f067aa0ba902b7");
    EXPECT(parsed->sampled);
    EXPECT(parsed->ToTraceparent() == valid_tp);

    EXPECT(!ParseTraceparent("invalid").has_value());
    EXPECT(!ParseTraceparent("00-00000000000000000000000000000000-00f067aa0ba902b7-01").has_value());

    auto exporter = std::make_shared<InMemorySpanExporter>(3);
    Tracer tracer(exporter);

    for (int i = 0; i < 5; ++i) {
        SpanScope span(tracer, "span-" + std::to_string(i), parsed);
        span.SetAttribute("iter", std::to_string(i));
        span.SetStatus(SpanStatus::Ok);
    }

    const auto exported = exporter->Snapshot();
    EXPECT(exported.size() == 3);
    EXPECT(exported.front().name == "span-2");
    EXPECT(exported.back().name == "span-4");
    EXPECT(exported.back().context.trace_id == parsed->trace_id);
    EXPECT(exported.back().context.parent_span_id == parsed->span_id);
    return true;
}

bool TestHealthAndMetricsHttpEndpoints() {
    auto health = std::make_shared<HealthRegistry>();
    std::atomic<bool> db_ok{true};

    health->RegisterLiveness("runtime", [] {
        return ComponentHealth{"runtime", HealthStatus::Healthy, "loop alive"};
    });
    health->RegisterReadiness("db", [&] {
        if (db_ok.load(std::memory_order_acquire)) {
            return ComponentHealth{"db", HealthStatus::Healthy, "connected"};
        }
        return ComponentHealth{"db", HealthStatus::Unhealthy, "connection lost"};
    });

    auto metrics = std::make_shared<MetricsRegistry>();
    metrics->GetOrCreateCounter("lamp_boot_total")->Increment();

    HttpRouter router;
    router.add(HttpMethod::Get, "/healthz", MakeLivenessHttpHandler(health));
    router.add(HttpMethod::Get, "/readyz", MakeReadinessHttpHandler(health));
    router.add(HttpMethod::Get, "/metrics", MakePrometheusMetricsHttpHandler(metrics));

    {
        HttpRequest req(HttpMethod::Get, "/healthz");
        const auto resp = router.dispatch(req);
        EXPECT(resp.statusCode() == 200);
        EXPECT(resp.header("Cache-Control") == "no-store");
    }
    {
        HttpRequest req(HttpMethod::Get, "/readyz");
        EXPECT(router.dispatch(req).statusCode() == 200);
    }

    db_ok.store(false, std::memory_order_release);
    {
        HttpRequest req(HttpMethod::Get, "/readyz");
        EXPECT(router.dispatch(req).statusCode() == 503);
    }
    {
        HttpRequest req(HttpMethod::Get, "/metrics");
        const auto resp = router.dispatch(req);
        EXPECT(resp.statusCode() == 200);
        const std::string body(
            reinterpret_cast<const char*>(resp.body().data()),
            resp.body().size()
        );
        EXPECT(body.find("lamp_boot_total 1") != std::string::npos);
    }
    return true;
}

bool TestTelemetryDispatcherWithCacheAndConcurrency() {
    auto registry = std::make_shared<OperationRegistry>();
    OperationDescriptor desc;
    desc.id = "orders.get";
    desc.execution.mode = ExecutionMode::Native;
    desc.cache.mode = CacheMode::Local;
    desc.cache.ttl = std::chrono::seconds(60);
    desc.capabilities.read_only = true;
    desc.capabilities.idempotent = true;

    EXPECT(
        registry->Register(
            desc,
            OperationBackends{
                std::make_shared<FnHandler>([](const RequestEnvelope& req) {
                    return ResultEnvelope::Success(
                        req,
                        {std::uint8_t('1')},
                        SerializationFormat::Json,
                        ExecutionBackend::Native
                    );
                }),
                {}
            }
        ).Ok()
    );

    auto cache = std::make_shared<LocalOperationCache>();
    auto base_disp = std::make_shared<ExecutionDispatcher>(
        registry,
        std::shared_ptr<IJavaScriptOperationExecutor>{},
        ExecutionDispatcherConfig{},
        cache
    );
    auto metrics = std::make_shared<MetricsRegistry>();
    auto exporter = std::make_shared<InMemorySpanExporter>(256);
    auto tracer = std::make_shared<Tracer>(exporter);

    TelemetryDispatcher obs(base_disp, metrics, tracer);

    // First call warms cache (native), next calls hit cache concurrently
    RequestEnvelope warm;
    warm.request_id = 1;
    warm.operation_id = "orders.get";
    warm.transport = TransportKind::Internal;
    warm.format = SerializationFormat::Json;
    warm.payload = {std::uint8_t('k')};
    const auto warm_res = obs.Dispatch(warm);
    EXPECT(warm_res.IsSuccess());
    EXPECT(warm_res.backend == ExecutionBackend::Native);
    EXPECT(!warm_res.trace_id.empty());

    constexpr int kThreads = 4;
    constexpr int kIters = 25;
    std::vector<std::thread> workers;
    std::atomic<int> failures{0};

    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&, t] {
            for (int i = 0; i < kIters; ++i) {
                RequestEnvelope req = warm;
                req.request_id = static_cast<RequestId>((t * kIters) + i + 2);
                const auto res = obs.Dispatch(req);
                if (!res.IsSuccess()) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& w : workers) {
        w.join();
    }

    EXPECT(failures.load() == 0);
    EXPECT(metrics->GetOrCreateGauge("lamp_operations_in_flight")->Value() == 0);
    EXPECT(
        metrics->GetOrCreateCounter(
            "lamp_operation_cache_hits_total",
            {{"operation", "orders.get"}}
        )->Value() == 100
    );
    EXPECT(exporter->Snapshot().size() == 101);
    return true;
}

} // namespace

int main() {
    struct Case {
        const char* name;
        bool (*fn)();
    };

    const Case cases[] = {
        {"MetricsRegistryAndCardinalityBound", TestMetricsRegistryAndCardinalityBound},
        {"W3cTraceparentAndBoundedSpans", TestW3cTraceparentAndBoundedSpans},
        {"HealthAndMetricsHttpEndpoints", TestHealthAndMetricsHttpEndpoints},
        {"TelemetryDispatcherWithCacheAndConcurrency", TestTelemetryDispatcherWithCacheAndConcurrency},
    };

    int passed = 0;
    const int total = static_cast<int>(sizeof(cases) / sizeof(cases[0]));

    for (const auto& c : cases) {
        const int before = g_failed;
        if (c.fn() && g_failed == before) {
            std::cout << "[PASS] " << c.name << "\n";
            ++passed;
        } else {
            std::cout << "[FAIL] " << c.name << "\n";
        }
    }

    std::cout << "Lamp telemetry: " << passed << "/" << total << " tests passed\n";
    return (passed == total && g_failed == 0) ? 0 : 1;
}
