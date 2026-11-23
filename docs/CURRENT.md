# Lamp — Current Development State

**As of:** 2026-11-23, verified directly in `D:\Projects\lamp` via MSVC CMake/Ninja build, CTest, and Node 24 TypeScript runs.

## Verified baseline
- Project: `D:\Projects\lamp`
- Platform: Windows AMD64; MSVC Visual Studio 2026 Build Tools (`v14.52`).
- C++20, CMake + Ninja, `build-msvc-v8`, Release.
- V8 enabled (`ON`), native tasks enabled (`ON`), L2 cache disabled (`OFF`).
- **Latest full CTest: 47/47 suites passed, exit code 0, 13.66 seconds.**
- **Server-only SDK (`sdk-ts`, `"name": "lamp"`):** **5/5 passed** (`node --test sdk-ts/tests/sdk.test.ts`, exit code 0).
- **Client SDK (`client-ts`, `"name": "lamp-client"`):** **2/2 passed** (`node --test client-ts/tests/client_binary.test.ts`, exit code 0; live Node TCP `lmp://` and Browser WebSocket `ws://` `LMP1`+`LCB1` E2E against `lamp_server.exe`).
- **Pure-binary C++ server vs HTTP REST (`lamp_transport_test`):** **4/4 passed** — **`4.59x` faster roundtrip** and **`2.62x` less bandwidth** (small message `203B -> 94B`, **`2.16x` smaller**).

## Latest verified changes (Binary Architecture & Split SDKs)
- **Pure C++ Non-REST Runtime Host (`host/` + `runtime/transport/` + `runtime/core/`):**
  - `runtime/transport/lamp_binary_protocol.hpp`: 14-byte `LMP1` multiplexed binary frames + `LCB1` string-deduplicated compact binary codec with raw binary file attachments (`LampFilePayload`, zero base64) + RFC 6455 binary WebSocket upgrade for browser `LMP1` clients. Rejects HTTP REST calls (`404`).
  - `runtime/transport/async_binary_converter.hpp`: `AsyncBinaryStreamConverter` asynchronously streaming JSON + file inputs into binary `LCB1` chunks on `DynamicWorkerPool` -> bounded `StreamChannel`.
  - `runtime/transport/wire_cache.hpp`: `FullCycleWireCache` caching entire serialized binary responses by `(operation_id, Sha256(raw_request_payload))` with configurable TTL clamped by hard `max_ttl_limit`.
  - `runtime/transport/lamp_binary_server.hpp` & `host/lamp_app_server.hpp`: Nonblocking `WSAPoll`/`poll` TCP server (`TCP_NODELAY`) + C++ `DynamicWorkerPool` (`min_threads..max_threads` dynamic scaling) + full-cycle wire cache + `LampV8RuntimeScheduler` (`s.file()` raw `Uint8Array` hydration and `ctx.parallel` subtask splitting via `V8SubtaskIsolatePool` on `DynamicWorkerPool`).
- **Split JSDoc-documented SDK Packages:**
  - `sdk-ts/` (`"name": "lamp"`): Server-side contracts ONLY (`createLampApp`, `s` + `s.file()`, `LampError`, `ctx.parallel`, TTL upper-limit config, `lamp serve` CLI).
  - `client-ts/` (`"name": "lamp-client"`): Dedicated client contracts over `LMP1`/`LCB1` (`node` raw TCP + `browser` binary WebSocket + async streaming JSON/file converter).

## Known notes / deferred items
- Sample external consumer `D:\Projects\lamp-test-client` was updated to `lamp-client/browser` (`ws://` binary WebSocket) and verified (`tsc --noEmit && vite build`).
- Sample Node/MongoDB project `D:\Projects\lamp-test-project` uses Mongoose (`mongoose` npm package, which depends on Node core `net`/`tls` sockets) and therefore remains on its prior Node setup unless native MongoDB bindings are added to the C++ host.
