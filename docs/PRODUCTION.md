# Production Guide

## 1. Building a Production Bundle

Inside any Lamp project scaffolded with `create-lamp-app`:
```powershell
npm run typecheck   # Strict TypeScript verification (tsc --noEmit)
npm run build       # Bundles src/app.ts + all modules into dist/app.js
npm start           # Runs `lamp serve dist/app.js --port 3000`
```

Set `log: false` in `createLampApp({ log: false })` if you want to suppress per-request console logs in high-throughput production environments.

## 2. Native C++20 Build Options (`CMakeLists.txt`)

- `-DCMAKE_BUILD_TYPE=Release` — Enables `-O2`/`-O3` optimizations
- `-DLAMP_ENABLE_V8=ON|OFF` — Enables embedded V8 runtime & `lamp_server`
- `-DLAMP_BUILD_TESTS=ON|OFF` — Builds CTest unit & integration suites
- `-DLAMP_BUILD_BENCHMARKS=ON|OFF` — Builds `lamp_runtime_benchmark`

## 3. Runtime Tuning Flags (`lamp serve`)

- `--host <addr>` (default `127.0.0.1`)
- `--port <port>` (default `3000`)
- `--min-threads <n>` / `--max-threads <n>` — Bounds for `DynamicWorkerPool`
- `--max-cache-ttl-ms <ms>` — Hard server-side upper cap for `FullCycleWireCache`
- `--max-cache-entries <n>` — Maximum cached wire responses

## 4. Security Checklist

- Configure `allowOrigin` explicitly in production (avoid `allowOrigin: true` for browser-facing services unless intentional; use `(expose, callback) => boolean` with exact origins).
- Enable `security: { defaultDeny: true, jwtSecret: ... }` for authenticated operations.
- Attach `createErrorHandler({ includeStack: false })` at the root router or module registries to ensure stack traces are not leaked to clients.
