# runtime/bridge

Connects transport requests and V8 JS handlers to operation descriptors, envelopes, caching, and execution dispatchers.

## Key Components

- `operation_descriptor.hpp` & `operation_registry.hpp` — Operation metadata and registry lookup
- `request_envelope.hpp` & `result_envelope.hpp` — Canonical request/response envelopes
- `execution_dispatcher.hpp` — Synchronous/asynchronous operation dispatch
- `async_operation_coordinator.hpp` & `async_http_operation_pump.hpp` — Async coordination and pumping
- `v8_sdk_host_bridge.hpp` & `v8_operation_binding.hpp` — Native V8 host bridge callbacks
- `operation_cache_interface.hpp`, `http_transport_adapter.hpp`, `async_http_transport_adapter.hpp`
