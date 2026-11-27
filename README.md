# Lamp

**Lamp** (`0.4.1`) is a high-performance, operation-oriented backend framework built around a **C++20 native runtime**, an **embedded V8 JavaScript engine**, and a **TypeScript application SDK (`lamp-app`)**.

Instead of low-level REST/HTTP endpoints, applications declare typed logical operations (`app.users.get`, `app.users.local_users.list`, etc.) organized into modular **Route** (`createLampRouter`) and **Registry** (`createLampRegistry`) layers. The native C++20 runtime handles `LMP1/LCB1` binary framing, dynamic thread scaling, full-cycle wire caching, schema validation, security, and telemetry.

---

## Architecture Overview

```text
TypeScript Server SDK (sdk-ts / "lamp-app" v0.4.1)
  ├── Route Layer    : createLampRouter({ target, registry, nesting, options })
  ├── Registry Layer : createLampRegistry({ onError }).add("get", { ... })
  ├── Error Handler  : createErrorHandler({ log, defaultCode, transform, onError })
  └── CLI Toolchain  : lamp serve [--watch] | lamp build | lamp add <module>
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

---

## Quick Start (Scaffolding a New App)

### 1. Create a Lamp Backend
```powershell
npx create-lamp-app@latest my-server
cd my-server
```

Generated structure:
```text
my-server/
├── package.json         # dev (watch), build, start, typecheck
├── tsconfig.json
└── src/
    ├── app.ts           # Main bootstrap (createLampApp, log: true, registerRouter)
    ├── router.ts        # Root router (rootRouter, target: "app")
    └── registry.ts      # Root registry (ping + createErrorHandler)
```

### 2. Add Feature Modules & Submodules
```powershell
npx lamp add users               # creates src/modules/users/users.{router,registry}.ts & auto-registers in src/router.ts
npx lamp add users/local_users   # creates nested submodule & auto-registers in users.router.ts
```

### 3. Run in Watch Mode, Build, and Serve
```powershell
npm run dev       # lamp serve src/app.ts --port 3000 --watch (auto-restarts on file changes)
npm run build     # lamp build src/app.ts --out dist/app.js (single optimized low-level JS bundle)
npm start         # lamp serve dist/app.js --port 3000
npm run typecheck # tsc --noEmit
```

### 4. Connect from Node or Browser (`lamp-client`)
```ts
import { createLampClient } from "lamp-client"; // or "lamp-client/browser" for ws://

const client = createLampClient({ url: "lmp://127.0.0.1:3000" }); // or "ws://127.0.0.1:3000"
const manifest = await client.manifest();
const user = await client.call("app.users.get", { id: "u_1" });
```

---

## Key Features (`0.4.1`)

- **Two-Layer Lamp Routing:** `createLampRouter` (route manager, target prefixes, arbitrary `nesting`, inherited options) + `createLampRegistry` (business logic keyed by relative suffix) + `app.registerRouter(router, options)`.
- **Scoped Error Handling:** `createErrorHandler(config)` attachable at both registry level and route level with normalization, logging, transforms, and callbacks.
- **Console Operation Logging:** `log: true` (default) on `createLampApp({ log })` logs operation registrations and invocations; set `log: false` to silence.
- **CLI Scaffolding & Bundling:** `npx lamp add <module[/sub]>`, `lamp serve --watch`, and `lamp build` (bundles multi-file TypeScript into a single dependency-optimized low-level JS file).
- **Native Binary File Transfer:** `s.file()` streams `Uint8Array` / Web `File` / `Blob` payloads directly in `LCB1` frames without base64 inflation.
- **Elastic Multithreading (`ctx.parallel`):** Offloads CPU-bound subtasks across C++ `DynamicWorkerPool` V8 isolates.
- **Full-Cycle Wire Cache:** Native `FullCycleWireCache` serves cached responses directly from C++ with server-clamped `maxTtlLimitSeconds`.
- **Origin Policy (`allowOrigin`):** Supports `false` (deny all), `true` (allow all), or `(expose, callback) => boolean` allow-lists enforced natively.

---

## C++20 Build & Tests (CMake + Ninja)

```powershell
# Core C++ tests (without V8)
cmake -S . -B build-ci -DCMAKE_BUILD_TYPE=Release -DLAMP_BUILD_TESTS=ON -DLAMP_BUILD_BENCHMARKS=ON -DLAMP_ENABLE_V8=OFF
cmake --build build-ci --parallel 4
ctest --test-dir build-ci --output-on-failure

# Full build with embedded V8
ctest --test-dir build-msvc-v8 --output-on-failure
```

Run TypeScript SDK & scaffolder test suites (Node >= 22.18):
```powershell
npm test --prefix sdk-ts
npm test --prefix create-lamp-app
npm test --prefix client-ts
```

---

## Repository Structure

```text
lamp/
├── CMakeLists.txt           # Root C++20 CMake configuration
├── LICENSE                  # MIT License
├── CONTRIBUTING.md          # Contribution & test guide
├── .github/workflows/ci.yml # Linux + Windows CI workflow
├── adapters/                # Node.js adapter (@lamp/node-adapter)
├── benchmarks/              # C++20 runtime micro-benchmarks
├── client-ts/               # Client-side binary SDK (lamp-client: Node TCP & Browser WS)
├── create-lamp-app/         # Project scaffolder CLI (create-lamp-app)
├── docs/                    # Architecture, interface spec, roadmap & phase tracking
├── host/                    # Native C++ binary server (lamp_server, LampAppServer)
├── npm/                     # Platform-specific prebuilt native binary packages
├── runtime/                 # Modular C++20 runtime (core, transport, v8, bridge, etc.)
└── sdk-ts/                  # Server-side SDK (lamp-app) & lamp CLI (serve, build, add)
```

---

## Documentation & License

- [docs/CONCEPT.md](docs/CONCEPT.md) — Framework goals and core design principles
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — Native layer map and threading invariants
- [docs/INTERFACE.md](docs/INTERFACE.md) — `lamp-app` TypeScript programming model
- [docs/STRUCTURE.md](docs/STRUCTURE.md) — Repository & scaffolded project layout
- [docs/PRODUCTION.md](docs/PRODUCTION.md) — Build options, security checklist, telemetry, and release steps
- [docs/PHASE.md](docs/PHASE.md) / [docs/ROADMAP.md](docs/ROADMAP.md) / [docs/CURRENT.md](docs/CURRENT.md) / [docs/TESTS.md](docs/TESTS.md) — Milestone status and verified test suites
- Licensed under the [MIT License](LICENSE).
