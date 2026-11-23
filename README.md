# Lamp

**Lamp** is an operation-oriented backend framework built around a **C++20 native runtime**, an **embedded V8 JavaScript engine**, and a **TypeScript application SDK (`createLampApp`)**.

Applications declare typed logical operations (`users.get`, `orders.create`, etc.) rather than low-level Express-style routes, while C++ handles transport framing, scheduling, caching, Protobuf/JSON serialization, streaming, security enforcement, and telemetry.

---

## Architecture Overview

```text
TypeScript Server SDK (sdk-ts / "lamp": createLampApp / app.register / s.file() / ctx.parallel)
                                  |
                         Native V8 Host Bridge
                 (host/lamp_v8_scheduler.hpp + V8SubtaskIsolatePool)
                                  |
                 Core DynamicWorkerPool & FullCycleWireCache
            (Elastic thread scaling + full response cache w/ TTL cap)
                                  |
              Pure-Binary Non-REST Server (LampBinaryServer)
          (LMP1 14B frame + LCB1 string-interned compact binary & WebSocket)
                                  |
           Client SDK (client-ts / "lamp-client": Node TCP & Browser WS)
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

### 2. Run a Pure-Binary Non-REST Backend & `lamp-client`

Write server contracts in TypeScript (`sdk-ts/examples/live_backend.ts` — package `lamp`):

```ts
import { LampError, createLampApp, s } from "lamp";

const app = createLampApp({
  transport: "lmp1-binary",
  serialization: "lcb1",
  execution: "parallel-dynamic",
  cache: { enabled: true, defaultTtl: 60, maxTtlLimitSeconds: 300 },
});

app.register("users.get", {
  input: s.object({ id: s.string({ minLength: 1 }) }),
  output: s.object({ id: s.string(), name: s.string(), role: s.enum(["member", "admin"]) }),
  cache: { ttl: 60 },
  handler: async ({ input }) => ({
    id: input.id,
    name: `User ${input.id}`,
    role: input.id === "1" ? "admin" : "member",
  }),
});

await app.start();
```

Launch the pure C++20 `lamp_server` runtime:

```powershell
node .\sdk-ts\bin\lamp.ts serve .\sdk-ts\examples\live_backend.ts --port 3000
```

Call operations from Node or Browsers via **`lamp-client`** (`lmp://` TCP or `ws://` binary WebSocket — zero REST endpoints):

```ts
import { createLampClient } from "lamp-client"; // or "lamp-client/browser" in Vite/React

const client = createLampClient({ url: "lmp://127.0.0.1:3000" });
const user = await client.call("users.get", { id: "1" });
```

Run server and client verification suites (Node >= 22.18 / 24):

```powershell
node --test sdk-ts/tests/sdk.test.ts
node --test client-ts/tests/client_binary.test.ts
```

---

## Repository Structure

```text
lamp/
├── CMakeLists.txt           # Root C++20 CMake configuration
├── .github/workflows/ci.yml # Linux + Windows CI workflow
├── benchmarks/              # Release micro-benchmarks (LAMP_BUILD_BENCHMARKS=ON)
├── docs/                    # Architecture, interface spec, roadmap & phase tracking
├── host/                    # Native C++ binary server (`lamp_server`, `LampBinaryServer`)
├── runtime/                 # Modular C++20 runtime (`core`, `transport`, `v8`, etc.)
├── sdk-ts/                  # Server-side contract SDK (`lamp`: `createLampApp`, `s`, `lamp` CLI)
└── client-ts/               # Client-side binary SDK (`lamp-client` Node TCP & Browser WS)
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

- **C++20 CTest:** **47/47** suites passed (`100%`, MSVC x64 Release, V8 enabled, `13.66s`)
- **Transport vs REST (`lamp_transport_test`):** **4/4** passed (**`4.59x` faster** and **`2.62x` less bandwidth**; `2.16x` smaller small payloads)
- **Server SDK (`sdk-ts` / `lamp`):** **5/5** tests passed (`node --test sdk-ts/tests/sdk.test.ts`)
- **Client SDK (`client-ts` / `lamp-client`):** **2/2** live binary E2E tests passed (`node --test client-ts/tests/client_binary.test.ts`)
