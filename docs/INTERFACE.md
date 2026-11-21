# Lamp — Intended TypeScript Interface

> **Status: design target, NOT an implemented SDK.**

## Canonical application shape

```ts
import { createLampApp } from "lamp";

const app = createLampApp({
  transport: "auto",
  serialization: "protobuf",
  execution: "adaptive",
  cache: { enabled: true },
});

app.register("users.get", {
  input: UserInputSchema,
  output: UserOutputSchema,
  cache: { ttl: 60 },
  handler: async ({ input }) => {
    return getUserById(input.id);
  },
});

app.start();
```

`UserInputSchema`, `UserOutputSchema`, and `getUserById` are illustrative application-defined symbols.

## Required developer experience
1. Import **`createLampApp`** from `lamp`. Do **not** rename it to `createServer`.
2. Configure transports, serialization, execution policy, and cache at application creation.
3. Register **logical operation names** such as `users.get`.
4. Declare input/output schemas for validation and typing.
5. Implement sync or async TypeScript handlers.
6. Allow per-operation cache and execution policy where appropriate.
7. Start and stop the application through an explicit lifecycle.
8. Keep transport routing, serialization conversion, scheduling, and HTTP framing internal.

## Conceptual execution path

```text
Client request
  -> transport adapter (HTTP initially; others later)
  -> operation lookup
  -> input decoding + validation
  -> RequestEnvelope
  -> execution dispatcher / native or V8 worker
  -> ResultEnvelope
  -> output validation + serialization
  -> transport response
```

## Important distinctions
- `app.register("users.get", ...)` is the public programming model.
- Internal `HttpRouter` routes and `HttpTransportAdapter` exist, but should not define the public SDK's shape.
- `transport: "auto"`, `serialization: "protobuf"`, and `execution: "adaptive"` are **proposed configuration values**, not verified runtime capabilities.
- Exact schema library, generated types, error handling, auth API, and transport mapping are **to be designed**. Do not invent them as existing interfaces.

## SDK acceptance criteria
- TypeScript-first types and useful editor inference.
- Consistent errors, cancellation/deadline semantics, and request context.
- Working bridge from TypeScript handlers to the embedded V8/native execution system.
- Tests demonstrating real application registration, dispatch, serialization, and shutdown.
