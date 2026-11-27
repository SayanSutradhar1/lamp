# Lamp Native Host (`host/`)

The native C++20 binary runtime host (`lamp_server`) that executes Lamp TypeScript/JavaScript applications inside an embedded V8 engine.

## Key Files

- `lamp_server_main.cpp` — CLI entrypoint (`--entry`, `--host`, `--port`, `--min-threads`, `--max-threads`, `--max-cache-ttl-ms`, `--max-cache-entries`)
- `lamp_app_server.hpp` — `LampAppServer` orchestrating `LampBinaryServer` (`LMP1` + `LCB1` over TCP and binary WebSocket), `$lamp.manifest`, `$lamp.stats`, `FullCycleWireCache`, and `DynamicWorkerPool`
- `lamp_v8_scheduler.hpp` — Embedded V8 bootstrap (`createLampApp`, `createLampRouter`, `createLampRegistry`, `createErrorHandler`, `s`) and `V8SubtaskIsolatePool` for `ctx.parallel`
- `CMakeLists.txt` — Builds `lamp_server` when `LAMP_ENABLE_V8=ON`
