# Development Phases

## Phase 1 — Core C++20 Runtime & HTTP Foundation (Completed)
- Lifecycle state machine, `DynamicWorkerPool`, `TaskScheduler`, `EventDispatcher`, `ServiceRegistry`
- Incremental HTTP parser, router, middleware, and `HttpAsyncServer`

## Phase 2 — V8 Integration, Serialization, Cache, Security, Streaming & Telemetry (Completed)
- Embedded V8 platform/isolate/context/module loader (`runtime/v8/`)
- Dynamic `Value`, `SchemaRegistry`, `ProtobufCodec`, `JsonCodec` (`runtime/serialization/`)
- Sharded LRU+TTL `LocalOperationCache` & `LayeredOperationCache` (`runtime/cache/`)
- `SecurityGateway`, RBAC/scope/tenant policies, HMAC/SHA-256, rate limiter (`runtime/security/`)
- `StreamChannel`, `StreamHub`, `SseCodec` (`runtime/streaming/`)
- `MetricsRegistry`, W3C `traceparent`, `/healthz` & `/readyz` (`runtime/telemetry/`)

## Phase 3 — Pure-Binary `LMP1/LCB1` Transport & `lamp_server` Host (Completed)
- `LampBinaryServer` over TCP (`lmp://`) and binary WebSocket (`ws://`)
- `FullCycleWireCache` with server-clamped TTL upper limits
- `V8SubtaskIsolatePool` for `ctx.parallel`
- `OriginPolicy` (`allowOrigin`) native enforcement

## Phase 4 — TypeScript SDKs, Routing, CLI Toolchain & Packaging (Completed — `0.4.1`)
- `lamp-app` (`sdk-ts`): `createLampApp`, `createLampRouter`, `createLampRegistry`, `createErrorHandler`, `log` config, `s.*` (including `s.file()`)
- `lamp` CLI: `serve [--watch]`, `build` (multi-file bundler), `add <module[/submodule]>`
- `lamp-client` (`client-ts`): Node TCP and Browser WebSocket binary clients
- `create-lamp-app`: 3-file `src/` scaffolder (`app.ts`, `router.ts`, `registry.ts`)
- Prebuilt platform binary packaging (`npm/`)
