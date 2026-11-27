# Lamp Project Goals

Lamp is designed to combine the developer ergonomics of TypeScript with the throughput, memory efficiency, and concurrency of a native C++20 server:

1. **Zero-REST Pure Binary Transport (`LMP1` + `LCB1`)**
   - Persistent multiplexed TCP (`lmp://`) and binary WebSocket (`ws://`) connections with minimal framing overhead and native binary file transfer (`s.file()` without base64 inflation).
2. **Modular Two-Layer Routing**
   - Separate request structure (`createLampRouter`) from business logic (`createLampRegistry`), with arbitrary router nesting and scoped error handlers (`createErrorHandler`).
3. **True Parallelism in JavaScript (`ctx.parallel`)**
   - Offload CPU-intensive workloads across C++ `DynamicWorkerPool` V8 isolates without blocking the main event loop.
4. **Native Full-Cycle Wire Caching**
   - Serve repeated requests directly from C++ `FullCycleWireCache` before entering V8, with server-clamped `maxTtlLimitSeconds`.
5. **Turnkey Developer Experience**
   - Scaffold new backends in seconds (`npx create-lamp-app`), generate modules (`npx lamp add <module>`), develop with live watch restarts (`npm run dev`), and compile multi-file TypeScript into a single optimized low-level JS bundle (`lamp build`).
