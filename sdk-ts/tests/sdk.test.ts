import assert from "node:assert/strict";
import test from "node:test";

import {
  LampError,
  createLampApp,
  isLampError,
  isValidOperationId,
  s,
  type NativeHostBridge,
  type RegisteredOperationMetadata,
} from "../src/index.ts";

test("createLampApp enforces lifecycle transitions and hooks", async () => {
  const events: string[] = [];
  const app = createLampApp({
    transport: "auto",
    serialization: "protobuf",
    execution: "adaptive",
    cache: { enabled: true },
  });

  app
    .onStart(async () => {
      events.push("start:1");
    })
    .onStop(async () => {
      events.push("stop:1");
    })
    .onStop(async () => {
      events.push("stop:2");
    })
    .register("system.health", {
      output: s.object({ status: s.literal("ok") }),
      handler: () => ({ status: "ok" as const }),
    });

  assert.equal(app.state(), "created");

  await assert.rejects(
    () => app.invoke("system.health", undefined),
    (err: unknown) => isLampError(err) && err.code === "FAILED_PRECONDITION"
  );

  const info = await app.start();
  assert.equal(app.state(), "running");
  assert.equal(info.transport, "auto");
  assert.equal(info.serialization, "protobuf");
  assert.equal(info.execution, "adaptive");
  assert.deepEqual(info.operations, ["system.health"]);

  assert.throws(
    () =>
      app.register("late.op", {
        handler: () => null,
      }),
    (err: unknown) => isLampError(err) && err.code === "FAILED_PRECONDITION"
  );

  const health = await app.invoke<{ status: "ok" }>("system.health");
  assert.deepEqual(health, { status: "ok" });

  await app.stop();
  assert.equal(app.state(), "stopped");
  assert.deepEqual(events, ["start:1", "stop:2", "stop:1"]);
});

test("register validates logical operation IDs and rejects duplicates", () => {
  assert.equal(isValidOperationId("users.get"), true);
  assert.equal(isValidOperationId("analytics_v2.run-job"), true);
  assert.equal(isValidOperationId(""), false);
  assert.equal(isValidOperationId(".users"), false);
  assert.equal(isValidOperationId("users..get"), false);
  assert.equal(isValidOperationId("users/get"), false);

  const app = createLampApp();
  app.register("users.get", { handler: () => ({ ok: true }) });

  assert.throws(
    () => app.register("bad/operation", { handler: () => null }),
    (err: unknown) => isLampError(err) && err.code === "INVALID_ARGUMENT"
  );

  assert.throws(
    () => app.register("users.get", { handler: () => null }),
    (err: unknown) => isLampError(err) && err.code === "ALREADY_EXISTS"
  );
});

test("typed schema validation validates inputs, outputs, and external parse schemas", async () => {
  const UserInputSchema = s.object({
    id: s.string({ minLength: 1 }),
    limit: s.optional(s.number({ integer: true, min: 1, max: 50 })),
  });
  const UserOutputSchema = s.object({
    id: s.string(),
    tags: s.array(s.string()),
    active: s.boolean(),
  });

  // External schema object implementing only { parse(value) }
  const ExternalEchoSchema = {
    parse(value: unknown): { msg: string } {
      if (
        typeof value !== "object" ||
        value === null ||
        typeof (value as { msg?: unknown }).msg !== "string"
      ) {
        throw new Error("Expected { msg: string }");
      }
      return { msg: (value as { msg: string }).msg.trim() };
    },
  };

  let brokenOutput = false;
  const app = createLampApp();

  app
    .register("users.get", {
      input: UserInputSchema,
      output: UserOutputSchema,
      handler: async ({ input }) => {
        if (brokenOutput) {
          return { id: input.id, tags: [123] as unknown as string[], active: true };
        }
        return { id: input.id, tags: ["vip"], active: true };
      },
    })
    .register("echo.external", {
      input: ExternalEchoSchema,
      handler: ({ input }) => input.msg,
    });

  await app.start();

  const ok = await app.invoke("users.get", { id: "u1", limit: 10 });
  assert.deepEqual(ok, { id: "u1", tags: ["vip"], active: true });

  await assert.rejects(
    () => app.invoke("users.get", { id: "", limit: 5 }),
    (err: unknown) => isLampError(err) && err.code === "INVALID_ARGUMENT"
  );

  brokenOutput = true;
  await assert.rejects(
    () => app.invoke("users.get", { id: "u1" }),
    (err: unknown) => isLampError(err) && err.code === "SERIALIZATION_FAILED"
  );

  const ext = await app.invoke<string>("echo.external", { msg: "  hello  " });
  assert.equal(ext, "hello");

  await app.stop();
});

test("operation caching reuses entries within TTL and respects bypassCache", async () => {
  let executions = 0;
  const app = createLampApp({
    cache: { enabled: true },
  });

  app.register("users.get", {
    input: s.object({ id: s.string() }),
    output: s.object({ id: s.string(), seq: s.number() }),
    cache: { ttl: 60 },
    handler: async ({ input }) => {
      executions += 1;
      return { id: input.id, seq: executions };
    },
  });

  await app.start();

  const r1 = await app.invoke<{ id: string; seq: number }>("users.get", { id: "a" });
  const r2 = await app.invoke<{ id: string; seq: number }>("users.get", { id: "a" });
  const r3 = await app.invoke<{ id: string; seq: number }>("users.get", { id: "b" });
  const r4 = await app.invoke<{ id: string; seq: number }>(
    "users.get",
    { id: "a" },
    { bypassCache: true }
  );

  assert.equal(r1.seq, 1);
  assert.equal(r2.seq, 1);
  assert.equal(r3.seq, 2);
  assert.equal(r4.seq, 3);
  assert.equal(executions, 3);

  await app.stop();
});

test("error normalization preserves LampError and sanitizes unexpected exceptions", async () => {
  const app = createLampApp();

  app
    .register("op.domainError", {
      handler: () => {
        throw new LampError("User missing", {
          code: "NOT_FOUND",
          identifier: "USER_NOT_FOUND",
        });
      },
    })
    .register("op.crash", {
      handler: () => {
        throw new Error("C:\\secret\\db\\connection password=12345");
      },
    });

  await app.start();

  await assert.rejects(
    () => app.invoke("op.domainError"),
    (err: unknown) =>
      isLampError(err) &&
      err.code === "NOT_FOUND" &&
      err.identifier === "USER_NOT_FOUND" &&
      err.message === "User missing"
  );

  await assert.rejects(
    () => app.invoke("op.crash"),
    (err: unknown) =>
      isLampError(err) &&
      err.code === "EXECUTION_FAILED" &&
      !err.message.includes("password") &&
      !err.message.includes("secret")
  );

  await app.stop();
});

test("cancellation AbortSignal and deadline timeouts reject cleanly", async () => {
  const app = createLampApp();

  app.register("slow.work", {
    execution: { timeoutMs: 30 },
    handler: async ({ signal }) => {
      await new Promise<void>((resolve) => {
        const timer = setTimeout(resolve, 250);
        signal.addEventListener("abort", () => clearTimeout(timer), { once: true });
      });
      return "done";
    },
  });

  await app.start();

  // Timeout triggers DEADLINE_EXCEEDED
  await assert.rejects(
    () => app.invoke("slow.work"),
    (err: unknown) => isLampError(err) && err.code === "DEADLINE_EXCEEDED"
  );

  // Explicit AbortController cancellation triggers CANCELLED
  const ac = new AbortController();
  setTimeout(() => ac.abort(), 10);
  await assert.rejects(
    () => app.invoke("slow.work", undefined, { signal: ac.signal, timeoutMs: 5000 }),
    (err: unknown) => isLampError(err) && err.code === "CANCELLED"
  );

  await app.stop();
});

test("auth policy enforcement and nativeHostBridge registration", async () => {
  const registered: Array<{ id: string; metadata: RegisteredOperationMetadata }> = [];
  let startedCalls = 0;
  let stoppedCalls = 0;
  let capturedInvoker:
    | ((rawJsonInput: string, metadataJson: string) => unknown | Promise<unknown>)
    | undefined;

  const mockBridge: NativeHostBridge = {
    registerOperation(operationId, metadata, nativeInvoker) {
      registered.push({ id: operationId, metadata });
      capturedInvoker = nativeInvoker;
    },
    markStarted() {
      startedCalls += 1;
    },
    markStopped() {
      stoppedCalls += 1;
    },
  };

  const app = createLampApp({
    nativeBridge: mockBridge,
    security: { defaultDeny: true },
  });

  app.register("admin.users.get", {
    input: s.object({ id: s.string() }),
    auth: { roles: ["admin"], scopes: ["users:read"], tenants: ["t1"] },
    handler: ({ input, ctx }) => ({
      id: input.id,
      actor: ctx.auth.subject,
    }),
  });

  const info = await app.start();
  assert.equal(info.nativeBacked, true);
  assert.equal(startedCalls, 1);
  assert.equal(registered.length, 1);
  assert.equal(registered[0].metadata.auth.allowAnonymous, false);

  await assert.rejects(
    () => app.invoke("admin.users.get", { id: "u1" }),
    (err: unknown) => isLampError(err) && err.code === "UNAUTHENTICATED"
  );

  await assert.rejects(
    () =>
      app.invoke(
        "admin.users.get",
        { id: "u1" },
        { auth: { authenticated: true, subject: "u", tenant: "t1", roles: ["member"] } }
      ),
    (err: unknown) => isLampError(err) && err.code === "PERMISSION_DENIED"
  );

  assert.ok(capturedInvoker);
  const bridgeResult = await capturedInvoker(
    JSON.stringify({ id: "42" }),
    JSON.stringify({
      "lamp.auth.authenticated": "true",
      "lamp.auth.subject": "root-admin",
      "lamp.auth.tenant": "t1",
      "lamp.auth.roles": "admin",
      "lamp.auth.scopes": "users:read",
    })
  );
  assert.deepEqual(bridgeResult, { id: "42", actor: "root-admin" });

  await app.stop();
  assert.equal(stoppedCalls, 1);
});
