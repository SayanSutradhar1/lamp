# Lamp — Current Development State

**As of:** 2026-11-22, verified directly in `D:\Projects\lamp` via MSVC CMake/Ninja build and CTest.

## Verified baseline
- Project: `D:\Projects\lamp`
- Platform: Windows AMD64; MSVC Visual Studio 2026 Build Tools (`v14.52`).
- C++20, CMake + Ninja, `build-msvc-v8`, Release.
- V8 enabled (`ON`), native tasks enabled (`ON`), L2 cache disabled (`OFF`).
- **Latest full CTest: 44/44 suites passed, exit code 0, 9.68 seconds.**
- **Latest TypeScript SDK (`sdk-ts`) tests: 7/7 passed (`node --test sdk-ts/tests/sdk.test.ts`, exit code 0, 207.9ms).**
- **Latest benchmark build & run (`LAMP_BUILD_BENCHMARKS=ON`): `lamp_runtime_benchmark.exe` exited 0 across 7 micro-benchmarks.**
- **Phases 1 – 12 are complete and verified for the current repository scope.**

## Latest verified changes (Phase 12 — Production Readiness and v1.0)
- **`runtime/telemetry/` (`lamp::telemetry`):**
  - `metrics_registry.hpp`: Thread-safe atomic `Counter` / `Gauge`, `Histogram` with percentile estimation, cardinality-bounded `MetricsRegistry` (`max_series`), Prometheus text & JSON snapshot renderers.
  - `tracing.hpp`: W3C `traceparent` parser (`ParseTraceparent`) & formatter, `SpanContext`, bounded `InMemorySpanExporter`, `Tracer`, and RAII `SpanScope`.
  - `health_check.hpp`: Liveness/readiness `HealthRegistry`, JSON `HealthReport`, and HTTP route handlers (`MakeLivenessHttpHandler`, `MakeReadinessHttpHandler`, `MakePrometheusMetricsHttpHandler`).
  - `telemetry_dispatcher.hpp`: `TelemetryDispatcher` decorating `bridge::ExecutionDispatcher` with `lamp_operations_in_flight`, `lamp_operations_total`, `lamp_operation_duration_ms`, `lamp_operation_cache_hits_total`, and W3C trace/span propagation.
  - `CMakeLists.txt` & `tests/telemetry_test.cpp` (`lamp_telemetry_test`): **4/4** tests passed (metrics cardinality, W3C tracing & span bounds, `/healthz`/`/readyz`/`/metrics` endpoints, and concurrent `TelemetryDispatcher` + cache execution).
- **`benchmarks/` (`LAMP_BUILD_BENCHMARKS=ON` in `CMakeLists.txt`):**
  - `lamp_runtime_benchmark.cpp` wired into CMake and verified in MSVC Release (`http_parse_and_route`, `bridge_dispatch_native_uncached`, `bridge_dispatch_local_cache_hit`, `telemetry_dispatch_cached`, `security_hmac_token_verify`, `security_token_bucket_acquire`, `streaming_channel_push_pop`).
- **CI & Production Documentation:**
  - `.github/workflows/ci.yml`: GitHub Actions CI covering Ubuntu + Windows C++20 builds/CTest/benchmarks and Node 24 TypeScript SDK verification.
  - `docs/PRODUCTION.md`: Production build, SDK, security, observability (`/healthz`, `/readyz`, `/metrics`), and release verification guide.

## What works
- All 12 roadmap phases implemented and verified: core runtime (8 suites), HTTP engine + async nonblocking `HttpAsyncServer` (11 `http`-labeled suites), embedded V8 runtime (8 `v8` unit suites), operation bridge + V8 SDK host bridge (5 `bridge`-labeled suites), async execution engine (`NativeWorkerPool`, `V8IsolateWorkerPool`, `AsyncExecutionDispatcher`), Phase 7 serialization (`lamp_serialization_test`), Phase 8 operation caching (`lamp_cache_test`), Phase 9 streaming/real-time (`lamp_streaming_test`), Phase 10 security/reliability (`lamp_security_test`), Phase 11 TypeScript SDK (`sdk-ts/` + `lamp_v8_sdk_host_bridge`), and Phase 12 telemetry/observability + benchmarks + CI (`lamp_telemetry_test`, `lamp_runtime_benchmark`).

## Immediate next actions (Post-Phase 12 / Maintenance)
- Run optional multi-OS GitHub Actions checks when pushed to remote git hosting, and maintain regression coverage across future SDK/runtime extensions.

## Known notes / deferred items
- `HttpServer` (`runtime/http/http_server.hpp/.cpp`) remains the blocking per-connection-thread server alongside `HttpAsyncServer` (`runtime/http/http_async_server.hpp/.cpp`).
- Pre-existing root scratch files (`lamp_phase6_*`, `*.obj`, `*.exe`) were inspected and removed; `CMakeLists.txt` is the only file in the repository root.

## Working agreement for an autonomous coding agent
- Read files and run commands directly; avoid manual copy/paste.
- Keep changes scoped and explain them.
- Run relevant tests and report exact output/exit codes.
- Never fabricate test results or implemented capabilities.
- Do not delete untracked root files without inspecting them.
- Update `CURRENT.md`, `PHASE.md`, `TESTS.md`, and `ROADMAP.md` at the end of each work unit with verified results.
- Store manual compile artifacts in `build-msvc-v8/manual-tests/`.
- Preserve `createLampApp` as the intended TypeScript factory.
