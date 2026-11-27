# Current Project Status (`0.4.1`)

Lamp is at **v0.4.1** across `lamp-app`, `create-lamp-app`, and `@sayansutradhar1/lamp-app-win32-x64`, with `lamp-client` at **v0.2.0**.

## Implemented & Verified

- **C++20 Native Runtime (`runtime/` & `host/`):**
  - `LampBinaryServer` (`LMP1` 14-byte framing + `LCB1` string-interned compact binary over `lmp://` TCP and `ws://` WebSocket)
  - `FullCycleWireCache` with server-enforced `maxTtlLimitSeconds`
  - `DynamicWorkerPool` + `V8SubtaskIsolatePool` (`ctx.parallel`)
  - Native `OriginPolicy` (`allowOrigin`) enforcement
  - Embedded V8 bootstrap synced with `createLampApp`, `createLampRouter`, `createLampRegistry`, `createErrorHandler`, and `log`
- **Server SDK & CLI (`sdk-ts` / `lamp-app@0.4.1`):**
  - Two-layer routing (`createLampRouter`, `createLampRegistry`, `app.registerRouter`) with arbitrary nesting and option inheritance
  - Scoped `createErrorHandler` attachable at registry level and router level
  - Root `log: true` (default) console logging toggle
  - `lamp serve [--watch]`, `lamp build` (multi-file TS -> single optimized JS bundle), and `lamp add <module[/submodule]>`
- **Scaffolder (`create-lamp-app@0.4.1`):**
  - Clean 3-file `src/` scaffold (`app.ts`, `router.ts`, `registry.ts`) with `lamp-app@^0.4.1` only (no `lamp-client` or client script)
- **Client SDK (`client-ts` / `lamp-client@0.2.0`):**
  - Node TCP (`lmp://`) and Browser WebSocket (`ws://`) binary clients with `$lamp.manifest`, `ping()`, `stats()`, and native binary `File`/`Blob` uploads
