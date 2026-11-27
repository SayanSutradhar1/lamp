# Lamp C++20 Core Runtime (`runtime/`)

Modular C++20 runtime libraries powering the Lamp native backend server.

## Modules

| Module | Responsibility |
| :--- | :--- |
| [`core/`](core/README.md) | Application lifecycle, `DynamicWorkerPool`, `TaskScheduler`, `EventDispatcher`, `OriginPolicy`, `ServiceRegistry` |
| [`transport/`](transport/README.md) | Pure-binary `LMP1` multiplexed TCP/WebSocket server (`LampBinaryServer`), `LCB1` async converter, `FullCycleWireCache` |
| [`serialization/`](serialization/README.md) | Dynamic `Value`, `SchemaRegistry`, `ProtobufCodec`, `JsonCodec`, `CodecNegotiator` |
| [`bridge/`](bridge/README.md) | Operation contracts (`OperationDescriptor`, `OperationRegistry`), `ExecutionDispatcher`, `V8SdkHostBridge` |
| [`execution/`](execution/README.md) | `NativeWorkerPool`, `V8IsolateWorkerPool`, Promise continuations, cancellation tokens & deadlines |
| [`cache/`](cache/README.md) | `CacheKeyBuilder`, sharded LRU+TTL `LocalOperationCache`, `LayeredOperationCache` |
| [`security/`](security/README.md) | SHA-256 / HMAC-SHA256 tokens, RBAC/scope/tenant policies, rate limiter, input sanitization |
| [`streaming/`](streaming/README.md) | Bounded `StreamChannel`, `StreamHub`, chunked HTTP/SSE codecs |
| [`telemetry/`](telemetry/README.md) | `MetricsRegistry`, W3C `traceparent` `Tracer`, health checks, `TelemetryDispatcher` |
| [`v8/`](v8/README.md) | Embedded V8 platform, isolate/context management, ES module loader, native bindings |
| [`http/`](http/README.md) | Incremental HTTP/1.0–1.1 parser, router, middleware chain, async connection & `HttpAsyncServer` |
