# Lamp Benchmarks (`benchmarks/`)

Release-mode C++20 micro-benchmarks measuring `DynamicWorkerPool`, `FullCycleWireCache`, `LMP1/LCB1` binary framing, and runtime dispatch overhead.

## Building & Running

```powershell
cmake -S . -B build-msvc-v8 -DCMAKE_BUILD_TYPE=Release -DLAMP_BUILD_BENCHMARKS=ON
cmake --build build-msvc-v8 --target lamp_runtime_benchmark --parallel 4
.\build-msvc-v8\benchmarks\lamp_runtime_benchmark.exe
```

## Key Files

- `lamp_runtime_benchmark.cpp` — C++20 benchmark suite
- `CMakeLists.txt` — Benchmark target definition
