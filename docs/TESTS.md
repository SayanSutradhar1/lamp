# Verification & Test Suites

## TypeScript Suites (Node >= 22.18)

### Server SDK (`sdk-ts` / `lamp-app`)
```powershell
npm run build --prefix sdk-ts
npx --prefix sdk-ts tsc -p sdk-ts/tsconfig.json --noEmit
npm test --prefix sdk-ts
```
Covers (`13/13` tests in `sdk-ts/tests/sdk.test.ts`):
- App lifecycle transitions and `onStart` / `onStop` hooks
- Dotted operation IDs, duplicates, and TTL upper limit clamping
- `s.file()` binary validation and `ctx.files` hydration
- `ctx.parallel` delegation to `NativeHostBridge.runParallel`
- `allowOrigin` policy normalization and native bridge sync
- `createLampRegistry` & `createLampRouter` flat/nested resolution, suffix validation, option precedence, and circular nesting detection
- `createErrorHandler` at registry level and route level
- `log: true` / `log: false` console logging behavior
- `bundleEntryForEmbeddedV8` multi-file TypeScript bundling

### Scaffolder (`create-lamp-app`)
```powershell
npm run build --prefix create-lamp-app
npm test --prefix create-lamp-app
```
Covers (`2/2` tests in `create-lamp-app/tests/create_lamp_app.test.ts`):
- 3-file `src/` scaffold (`app.ts`, `router.ts`, `registry.ts`), `lamp-app` dependency only, `dev` (`--watch`), `build`, `start` scripts
- Refusal to overwrite non-empty target directories

### Client SDK (`client-ts` / `lamp-client`)
```powershell
npm run build --prefix client-ts
npm test --prefix client-ts
```
Covers live binary `LMP1/LCB1` framing, multiplexing, and round-trip client calls.

## C++20 Suites (CMake + CTest)

```powershell
# Core C++ suites without V8
cmake -S . -B build-ci -DCMAKE_BUILD_TYPE=Release -DLAMP_BUILD_TESTS=ON -DLAMP_ENABLE_V8=OFF
cmake --build build-ci --parallel 4
ctest --test-dir build-ci --output-on-failure

# Full build with embedded V8
ctest --test-dir build-msvc-v8 --output-on-failure
```
