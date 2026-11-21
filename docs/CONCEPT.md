# Lamp — Concept

## What Lamp is
Lamp is a planned high-performance backend framework: a **C++20 native runtime** with an **embedded V8 JavaScript engine**, exposed through a **TypeScript SDK**.

Developers should write TypeScript operation handlers without having to manage sockets, worker pools, serialization, or native resource ownership.

## Problems Lamp aims to solve
1. **Backend overhead:** Keep transport, scheduling, and other hot paths native where practical.
2. **Fragmented execution:** Run native and JavaScript operations through a consistent request/result contract.
3. **Async complexity:** Support asynchronous work, cancellation, deadlines, bounded queues, and deterministic shutdown.
4. **Transport coupling:** Separate logical operations from HTTP paths and other transport-specific details.
5. **Serialization cost:** Target protobuf-first operation payloads, with JSON fallback where needed.
6. **Repeated work:** Add native caching and efficient execution paths.
7. **Scaling and safety:** Apply backpressure, resource limits, and explicit ownership rather than unbounded threads or buffers.
8. **Developer experience:** Provide a simple, typed TypeScript application API on top of a complex native runtime.
9. **Operational reliability:** Eventually provide security, observability, streaming, and production deployment support.

## Design principles
- **Operation-oriented**, not Express-like.
- C++ owns runtime infrastructure; TypeScript defines application behavior.
- Protocol adapters translate external requests to internal operation envelopes.
- V8 isolates are thread-affine; never access V8 handles from unrelated threads.
- Resource limits and error paths are first-class.
- Tests establish correctness; successful compilation alone does not.
- Performance is a **goal**, not an established benchmark result.

## Non-goals for the current implementation
- Do not present Lamp as production-ready or v1.0.
- Do not assume HTTP/2, HTTP/3, protobuf, caching, or the TypeScript SDK are already implemented.
- Do not expose internal HTTP routing as the primary TypeScript programming model.
