# Lamp Architecture

```text
+-----------------------------------------------------------------------+
| TypeScript Server SDK (lamp-app v0.4.1)                               |
|  - Route Layer    : createLampRouter({ target, registry, nesting })   |
|  - Registry Layer : createLampRegistry({ onError }).add(suffix, def)  |
|  - Error Utility  : createErrorHandler({ log, transform, onError })   |
|  - CLI & Bundler  : lamp serve [--watch] | lamp build | lamp add      |
+-----------------------------------+-----------------------------------+
                                    |
                           Native V8 Host Bridge
                (host/lamp_v8_scheduler.hpp + V8SubtaskIsolatePool)
                                    |
+-----------------------------------+-----------------------------------+
| C++20 Core & Execution Engine                                         |
|  - core::DynamicWorkerPool (elastic thread scaling)                   |
|  - transport::FullCycleWireCache (L1 wire cache w/ TTL upper bound)   |
|  - security::SecurityGateway & core::OriginPolicy                     |
+-----------------------------------+-----------------------------------+
                                    |
+-----------------------------------+-----------------------------------+
| Pure-Binary Non-REST Transport (transport::LampBinaryServer)          |
|  - LMP1 (14-byte multiplexed frame header)                            |
|  - LCB1 (string-interned compact binary codec + native file bytes)    |
|  - TCP (lmp://) and Binary WebSocket (ws://)                          |
+-----------------------------------+-----------------------------------+
                                    |
+-----------------------------------+-----------------------------------+
| Client SDK (lamp-client)                                              |
|  - Node TCP Client (`lamp-client`)                                    |
|  - Browser Binary WebSocket Client (`lamp-client/browser`)            |
+-----------------------------------------------------------------------+
```

## Threading & Execution Invariants

- **Main V8 Isolate:** Runs application bootstrap (`src/app.ts`), registers operation contracts onto `LampAppServer`, and dispatches async/sync operation handlers.
- **Subtask Isolate Pool (`V8SubtaskIsolatePool`):** Stateless pooled V8 isolates backed by `core::DynamicWorkerPool` threads execute `ctx.parallel(...)` subtasks concurrently without lock contention on the main isolate.
- **Full-Cycle Wire Cache (`FullCycleWireCache`):** Keyed by operation ID + canonical input payload; hits return pre-serialized `LCB1` bytes directly from C++ I/O threads without invoking V8.
- **Origin Allowance (`OriginPolicy`):** Configured via `createLampApp({ allowOrigin })` and enforced natively before WebSocket/binary dispatch.
