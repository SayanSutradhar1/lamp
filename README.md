# Lamp

**Lamp** is an operation-oriented backend framework built around a **C++20 native runtime**, an **embedded V8 JavaScript engine**, and a **TypeScript application SDK (`createLampApp`)**.

Applications declare typed logical operations (`users.get`, `orders.create`, etc.) rather than low-level Express-style routes, while C++ handles transport framing, scheduling, caching, Protobuf/JSON serialization, streaming, security enforcement, and telemetry.

---

## Architecture Overview

```text
TypeScript SDK (sdk-ts: createLampApp / app.register / schemas)
                         |
                 V8 SDK Host Bridge
            (runtime/bridge/v8_sdk_host_bridge.hpp)
                         |
        ExecutionDispatcher + OperationRegistry
         /        |         |        |        \
   Security   Cache   Serialization  Async    Telemetry
(401/403/429) (L1/L2)  (Protobuf/JSON) Pool   (Metrics/Traces)
                         |
         HTTP Engine & Async Server (HttpAsyncServer)
                         |
                   Chunked / SSE Streaming
```

### Runtime Modules (`runtime/`)

| Module | Purpose |
| :--- | :--- |
| `runtime/core/` | Application lifecycle, event dispatcher, worker pool, task scheduler, runtime manager, service registry |
| `runtime/http/` | Incremental HTTP/1.0–1.1 parser, router, route groups, middleware chain, completion queue, async connection & `HttpAsyncServer` |
| `runtime/v8/` | V8 platform, isolate/context management, script engine, value conversion, native bindings, ES module loader |
| `runtime/bridge/` | `OperationDescriptor`, `OperationRegistry`, `RequestEnvelope`/`ResultEnvelope`, `ExecutionDispatcher`, HTTP adapters, `V8SdkHostBridge` |
| `runtime/execution/` | `NativeWorkerPool`, `V8IsolateWorkerPool`, Promise continuations, cancellation tokens & deadlines, `AsyncExecutionDispatcher` |
| `runtime/serialization/` | Dynamic `Value`, `SchemaRegistry`, `ProtobufCodec`, `JsonCodec` (base64 `Bytes`), media-type/`q=` negotiation |
| `runtime/cache/` | `CacheKeyBuilder`, sharded LRU+TTL `LocalOperationCache` (`stale_while_revalidate`), `LayeredOperationCache` |
| `runtime/streaming/` | Bounded `StreamChannel`, RFC 9112 `ChunkedHttpEncoder`/`ChunkedHttpDecoder`, `SseCodec`, `StreamHub`, `HttpChunkedStreamPump` |
| `runtime/security/` | SHA-256 / HMAC-SHA256 tokens & hashed API keys, RBAC/scope/tenant policies, sharded LRU token-bucket rate limiter, circuit breakers, input sanitization |
| `runtime/telemetry/` | Cardinality-bounded `MetricsRegistry` (Prometheus/JSON), W3C `traceparent` `Tracer`/`SpanScope`, `/healthz`/`/readyz` probes, `TelemetryDispatcher` |

---

## Quick Start

### 1. C++20 Build & Tests (CMake + Ninja)

Configure and build (native tests + benchmarks; add `-DLAMP_ENABLE_V8=OFF` if V8 is not installed locally):

```powershell
cmake -S . -B build-ci -DCMAKE_BUILD_TYPE=Release -DLAMP_BUILD_TESTS=ON -DLAMP_BUILD_BENCHMARKS=ON -DLAMP_ENABLE_V8=OFF
cmake --build build-ci --parallel 4
ctest --test-dir build-ci --output-on-failure
```

With embedded V8 enabled (`build-msvc-v8` verified baseline):

```powershell
ctest --test-dir build-msvc-v8 --output-on-failure
```

Run the Release runtime micro-benchmarks (`benchmarks/lamp_runtime_benchmark.cpp`):

```powershell
.\build-msvc-v8\benchmarks\lamp_runtime_benchmark.exe
```

### 2. TypeScript SDK (`sdk-ts/`)

```ts
import { createLampApp, s } from "./sdk-ts/src/index.ts";

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
  handler: async ({ input }) => ({
    id: input.id,
    name: `User ${input.id}`,
  }),
});

await app.start();
const user = await app.invoke("users.get", { id: "1" });
await app.stop();
```

Run the SDK tests and canonical example (Node >= 22.18 / 24):

```powershell
node --test sdk-ts/tests/sdk.test.ts
node sdk-ts/examples/users_service.ts
```

---

## Repository Structure

```text
lamp/
├── CMakeLists.txt           # Root C++20 CMake configuration
├── .github/workflows/ci.yml # Linux + Windows CI workflow
├── benchmarks/              # Release micro-benchmarks (LAMP_BUILD_BENCHMARKS=ON)
├── docs/                    # Architecture, interface spec, roadmap & phase tracking
├── runtime/                 # Modular C++20 runtime & CTest suites
└── sdk-ts/                  # Operation-oriented TypeScript SDK (createLampApp)
```

---

## Documentation

- [docs/CONCEPT.md](docs/CONCEPT.md) — Framework goals and core design principles
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — Native layer map and threading invariants
- [docs/INTERFACE.md](docs/INTERFACE.md) — `createLampApp` TypeScript programming model
- [docs/PRODUCTION.md](docs/PRODUCTION.md) — Build options, security checklist, telemetry endpoints, and release steps
- [docs/PHASE.md](docs/PHASE.md) / [docs/ROADMAP.md](docs/ROADMAP.md) / [docs/CURRENT.md](docs/CURRENT.md) / [docs/TESTS.md](docs/TESTS.md) — Verified build, test counts, and milestone status

---

## Verified Baseline

- **C++20 CTest:** **44/44** suites passed (`100%`, MSVC x64 Release, V8 enabled, `9.29 sec`)
- **TypeScript SDK (`sdk-ts`):** **7/7** tests passed (`node --test sdk-ts/tests/sdk.test.ts`)
- **Benchmarks (`lamp_runtime_benchmark.exe`):** 7 Release micro-benchmarks verified (`exit code 0`)
