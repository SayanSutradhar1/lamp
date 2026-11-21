# Lamp — Repository Structure and Discovery

## Known root

```text
D:\Projects\lamp\
├── CMakeLists.txt
├── runtime/
│   ├── CMakeLists.txt
│   ├── core/
│   ├── http/
│   │   ├── CMakeLists.txt
│   │   ├── http_server.hpp
│   │   ├── http_server.cpp
│   │   ├── http_connection.hpp
│   │   ├── http_async_connection.hpp
│   │   ├── http_completion_queue.hpp
│   │   ├── http_parser.hpp
│   │   ├── http_request.hpp
│   │   ├── http_response.hpp
│   │   ├── http_router.hpp
│   │   ├── http_middleware.hpp
│   │   ├── http_route_group.hpp
│   │   └── tests/
│   │       ├── CMakeLists.txt
│   │       └── http_async_connection_test.cpp
│   ├── v8/
│   ├── bridge/
│   │   ├── CMakeLists.txt
│   │   ├── request_envelope.hpp
│   │   ├── result_envelope.hpp
│   │   ├── execution_dispatcher.hpp
│   │   ├── http_transport_adapter.hpp
│   │   ├── async_operation_coordinator.hpp
│   │   ├── async_http_operation_pump.hpp
│   │   └── tests/
│   └── execution/
│       ├── CMakeLists.txt
│       ├── async_execution_contract.hpp
│       ├── async_execution_dispatcher.hpp
│       ├── native_worker_pool.hpp
│       ├── v8_isolate_worker_pool.hpp
│       ├── v8_promise_execution_contract.hpp
│       ├── v8_promise_continuation_scheduler.hpp
│       ├── v8_async_worker_integration.hpp
│       └── tests/
└── build-msvc-v8/                 # Generated CMake/Ninja build
```

This is an **illustrative known-path map**, not a verified exhaustive tree. Some future components are pending, and the exact local checkout is authoritative.

## Discover the real tree (PowerShell)

```powershell
cd D:\Projects\lamp
Get-ChildItem -Force | Select-Object Name, Mode
Get-ChildItem .\runtime -Directory | Select-Object Name
Get-ChildItem .\runtime -Recurse -File |
    Where-Object { $_.FullName -notmatch '[\\/]build-' } |
    Select-Object -ExpandProperty FullName
```

To inspect CMake registrations:

```powershell
Get-Content .\CMakeLists.txt
Get-Content .\runtime\CMakeLists.txt
Get-Content .\runtime\http\CMakeLists.txt
Get-Content .\runtime\bridge\CMakeLists.txt
Get-Content .\runtime\execution\CMakeLists.txt
ctest --test-dir build-msvc-v8 -N
```

If installed, `git status --short` and `git ls-files` help distinguish tracked source from temporary files.

## Working conventions
- C++20 native source under `runtime/`.
- Public headers use includes such as `#include "http/http_request.hpp"` with `runtime/` on the include path.
- Keep source and test files inside their owning module.
- Keep manually compiled `.obj` and `.exe` under `build-msvc-v8/manual-tests/`, **not the repository root**.
- Do not delete unknown root files automatically; inspect before cleanup.
- The TypeScript SDK folder/layout is **not yet verified**. Inspect repository before creating it.
- Avoid duplicating existing modules or inventing missing files from this diagram.
