# runtime/core

Core C++20 foundation for Lamp applications: lifecycle state machine, event loop, elastic thread pools, configuration, error/result primitives, and origin policies.

## Key Components

- `application.hpp` / `application.cpp` — Top-level `Application` lifecycle orchestrator
- `dynamic_worker_pool.hpp` — Elastic `DynamicWorkerPool` that scales threads up/down with load
- `worker_pool.hpp` & `task_scheduler.hpp` — Fixed worker pool and delayed/periodic task scheduler
- `event_dispatcher.hpp` — Thread-safe typed event bus
- `lifecycle.hpp` — Startup/shutdown state transitions and hooks
- `origin_policy.hpp` — Normalized CORS/origin allowance evaluator (`deny_all`, `allow_all`, `allow_list`)
- `runtime_manager.hpp` & `runtime_context.hpp` — Service container and runtime context
- `service_registry.hpp`, `config.hpp`, `error.hpp`, `result.hpp` — Shared primitives
