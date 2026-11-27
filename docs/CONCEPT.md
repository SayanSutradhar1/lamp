# Lamp Concept

**Lamp** is a high-performance, operation-oriented backend framework built around a **C++20 native runtime**, an **embedded V8 JavaScript engine**, and a **TypeScript application SDK (`lamp-app`)**.

Instead of building applications around low-level HTTP routes and Express-style middleware chains, Lamp replaces URL routing with **typed logical operations** (`app.users.get`, `app.users.local_users.list`, `app.compute.parallel`, etc.) organized into two clean layers:

1. **Route Layer (`createLampRouter`)** — Manages target prefixes, module nesting, shared policies (`execution`, `cache`, `auth`, `tags`), and route-level error handling.
2. **Registry Layer (`createLampRegistry`)** — Holds business-logic handlers keyed by relative operation suffixes (`"get"`, `"create"`, `"list"`), input/output schemas (`s.*` including `s.file()`), and registry-level error handling.

Everything below the TypeScript contract layer—binary framing (`LMP1`), compact string-interned serialization (`LCB1`), dynamic worker thread scaling (`DynamicWorkerPool`), full-cycle response caching (`FullCycleWireCache`), origin policy enforcement, and telemetry—runs natively in C++20.
