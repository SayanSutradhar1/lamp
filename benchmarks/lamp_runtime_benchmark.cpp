// ============================================================
// Lamp Framework - Phase 12 Runtime Micro-Benchmarks
//
// File: benchmarks/lamp_runtime_benchmark.cpp
// ============================================================

#include "bridge/execution_dispatcher.hpp"
#include "bridge/operation_registry.hpp"
#include "cache/local_operation_cache.hpp"
#include "http/http_parser.hpp"
#include "http/http_router.hpp"
#include "security/auth_policy.hpp"
#include "security/rate_limiter.hpp"
#include "streaming/stream_channel.hpp"
#include "telemetry/telemetry_dispatcher.hpp"

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

namespace {

using namespace lamp;
using Clock = std::chrono::steady_clock;

template <typename Fn>
void RunBenchmark(std::string_view name, std::size_t iterations, Fn&& fn) {
    // Warmup
    for (std::size_t i = 0; i < 100; ++i) {
        fn();
    }

    const auto start = Clock::now();
    for (std::size_t i = 0; i < iterations; ++i) {
        fn();
    }
    const auto end = Clock::now();

    const double elapsed_ms =
        std::chrono::duration<double, std::milli>(end - start).count();
    const double ops_per_sec =
        elapsed_ms > 0.0 ? (static_cast<double>(iterations) * 1000.0 / elapsed_ms) : 0.0;
    const double ns_per_op =
        iterations > 0 ? (elapsed_ms * 1e6 / static_cast<double>(iterations)) : 0.0;

    std::cout << std::left << std::setw(42) << name
              << " iters=" << std::setw(8) << iterations
              << " total_ms=" << std::setw(10) << std::fixed << std::setprecision(2) << elapsed_ms
              << " ops/s=" << std::setw(12) << std::fixed << std::setprecision(0) << ops_per_sec
              << " ns/op=" << std::fixed << std::setprecision(1) << ns_per_op
              << "\n";
}

class EchoNativeHandler final : public bridge::INativeOperationHandler {
public:
    bridge::ResultEnvelope Execute(const bridge::RequestEnvelope& req) override {
        return bridge::ResultEnvelope::Success(
            req,
            req.payload,
            bridge::SerializationFormat::Json,
            bridge::ExecutionBackend::Native
        );
    }
};

} // namespace

int main() {
    std::cout << "=== Lamp Runtime Benchmark Suite ===\n";

    // 1. HTTP Request Parse + Route Dispatch
    {
        http::HttpRouter router;
        router.add(
            http::HttpMethod::Get,
            "/users/:id",
            [](const http::HttpRequest&, const http::HttpRouteContext&) {
                http::HttpResponse resp(http::HttpStatus::Ok);
                resp.setBody("{\"ok\":true}");
                return resp;
            }
        );

        const std::string raw =
            "GET /users/42?fields=name HTTP/1.1\r\n"
            "Host: api.example.com\r\n"
            "Connection: keep-alive\r\n"
            "\r\n";

        RunBenchmark("http_parse_and_route", 20000, [&] {
            http::HttpParser parser;
            (void)parser.feed(raw);
            if (parser.hasRequest()) {
                (void)router.dispatch(parser.takeRequest());
            }
        });
    }

    // 2. Native ExecutionDispatcher (uncached vs cached + telemetry)
    {
        auto registry = std::make_shared<bridge::OperationRegistry>();
        bridge::OperationDescriptor desc;
        desc.id = "users.get";
        desc.execution.mode = bridge::ExecutionMode::Native;
        desc.cache.mode = bridge::CacheMode::Local;
        desc.cache.ttl = std::chrono::seconds(60);
        desc.capabilities.read_only = true;
        desc.capabilities.idempotent = true;
        (void)registry->Register(
            desc,
            bridge::OperationBackends{std::make_shared<EchoNativeHandler>(), {}}
        );

        auto uncached_disp = std::make_shared<bridge::ExecutionDispatcher>(registry);
        auto cache = std::make_shared<cache::LocalOperationCache>();
        auto cached_disp = std::make_shared<bridge::ExecutionDispatcher>(
            registry,
            nullptr,
            bridge::ExecutionDispatcherConfig{},
            cache
        );
        auto telemetry_disp = std::make_shared<telemetry::TelemetryDispatcher>(cached_disp);

        bridge::RequestEnvelope req;
        req.request_id = 1;
        req.operation_id = "users.get";
        req.transport = bridge::TransportKind::Internal;
        req.format = bridge::SerializationFormat::Json;
        req.payload = {std::uint8_t('{'), std::uint8_t('}')};

        // Prime cache
        (void)cached_disp->Dispatch(req);

        RunBenchmark("bridge_dispatch_native_uncached", 25000, [&] {
            (void)uncached_disp->Dispatch(req);
        });
        RunBenchmark("bridge_dispatch_local_cache_hit", 25000, [&] {
            (void)cached_disp->Dispatch(req);
        });
        RunBenchmark("telemetry_dispatch_cached", 25000, [&] {
            (void)telemetry_disp->Dispatch(req);
        });
    }

    // 3. Security: HMAC token verification + Sharded rate limiter
    {
        security::SignedTokenAuthenticator auth("bench-hmac-secret-2026");
        const std::string token = auth.IssueToken(security::SignedTokenClaims{
            "user-99",
            "tenant-1",
            {"member"},
            {"users:read"},
            0
        });
        const security::RawCredentials creds{"bearer", token};
        RunBenchmark("security_hmac_token_verify", 10000, [&] {
            (void)auth.Authenticate(creds);
        });

        security::TokenBucketRateLimiter limiter(
            security::RateLimitPolicy{1e9, 1e6, 1024, 32}
        );
        RunBenchmark("security_token_bucket_acquire", 25000, [&] {
            (void)limiter.TryAcquire("user-99", "users.get");
        });
    }

    // 4. Streaming: bounded StreamChannel TryPush + TryPop
    {
        streaming::StreamChannel channel(
            streaming::StreamConfig{64, 64 * 1024, 4096}
        );
        RunBenchmark("streaming_channel_push_pop", 25000, [&] {
            streaming::StreamChunk chunk;
            chunk.data.push_back(std::uint8_t('x'));
            (void)channel.TryPush(std::move(chunk));
            (void)channel.TryPop();
        });
    }

    return 0;
}
