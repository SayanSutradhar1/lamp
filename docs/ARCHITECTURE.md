# Lamp — Native Architecture

## Layer map

```text
TypeScript SDK (planned)
  createLampApp / register(operation) / start
                 |
          Embedded V8 runtime
                 |
      JS <-> C++ operation bridge
  RequestEnvelope / ResultEnvelope
                 |
        Execution dispatcher
         /               \
  Native worker pool   V8 isolate worker pool
         \               /
        Async completion / cancellation
                 |
        HTTP transport adapter
                 |
    HTTP parser / router / connection / server
                 |
              Network
```

This is a **target integration map**. Not every arrow is live in the running server.

## Implemented or observed components

### Core (`runtime/core/`)
Application/runtime foundation, lifecycle, event dispatch, workers, scheduler, service registry. Eight core CTest suites passed in the latest run.

### HTTP (`runtime/http/`)
- `http_method.hpp`, `http_headers.hpp`, `http_request.hpp`, `http_response.hpp`: HTTP data model.
- `http_parser.hpp`: incremental HTTP/1.0/1.1 parser; stops at one completed request and reports bytes consumed.
- `http_router.hpp`: internal route dispatch.
- `http_connection.hpp`: existing synchronous request/response connection.
- `http_server.hpp/.cpp`: existing blocking accept/connection handling; **not yet an async reactor**.
- `http_middleware.hpp`, `http_route_group.hpp`: Phase 6 routing extensions.
- `http_completion_queue.hpp`: bounded completion handoff, with connection ID/generation/request ID tokens.
- `http_async_connection.hpp`: async HTTP protocol state, ordered pipelined responses, partial output consumption, in-flight limit; **no socket ownership**.

### V8 (`runtime/v8/`)
Embedded V8 platform, isolates, contexts, scripts, native bindings, module loader, runtime. Existing V8 suites passed.

### Bridge (`runtime/bridge/`)
- `request_envelope.hpp` / `result_envelope.hpp`: transport-neutral operation data and outcomes.
- `execution_dispatcher.hpp`: synchronous operation dispatch path.
- `http_transport_adapter.hpp`: HTTP request-to-operation adapter, currently synchronous.
- `async_operation_coordinator.hpp`: submits to `IAsyncExecutionScheduler`, tracks outstanding handles, polls and invokes callbacks.
- `async_http_operation_pump.hpp`: **new header-only draft**, compiled in a standalone MSVC header check. Intended coordinator -> completion queue -> async connection handoff; **not yet covered by integration tests or wired into the live server**.

### Execution (`runtime/execution/`)
- `async_execution_contract.hpp`: scheduler interface, task states, priorities, targets, handles, cancellation and futures.
- `native_worker_pool.hpp`: native scheduler.
- `v8_isolate_worker_pool.hpp`: V8 worker isolates; V8 thread affinity is mandatory.
- Promise continuation and V8 async worker integration components.
- `async_execution_dispatcher.hpp`: chooses execution backend.
- Real V8 worker/Promise integration tests were present and passed by the latest 35-suite run; inspect current source for exact behavior before extending it.

## Asynchronous HTTP handoff (in progress)

```text
HTTP owner thread:
  HttpAsyncConnection.receive(bytes)
     -> dispatch callback(token, HttpRequest)
     -> HTTP operation adapter builds RequestEnvelope
     -> AsyncHttpOperationPump.Submit(...)
     -> AsyncOperationCoordinator.Submit(...)
     -> IAsyncExecutionScheduler

Worker/scheduler finishes task

HTTP owner thread:
  pump.Poll() -> coordinator callback
     -> map result to HttpResponse
     -> HttpCompletionQueue.post(...)
  pump.Drain() -> connection.complete(token, response)
  connection.pendingOutput() -> socket send
  connection.consumeOutput(sent_bytes)
```

**Important:** The pump's `Poll()` and `Drain()` must run on the appropriate connection owner thread. A multi-reactor design requires per-owner routing or equivalent. The pump's queue-full direct-delivery fallback and response buffering require further review.

## Known design risks / unfinished integration
1. `HttpAsyncConnection` can stall permanently if a serialized response exceeds `maxPendingWriteBytes`.
2. In-flight request bounds do not automatically bound all memory held by completed responses.
3. Parsing errors after earlier pipelined requests may discard pending valid responses; define policy.
4. Dispatch callbacks can be reentrant; confirm lifecycle and slot invariants.
5. `abort()` does not automatically cancel scheduler tasks; add token-to-task cancellation ownership.
6. `AsyncHttpOperationPump` is not yet tested end-to-end or integrated into `HttpServer`.
7. Existing `HttpServer` uses blocking connections, not a nonblocking event loop.
8. Ensure correct HTTP keep-alive/Connection headers, HEAD framing, shutdown and queue saturation.
9. No claim of production-grade security, performance, or full transport support yet.

## Architectural rules
- Keep operation APIs separate from HTTP routes.
- Preserve binary-safe request/response payloads.
- Respect isolate/thread affinity and never move V8 local handles across threads.
- Define explicit ownership of sockets, tasks, callbacks, and completion queues.
- Never call a blocking operation on a reactor/connection owner thread.
- Reuse existing bridge error/serialization mapping; avoid parallel implementations.
