# runtime/v8

Embedded V8 JavaScript engine integration for C++20.

## Key Components

- `v8_platform.hpp` & `v8_runtime.hpp` — V8 platform initialization and runtime lifecycle
- `v8_isolate.hpp` & `v8_context.hpp` — Isolate and context RAII wrappers
- `v8_script_engine.hpp` & `v8_module_loader.hpp` — Script compilation and ES module resolution
- `v8_value.hpp` & `v8_native_binding.hpp` — Zero-copy/typed conversion between C++ values and V8 handles, plus native host bindings
