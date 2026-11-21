# Lamp Framework v1.0 — Production Guide

This document summarizes how to build, configure, observe, secure, benchmark, and deploy Lamp.

## 1. Build Options (`CMakeLists.txt`)
- `CMAKE_CXX_STANDARD=20` (required C++20 compiler: MSVC v14+, GCC 12+, or Clang 15+).
- `LAMP_BUILD_TESTS=ON` — builds and registers CTest unit and integration suites across all runtime modules (`core`, `http`, `v8`, `bridge`, `cache`, `execution`, `serialization`, `streaming`, `security`, `telemetry`).
- `LAMP_BUILD_BENCHMARKS=ON` — builds `lamp_runtime_benchmark` (`benchmarks/lamp_runtime_benchmark.cpp`).
- `LAMP_ENABLE_V8=ON|OFF` — toggles embedded V8 isolate runtime and JS operation execution (`V8SdkHostBridge`, `V8IsolateWorkerPool`).
- `LAMP_ENABLE_L2_CACHE=ON|OFF` — toggles distributed L2 cache backend support alongside `LayeredOperationCache`.

### Canonical Build & Test (Windows MSVC)
```powershell
cmake -S . -B build-msvc-v8 -DLAMP_BUILD_TESTS=ON -DLAMP_BUILD_BENCHMARKS=ON
cmake --build build-msvc-v8 --parallel 4
ctest --test-dir build-msvc-v8 --output-on-failure
```

## 2. TypeScript SDK (`sdk-ts/`)
Developers define logical operations rather than low-level HTTP routes:

```ts
import { createLampApp, s } from "./src/index.ts";

const app = createLampApp({
  transport: "auto",
  serialization: "protobuf",
  execution: "adaptive",
  cache: { enabled: true },
});

app.register("users.get", {
  input: s.object({ id: s.string({ minLength: 1 }) }),
  output: s.object({ id: s.string(), name: s.string() }),
  cache: { ttl: 60 },
  handler: async ({ input }) => ({ id: input.id, name: `User ${input.id}` }),
});

await app.start();
```

Run the SDK tests and example on Node >= 22.18 / 24:
```powershell
node --test sdk-ts/tests/sdk.test.ts
node sdk-ts/examples/users_service.ts
```

## 3. Security Checklist (`runtime/security/`)
- Enforce `HttpRequestSecurityValidator` and `EnvelopeSecuritySanitizer` at ingress to reject path traversal, ambiguous framing (`Content-Length` + `Transfer-Encoding`), missing/malformed HTTP/1.1 `Host`, oversized headers/bodies, and metadata injection.
- Strip untrusted headers (`Cookie`, `X-Lamp-*`, `X-Forwarded-*` unless behind a configured trusted proxy) and strip `lamp.*` / `auth.*` metadata keys prior to injecting authenticated principal context.
- Enable `AuthorizationPolicyEngine` with `default_deny = true` so operations require explicit `allow_anonymous = true` or RBAC roles/scopes/tenant matches.
- Use `TokenBucketRateLimiter` (bounded `max_tracked_keys` LRU) + per-operation `CircuitBreaker` (`CircuitBreakerRegistry`).
- Sanitize all returned errors with `SanitizeResultError` and apply safe response headers (`X-Content-Type-Options: nosniff`, `X-Frame-Options: DENY`, `Referrer-Policy: no-referrer`, `Cache-Control: no-store`).

## 4. Observability & Health Probes (`runtime/telemetry/`)
- Register `MakeLivenessHttpHandler(health)` (`/healthz`) and `MakeReadinessHttpHandler(health)` (`/readyz`) returning JSON `200 OK` (healthy/degraded) or `503 Service Unavailable` (unhealthy).
- Expose `MakePrometheusMetricsHttpHandler(metrics)` (`/metrics`) with bounded cardinality (`MetricsConfig::max_series`).
- Wrap `bridge::ExecutionDispatcher` with `TelemetryDispatcher` for automatic `lamp_operations_total`, `lamp_operations_in_flight`, `lamp_operation_duration_ms`, `lamp_operation_cache_hits_total`, and W3C `traceparent` span export.

## 5. Cross-Platform CI & Release Checklist
- Workflow defined in `.github/workflows/ci.yml` verifying Ubuntu + Windows builds, CTest suites, benchmark smoke execution, and `sdk-ts` test runs.
- Before tagging v1.0.0: verify zero CTest failures, zero `sdk-ts` failures, run `lamp_runtime_benchmark` in Release mode, and confirm docs (`CURRENT.md`, `PHASE.md`, `ROADMAP.md`, `TESTS.md`, `PRODUCTION.md`) reflect current commit state.
