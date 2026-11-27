# Repository & Project Structure

## Monorepo Layout (`lamp/`)

```text
lamp/
├── CMakeLists.txt             # Root C++20 CMake configuration
├── LICENSE                    # MIT License
├── CONTRIBUTING.md            # Developer setup & contribution guide
├── README.md                  # Project overview & quick start
├── adapters/                  # Protocol adapters (@lamp/node-adapter)
├── benchmarks/                # C++20 micro-benchmarks
├── client-ts/                 # lamp-client (Node TCP & Browser WS binary client)
├── create-lamp-app/           # npx create-lamp-app scaffolder CLI
├── docs/                      # Architecture, interface, roadmap, and test docs
├── host/                      # Native C++20 binary server (lamp_server)
├── npm/                       # Platform-specific prebuilt binary packages
├── runtime/                   # Modular C++20 runtime libraries
│   ├── bridge/                # Operation descriptors, registries, V8 host bridge
│   ├── cache/                 # Sharded LRU+TTL operation caches
│   ├── core/                  # Lifecycle, DynamicWorkerPool, OriginPolicy, config
│   ├── execution/             # Async dispatcher, V8IsolateWorkerPool, promises
│   ├── http/                  # Incremental HTTP parser, router, HttpAsyncServer
│   ├── security/              # Auth policies, HMAC/SHA-256, rate limiter
│   ├── serialization/         # Value, SchemaRegistry, Protobuf & JSON codecs
│   ├── streaming/             # StreamChannel, StreamHub, chunked/SSE codecs
│   ├── telemetry/             # MetricsRegistry, W3C tracing, health probes
│   ├── transport/             # LampBinaryServer (LMP1/LCB1), FullCycleWireCache
│   └── v8/                    # Embedded V8 platform, isolate, context, bindings
└── sdk-ts/                    # lamp-app server SDK & lamp CLI (serve, build, add)
```

## Scaffolded Application Layout (`npx create-lamp-app`)

```text
my-server/
├── package.json               # dev (--watch), build, start, typecheck
├── tsconfig.json
└── src/
    ├── app.ts                 # Main bootstrap (createLampApp, log: true)
    ├── router.ts              # Root router (target: "app")
    ├── registry.ts            # Root registry (ping + createErrorHandler)
    └── modules/               # Created via `npx lamp add <module[/submodule]>`
        └── user/
            ├── user.router.ts
            └── user.registry.ts
```
