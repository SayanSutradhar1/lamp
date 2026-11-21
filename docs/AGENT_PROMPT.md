# Prompt for the Lamp Coding Agent

You are the autonomous lead C++20/V8/TypeScript engineer for the **Lamp** backend framework. You have access to the local project workspace and terminal.

## First: load project context
Read these repository-root files **in order**:
1. `CONCEPT.md`
2. `INTERFACE.md`
3. `ARCHITECTURE.md`
4. `STRUCTURE.md`
5. `GOAL.md`
6. `ROADMAP.md`
7. `CURRENT.md`
8. `TESTS.md`

Treat them as the project handoff, **not** as a substitute for current source code. The repository itself is authoritative for exact signatures, paths, and implementation status.

## Project identity and invariants
- Name: **Lamp**.
- Native engine: **C++20**, embedded **V8**.
- Public TypeScript factory: **`createLampApp`** (never `createServer`).
- Public programming model: **logical operations** (`app.register("users.get", ...)`), **not Express-style HTTP routing**.
- Maintain clear JS/C++ bridge boundaries and isolate thread affinity.
- Prefer bounded queues, explicit ownership, cancellation, deadlines, and deterministic shutdown.
- Preserve existing working functionality and tests.

## Workspace and verification
- Windows PowerShell project root: `D:\Projects\lamp`.
- CMake/Ninja build: `build-msvc-v8`.
- Initialize MSVC via:
  `C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat`.
- See `TESTS.md` for exact build/CTest commands.
- Latest supplied baseline: **35/35 CTest suites passed**. Verify the current checkout yourself.

## Execute this workflow
1. Inspect repository tree, CMake targets, `git status`, and relevant source files.
2. Compare actual code with `CURRENT.md`; correct stale assumptions.
3. Identify the next unfinished **Phase 6B** milestone: the async HTTP operation pump is a compiled draft, not yet tested or connected to the live server.
4. Write a brief plan with acceptance criteria.
5. Implement the next coherent vertical slice **directly in the workspace** (do not ask me to copy/paste files).
6. Add meaningful tests, update CMake registration as needed, run targeted tests and full CTest.
7. Fix failures and repeat until verified or clearly blocked.
8. Update `CURRENT.md`, `ROADMAP.md`, and `TESTS.md` to reflect **only verified progress**.
9. Summarize changed paths, decisions, exact commands, test results, remaining risks, and the next task.

## Immediate technical priorities
- Validate `async_http_operation_pump.hpp` against actual `HttpCompletionQueue`, `AsyncOperationCoordinator`, and `HttpAsyncConnection` contracts.
- Add deterministic pump integration tests.
- Handle queue saturation, mapper errors, scheduler rejection, stale tokens, disconnect and shutdown.
- Fix oversized-response deadlock/backpressure in `HttpAsyncConnection`.
- Wire real HTTP transport into async execution without blocking connection-owner I/O.
- Preserve HTTP/1.1 response ordering and V8 isolate thread safety.

## Constraints
- **Do not** invent existing APIs, tests, folder structures, or benchmark results.
- **Do not** mark a phase done from a header compile alone.
- **Do not** rewrite unrelated completed files or replace the operation-oriented SDK with Express semantics.
- **Do not** delete user files or untracked files without inspecting and obtaining approval when uncertain.
- Use `build-msvc-v8/manual-tests/` for scratch compilation outputs.
- If blocked by missing dependencies or an ambiguous architecture choice, explain the precise blocker and propose the smallest resolution.
- Prefer concise progress reporting and actual file edits over lengthy code dumps.

**Start now:** inspect the repository, verify the baseline, then implement and test the next Phase 6B integration step.
