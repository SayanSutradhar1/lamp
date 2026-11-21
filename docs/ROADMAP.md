# Lamp — Phase-by-Phase Roadmap

Statuses reflect the **latest supplied build log (2026-11-21)** and the current discussion, not a fresh checkout inspection.

## Phase 1 — Core runtime foundation — previously completed
**Scope:** application lifecycle, event dispatcher, task scheduler, worker infrastructure, runtime context/manager, service registry.
**Evidence:** 8 core CTest suites passed.
**Exit:** reliable native lifecycle and basic concurrency primitives.

## Phase 2 — Native HTTP engine — previously completed
**Scope:** methods, headers, requests/responses, incremental parser, router, connection, server, tests.
**Evidence:** parser, router, connection, server suites passed.
**Note:** existing live server is blocking/synchronous; async upgrade is Phase 6.

## Phase 3 — Embedded V8 runtime — previously completed
**Scope:** platform, isolate/context management, script execution, value conversion, native bindings, module loading.
**Evidence:** 8 V8-specific CTest suites passed.
**Exit:** working embedded JavaScript runtime.

## Phase 4 — Operation bridge — previously completed
**Scope:** request/result envelopes, operation dispatch, HTTP transport adapter, JS/native integration.
**Evidence:** bridge integration suite passed.
**Exit:** stable transport-neutral operation contract; inspect actual code before extending.

## Phase 5 — Asynchronous execution engine — previously completed for current scope
**Scope:**
- Scheduler contract and task handles.
- Native worker pool.
- V8 isolate worker pool.
- Cancellation/deadlines.
- Promise continuation contract and worker-local scheduler.
- V8 async integration.
- Native/V8 dispatcher and integration tests.
**Evidence:** latest run includes native worker, cancellation, V8 worker lifecycle, Promise integration and dispatcher suites, all passing.
**Caveat:** actual external event-loop integration and broad production behavior still need verification; Promise microtasks alone do not implement timers or network I/O.

## Phase 6 — Transport and advanced routing — COMPLETED
### 6A. Routing and completion primitives — verified
- `http_middleware.hpp` and tests.
- `http_route_group.hpp` and tests.
- `http_completion_queue.hpp` and tests.
### 6B. Async HTTP execution bridge — verified
- `async_operation_coordinator.hpp` and 17 tests: passed.
- `http_async_connection.hpp` and 24 tests: passed and registered (includes oversized-response 500/abort fallback, write-buffer backpressure, `maxRetainedResponseBytes`, pipelined parse-error preservation, and `Connection` headers).
- `async_http_operation_pump.hpp` and `async_http_transport_adapter.hpp`: verified via 14 pump/adapter tests (`lamp_async_http_operation_pump`) and mixed Native + V8 Promise HTTP integration (`lamp_v8_async_http_pump_integration`).
### 6C. Server lifecycle and nonblocking I/O — verified
- `http_async_server.hpp` / `http_async_server.cpp`: single event-loop owner thread (`WSAPoll`/`poll`), loopback `WakeupState` socket pair, `(connectionId, generation)` registry, partial nonblocking writes, read backpressure, half-close graceful drain, disconnect cancellation hook, idle timeout, and bounded graceful/forced shutdown.
- `AsyncTaskCompletion::SetOnComplete()` + `AsyncOperationCoordinator::SetWakeupCallback()` + `AsyncHttpTransportAdapter::CreateServerCallbacks()` wire worker completions directly to reactor wakeup and `pump->Advance()`.
- `lamp_http_async_server_test` (10/10) and `LiveTcpAsyncServerWithNativePoolAndDisconnectCancel` passed.
### 6D. Backpressure and reliability — verified
- Verified in `HttpAsyncConnection` (24/24): oversized response stall fix, write-buffer backpressure, `maxRetainedResponseBytes` bounding out-of-order `Slot::response` memory, pipelined parse-error preservation, stale tokens, and `CancelConnection`/`CancelToken`.
- Verified in `AsyncHttpOperationPump`: queue saturation direct delivery, closed-queue drop, mapper exception 500, and scheduler rejection 503/504/400 mapping.
### 6E. Verification — verified
- Live loopback TCP tests passed for `HttpAsyncServer` (10/10 including idle timeout, forced shutdown timeout abort, and 8-client concurrent pipelined stress), `NativeWorkerPool` (`LiveTcpAsyncServerWithNativePoolAndDisconnectCancel`), and mixed `NativeWorkerPool` + `V8IsolateWorkerPool` Promise execution (`LiveTcpMixedNativeAndV8AsyncHttpServer`).

## Phase 7 — Serialization — COMPLETED
- Implemented `runtime/serialization/` (`lamp::serialization`): `serialization_value.hpp`, `schema_registry.hpp`, `protobuf_wire.hpp`, `protobuf_codec.hpp`, `json_codec.hpp`, `codec_negotiator.hpp`, `operation_codec_adapter.hpp`.
- Verified Protobuf-first codecs, JSON fallback + base64 `Bytes`, schema validation, media-type/`q=` negotiation, binary-safe payloads, and `ExecutionDispatcher` integration (`lamp_serialization_test`, 9/9 passed).

## Phase 8 — Caching and performance — COMPLETED
- Implemented `bridge::IOperationCache` integration in `ExecutionDispatcher` (`Dispatch`, `DispatchFresh`, `SetCache`, `SetRevalidationCallback`).
- Implemented `runtime/cache/` (`lamp::cache`): `CacheKeyBuilder`, sharded LRU+TTL `LocalOperationCache` with `stale_while_revalidate` and operation/tag invalidation, and `LayeredOperationCache` (`ISharedCacheBackend` / `InMemorySharedCacheBackend`) (`lamp_cache_test`, 8/8 passed).

## Phase 9 — Streaming and real-time — COMPLETED
- Implemented `runtime/streaming/` (`lamp::streaming`): bounded `StreamChannel`, `ChunkedHttpEncoder`/`ChunkedHttpDecoder`, `SseCodec`, `StreamHub`, and `HttpChunkedStreamPump`.
- Verified backpressure, slow-consumer flow control, cooperative cancellation, incremental chunked decoding, and live loopback TCP chunked streaming (`lamp_streaming_test`, 8/8 passed).

## Phase 10 — Security and reliability — COMPLETED
- Implemented `runtime/security/` (`lamp::security`): `crypto_utils.hpp`, `security_context.hpp`, `input_sanitizer.hpp`, `auth_policy.hpp`, `rate_limiter.hpp`, and `security_gateway.hpp`.
- Verified cryptographic primitives, path/metadata injection prevention, untrusted header stripping, 401/403 RBAC/scope/tenant enforcement, token-bucket 429 rate limiting with bounded LRU key tracking, circuit breakers, secret-free error sanitization, HTTP middleware integration, and 8-thread concurrent stress (`lamp_security_test`, 7/7 passed).

## Phase 11 — TypeScript SDK and developer experience — COMPLETED
- Implemented `sdk-ts/` (`createLampApp`, typed `app.register(operationId, ...)`, `s` schema builder + external `{ parse() }` interop, `LampError` canonical error model, lifecycle hooks, cancellation/deadlines, caching, auth enforcement, `__lampNativeBridge` integration, examples, and tests).
- Implemented `runtime/bridge/v8_sdk_host_bridge.hpp` connecting V8 JS SDK registrations into `OperationRegistry`, `ExecutionDispatcher`, `LocalOperationCache`, and `HttpTransportAdapter` (`lamp_v8_sdk_host_bridge` passed; `node --test sdk-ts/tests/sdk.test.ts` 7/7 passed).

## Phase 12 — Production readiness and v1.0 — COMPLETED
- Implemented `runtime/telemetry/` (`lamp::telemetry`): `metrics_registry.hpp`, `tracing.hpp`, `health_check.hpp`, `telemetry_dispatcher.hpp` (`lamp_telemetry_test`, 4/4 passed).
- Implemented `benchmarks/` (`LAMP_BUILD_BENCHMARKS=ON`, `lamp_runtime_benchmark` verified across 7 Release runtime micro-benchmarks), `.github/workflows/ci.yml` cross-platform CI workflow, and `docs/PRODUCTION.md`.

## Roadmap discipline
- Each phase requires real tests and integration evidence.
- Preserve backward compatibility when feasible.
- Change completed modules only for verified defects or necessary integration.
- Update `CURRENT.md` and test counts after each verified milestone.
