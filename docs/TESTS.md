# Lamp — Build, Tests, and Verification

## Verified environment
- Windows PowerShell, `D:\Projects\lamp`.
- Visual Studio 2026 Build Tools, x64 developer environment.
- CMake + Ninja build directory: `build-msvc-v8`.
- C++20, V8 enabled.

## Canonical full build and test

```powershell
cd D:\Projects\lamp

cmd /c '"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" && cmake -S . -B build-msvc-v8 && cmake --build build-msvc-v8 --parallel 4'
if ($LASTEXITCODE -ne 0) { throw "Configure/build failed: $LASTEXITCODE" }

ctest --test-dir build-msvc-v8 --output-on-failure
if ($LASTEXITCODE -ne 0) { throw "CTest failed: $LASTEXITCODE" }
```

If a terminal is already initialized for MSVC, the developer-batch prefix is unnecessary.

## Targeted tests

```powershell
ctest --test-dir build-msvc-v8 -L http --output-on-failure
ctest --test-dir build-msvc-v8 -L bridge --output-on-failure
ctest --test-dir build-msvc-v8 -L execution --output-on-failure
ctest --test-dir build-msvc-v8 -R lamp_http_async_connection_test --output-on-failure
ctest --test-dir build-msvc-v8 -N
```

CTest counts **test executables/suites**, not the number of individual assertions inside them.

## Latest verified full CTest (2026-11-23)
- **47/47 suites passed** (`100%`)
- **Build exit code: 0**
- **CTest exit code: 0**
- **Total test time: 13.66 seconds**
- Host: 1 pure-binary TCP/WebSocket server suite (`lamp_app_server_test`: `LMP1`/`LCB1` roundtrips, REST rejection `404`, `s.file()` binary upload, `FullCycleWireCache` hit & `5000ms` TTL cap, `ctx.parallel` subtask execution).
- Transport (`lamp_transport_test`): **4/4** (`CompactBinaryAndBandwidthSavings` `2.16x` smaller, `AsyncBinaryStreamingConversion`, `FullCycleWireCacheAndTtlUpperLimit`, `BinaryServerSpeedAndBandwidthVsRest` **`4.59x` faster & `2.62x` less bandwidth**).
- Core: **9 suites** (includes `lamp.core.dynamic_worker_pool` — 2/2 elastic scaling + parallel subtask tests).
- Server SDK (`node --test .\sdk-ts\tests\sdk.test.ts`): **5/5 tests passed** (`lamp` server-only contracts + `s.file()` + `ctx.parallel` + TTL upper limit).
- Client SDK (`node --test .\client-ts\tests\client_binary.test.ts`): **2/2 tests passed** (async binary request stream chunking + live C++ `lamp_server.exe` TCP & WebSocket binary E2E).
- HTTP: 11 suites (`lamp_http_async_server_test` added).
- V8-specific component: 16 labeled tests (8 `runtime/v8/` unit suites + 8 V8 execution/bridge/SDK integration suites).
- Bridge: 5 labeled suites (`lamp_bridge_integration`, `lamp_async_operation_coordinator`, `lamp_async_http_operation_pump`, `lamp_v8_async_http_pump_integration`, `lamp_v8_sdk_host_bridge`).
- Execution-related: 11 suites.
- Core: 8 suites.
- Serialization: 1 suite (`lamp_serialization_test` — 9/9 internal assertions passed).
- Cache: 1 suite (`lamp_cache_test` — 8/8 internal assertions passed).
- Streaming: 1 suite (`lamp_streaming_test` — 8/8 internal assertions passed).
- Security: 1 suite (`lamp_security_test` — 7/7 internal assertions passed).
- Telemetry: 1 suite (`lamp_telemetry_test` — 4/4 internal assertions passed).
- TypeScript SDK (`node --test .\sdk-ts\tests\sdk.test.ts`): **7/7 tests passed** (`207.9ms`).
- Runtime micro-benchmarks (`lamp_runtime_benchmark.exe`): 7 benchmarks completed in Release build (exit code 0).
- Async HTTP connection (`lamp_http_async_connection_test.exe`): 24/24 internal assertions passed.
- Async HTTP server (`lamp_http_async_server_test.exe`): 10/10 internal assertions passed.
- Async operation coordinator (`lamp_async_operation_coordinator_test.exe`): 17/17 internal assertions passed.
- Async HTTP operation pump & adapter (`lamp_async_http_operation_pump_test.exe`): 14/14 internal assertions passed.
- V8 async HTTP pump integration (`lamp_v8_async_http_pump_integration_test.exe`): 2/2 passed (includes live TCP mixed Native + V8 Promise test).

## Registered HTTP suites
1. `lamp_http_parser_test`
2. `lamp_http_router_test`
3. `lamp_http_connection_test`
4. `lamp_http_server_test`
5. `lamp_http_middleware_test`
6. `lamp_http_route_group_test`
7. `lamp_http_completion_queue_test`
8. `lamp_http_async_connection_test`
9. `lamp_http_async_server_test`

## Registered bridge suites
- `lamp_bridge_integration`
- `lamp_async_operation_coordinator`
- `lamp_async_http_operation_pump`
- `lamp_v8_sdk_host_bridge`

## Execution-related suites observed
- `lamp_native_worker_pool`
- `lamp_cancellation_propagation`
- `lamp_v8_isolate_worker_pool`
- `lamp_v8_isolate_worker_pool_lifecycle`
- `lamp_v8_promise_execution_contract`
- `lamp_v8_promise_continuation_scheduler`
- `lamp_v8_async_worker_integration`
- `lamp_async_execution_dispatcher`
- `lamp_v8_isolate_worker_pool_promise_integration`
- `lamp_v8_async_http_pump_integration`

Additional tests exist in other modules; use `ctest -N` for the exact current inventory.

## Next planned verification (Post-v1.0)
- Run remote Ubuntu & Windows matrix workflows defined in `.github/workflows/ci.yml`.
- Keep all 44 CTest suites + `sdk-ts` test suite green on future changes.

## Test quality rules
- Tests must assert behavior, not just include headers.
- Prefer deterministic mock schedulers for unit tests.
- Add real socket/V8 integration tests for claims about live execution.
- Preserve existing tests; do not silently weaken assertions.
- Register tests in the owning module's CMake files.
- Report commands, pass/fail counts, and any limitations accurately.
- Never claim a header compile check proves runtime correctness.

## Temporary build artifacts
Keep manual checks under `build-msvc-v8/manual-tests/`, not the project root. Inspect files before deleting them.
