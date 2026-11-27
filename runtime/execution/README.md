# runtime/execution

Asynchronous execution dispatch, native thread pools, V8 isolate worker pools, Promise continuation scheduling, and cooperative cancellation.

## Key Components

- `async_execution_dispatcher.hpp` & `async_execution_contract.hpp` — Async execution pipeline with deadlines and cancellation tokens
- `native_worker_pool.hpp` — Native C++ worker pool for CPU-bound tasks
- `v8_isolate_worker_pool.hpp` & `v8_async_worker_integration.hpp` — Isolated V8 worker threads for parallel JS execution
- `v8_promise_continuation_scheduler.hpp` & `v8_promise_execution_contract.hpp` — V8 Promise microtask/continuation bridge
- `v8_worker_async_execution.hpp` — Async worker execution integration
