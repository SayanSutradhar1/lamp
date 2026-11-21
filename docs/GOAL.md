# Lamp — Goals and Milestones

## Product destination
Deliver a production-capable, high-performance, operation-oriented backend framework with a C++20/V8 engine and a developer-friendly TypeScript SDK.

**Public factory: `createLampApp`.**

## Current milestone: finish Phase 6
Complete **Transport and Advanced Routing** with verified async HTTP execution, safe completion delivery, lifecycle management, and bounded backpressure.

### Phase 6 acceptance criteria
1. HTTP requests are parsed and routed to logical Lamp operations.
2. The async scheduler accepts operations without blocking connection I/O.
3. Completion tokens reject stale connection generations.
4. Responses preserve HTTP/1.1 request order.
5. Partial socket writes and keep-alive/close semantics are correct.
6. Queue saturation, oversized responses, request limits, deadlines, cancellation and disconnect are handled deterministically.
7. No unsafe cross-thread connection mutation.
8. Server lifecycle has clean start, drain, shutdown, and resource cleanup.
9. Existing synchronous tests remain passing.
10. New integration tests demonstrate a real HTTP request -> async execution -> response path.
11. All CTest suites pass with no regressions.

## Intermediate checkpoints
- **Checkpoint A (verified):** async HTTP connection header and 18 behavioral assertions passed; suite registered.
- **Checkpoint B (current):** async HTTP operation pump draft compiled, but not integrated or behavior-tested.
- **Checkpoint C:** pump integration tests and verified error/backpressure paths.
- **Checkpoint D:** live async server/transport adapter integration.
- **Checkpoint E:** lifecycle, load and regression verification.

## Longer-term v1.0 acceptance
- Real TypeScript `createLampApp` application works end-to-end.
- Multiple transport options or a clear supported transport subset.
- Protobuf-first serialization with documented fallback.
- Native cache, security controls, telemetry, streaming/real-time as planned.
- Performance benchmarks, documentation, packaging, CI and deployment guidance.

Do not mark an entire phase complete based only on compiling a header or passing unit tests that do not exercise the live integration.
