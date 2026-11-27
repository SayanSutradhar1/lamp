# Contributing to Lamp

Thank you for your interest in contributing to **Lamp**!

## Repository Layout

- `runtime/` — Modular C++20 runtime libraries (`core`, `transport`, `v8`, `bridge`, `execution`, `cache`, `serialization`, `streaming`, `security`, `telemetry`, `http`)
- `host/` — Native C++20 binary server (`lamp_server`, `LampAppServer`, `LampV8Scheduler`)
- `sdk-ts/` (`lamp-app`) — Server-side TypeScript SDK (`createLampApp`, `createLampRouter`, `createLampRegistry`, `createErrorHandler`, `s`) and `lamp` CLI (`serve`, `build`, `add`)
- `client-ts/` (`lamp-client`) — Node TCP (`lmp://`) and Browser WebSocket (`ws://`) binary `LMP1/LCB1` client SDK
- `create-lamp-app/` — Project scaffolder (`npx create-lamp-app`)
- `adapters/` — Node.js protocol adapter (`@lamp/node-adapter`)
- `benchmarks/` — Native C++20 runtime micro-benchmarks

## Prerequisites

- **C++20 compiler** (MSVC 2022/2026 on Windows, GCC 12+ or Clang 15+ on Linux/macOS) + **CMake 3.20+** + **Ninja**
- **Node.js >= 22.18.0** (uses native TypeScript type stripping for tests and CLI)
- **V8 headers/libs** (optional for core C++ tests via `-DLAMP_ENABLE_V8=OFF`; required for `lamp_server` and V8 suites)

## Running Tests

### TypeScript Packages
```powershell
npm run build --prefix sdk-ts && npm test --prefix sdk-ts
npm run build --prefix create-lamp-app && npm test --prefix create-lamp-app
npm run build --prefix client-ts && npm test --prefix client-ts
```

### C++20 Runtime & Host
```powershell
# Without V8
cmake -S . -B build-ci -DCMAKE_BUILD_TYPE=Release -DLAMP_BUILD_TESTS=ON -DLAMP_ENABLE_V8=OFF
cmake --build build-ci --parallel 4
ctest --test-dir build-ci --output-on-failure

# With V8
ctest --test-dir build-msvc-v8 --output-on-failure
```

## Coding Guidelines

- Keep `sdk-ts` and `client-ts` **zero-runtime-dependency** and compatible with Node's `erasableSyntaxOnly` and `verbatimModuleSyntax`.
- Preserve clean separation between the **route layer** (`createLampRouter`) and **registry layer** (`createLampRegistry`).
- Add or update unit tests in `sdk-ts/tests/`, `create-lamp-app/tests/`, `client-ts/tests/`, or the corresponding C++ `tests/` directory for every change.
- Run all relevant build and test suites before opening a pull request.
