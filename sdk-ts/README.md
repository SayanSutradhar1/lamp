# lamp-app

Server-side TypeScript contract SDK and CLI for the **Lamp** C++20 + embedded V8 binary runtime.

## Installation

```bash
npm install lamp-app
```

## Quick Example

```ts
import {
  LampError,
  createErrorHandler,
  createLampApp,
  createLampRegistry,
  createLampRouter,
  s,
} from "lamp-app";

const onError = createErrorHandler({ log: true });

const userRegistry = createLampRegistry({ onError }).add("get", {
  description: "Fetch user by ID",
  input: s.object({ id: s.string({ minLength: 1 }) }),
  output: s.object({ id: s.string(), name: s.string() }),
  cache: { ttl: 60 },
  handler: ({ input }) => {
    if (input.id === "missing") {
      throw new LampError("User not found", {
        code: "NOT_FOUND",
        identifier: "USER_NOT_FOUND",
      });
    }
    return { id: input.id, name: "Ada Lovelace" };
  },
});

const userRouter = createLampRouter({
  description: "Users module",
  target: "users",
  registry: [userRegistry],
});

const app = createLampApp({
  transport: "lmp1-binary",
  serialization: "lcb1",
  execution: "parallel-dynamic",
  log: true,
  cache: { enabled: true, defaultTtl: 60, maxTtlLimitSeconds: 300 },
});

app.registerRouter(userRouter);
await app.start();
```

## CLI Commands

- `lamp serve <entry.ts|entry.js> [--port 3000] [--watch|-w]` — Bundles and runs the app on the native `lamp_server` C++20 runtime (with optional recursive watch & auto-restart).
- `lamp build [src/app.ts] [--out dist/app.js]` — Compiles multi-file TypeScript modules into a single dependency-optimized low-level JS bundle.
- `lamp add <module[/submodule]>` — Scaffolds `<name>.router.ts` and `<name>.registry.ts` and auto-registers into the parent router.

## License

[MIT](LICENSE)
