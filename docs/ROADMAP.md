# Lamp Roadmap

## Completed Milestones (`0.1.0` -> `0.4.1`)
- Modular C++20 runtime (`core`, `transport`, `serialization`, `bridge`, `execution`, `cache`, `security`, `streaming`, `telemetry`, `v8`, `http`)
- Pure-binary `LMP1/LCB1` TCP & WebSocket server (`lamp_server`)
- Full-cycle wire cache + elastic `DynamicWorkerPool` (`ctx.parallel`)
- TypeScript `lamp-app` & `lamp-client` SDKs
- Two-layer routing (`createLampRouter` + `createLampRegistry` + `app.registerRouter`)
- Scoped `createErrorHandler`, `log` config, `lamp add`, `lamp build`, `lamp serve --watch`
- `create-lamp-app` scaffolder & Node adapter (`@lamp/node-adapter`)

## Upcoming Focus
1. **Cross-Platform Binary Releases:** Automated CI builds and npm publishing for `linux-x64`, `linux-arm64`, `darwin-x64`, and `darwin-arm64` alongside `win32-x64`.
2. **Interactive Dev Inspector (`lamp studio` / `lamp call`):** Terminal & browser-based operation explorer driven by `$lamp.manifest`.
3. **Streaming Operations over `LMP1`:** First-class server-to-client and bidirectional binary streaming handlers in `lamp-app` and `lamp-client`.
4. **Distributed L2 Cache Adapter:** Optional Redis/Valkey backend implementing `LayeredOperationCache` for multi-instance deployments.
