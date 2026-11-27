# Lamp TypeScript Interface (`lamp-app` v0.4.1)

## 1. Application Bootstrap (`createLampApp`)

```ts
import { createLampApp } from "lamp-app";
import { rootRouter } from "./router.ts";

const app = createLampApp({
  transport: "lmp1-binary",
  serialization: "lcb1",
  execution: "parallel-dynamic",
  log: true, // default: logs operation registrations & invocations
  cache: { enabled: true, defaultTtl: 60, maxTtlLimitSeconds: 300 },
  allowOrigin: (expose, callback) => {
    expose(["http://localhost:5173"]);
    callback();
    return true;
  },
});

app.registerRouter(rootRouter);
await app.start();
```

## 2. Registry Layer (`createLampRegistry`) & Error Handling (`createErrorHandler`)

```ts
import { LampError, createErrorHandler, createLampRegistry, s } from "lamp-app";

const onError = createErrorHandler({
  log: true,
  defaultCode: "EXECUTION_FAILED",
});

export const userRegistry = createLampRegistry({ onError })
  .add("get", {
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
```

## 3. Route Layer (`createLampRouter`) & Nesting

```ts
import { createLampRouter } from "lamp-app";
import { userRegistry } from "./users.registry.ts";
import { localUsersRouter } from "./local_users/local_users.router.ts";

export const userRouter = createLampRouter({
  description: "Users route module",
  target: "users",
  registry: [userRegistry],
  nesting: [localUsersRouter],
  options: { tags: ["users"] },
});
```

When mounted under `rootRouter` (`target: "app"`), operations resolve to `app.users.get` and `app.users.local_users.*`.

## 4. Schemas, Files, and Parallelism

- `s.string()`, `s.number()`, `s.boolean()`, `s.literal()`, `s.enum()`, `s.array()`, `s.object()`, `s.optional()`, `s.file()`
- `ctx.parallel(items, fn)` executes pure subtasks across C++ `DynamicWorkerPool` worker threads.
