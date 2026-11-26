import assert from "node:assert/strict";
import test from "node:test";

import {
  LampError,
  createLampApp,
  createLampRegistry,
  createLampRouter,
  getNativeHostBridge,
  isLampError,
  isValidOperationId,
  s,
  setNativeHostBridge,
  type NativeHostBridge,
} from "../src/index.ts";

test("createLampApp enforces server lifecycle transitions and hooks", async () => {
  const events: string[] = [];
  const app = createLampApp({
    transport: "lmp1-binary",
    serialization: "lcb1",
    execution: "parallel-dynamic",
    cache: { enabled: true, defaultTtl: 30, maxTtlLimitSeconds: 120 },
  });

  app
    .onStart(() => {
      events.push("start:1");
    })
    .onStop(() => {
      events.push("stop:1");
    })
    .onStop(() => {
      events.push("stop:2");
    })
    .register("system.health", {
      output: s.object({ status: s.literal("ok") }),
      handler: () => ({ status: "ok" as const }),
    });

  assert.equal(app.state(), "created");

  await assert.rejects(
    () => app.invoke("system.health"),
    (err: unknown) => isLampError(err) && err.code === "FAILED_PRECONDITION"
  );

  const info = await app.start();
  assert.equal(app.state(), "running");
  assert.equal(info.transport, "lmp1-binary");
  assert.equal(info.serialization, "lcb1");
  assert.equal(info.execution, "parallel-dynamic");
  assert.deepEqual(info.operations, ["system.health"]);

  assert.throws(
    () => app.register("late.op", { handler: () => null }),
    (err: unknown) => isLampError(err) && err.code === "FAILED_PRECONDITION"
  );

  assert.deepEqual(await app.invoke("system.health"), { status: "ok" });

  await app.stop();
  assert.equal(app.state(), "stopped");
  assert.deepEqual(events, ["start:1", "stop:2", "stop:1"]);
});

test("register enforces valid dotted operation IDs, duplicates, and TTL upper limit", () => {
  assert.equal(isValidOperationId("users.get"), true);
  assert.equal(isValidOperationId("orders_v2.bulk-sync"), true);
  assert.equal(isValidOperationId(""), false);
  assert.equal(isValidOperationId("users/get"), false);
  assert.equal(isValidOperationId("users..get"), false);

  const app = createLampApp({
    cache: { enabled: true, defaultTtl: 30, maxTtlLimitSeconds: 90 },
  });

  app.register("users.cached", {
    cache: { ttl: 9999 },
    handler: () => ({ ok: true }),
  });

  const meta = app.operations()[0];
  assert.equal(meta.cache.enabled, true);
  assert.equal(meta.cache.ttlSeconds, 90);
  assert.equal(meta.cache.maxTtlLimitSeconds, 90);

  assert.throws(
    () => app.register("users.cached", { handler: () => null }),
    (err: unknown) => isLampError(err) && err.code === "ALREADY_EXISTS"
  );
});

test("s.file() validates native binary payloads and ctx.files hydration", async () => {
  const app = createLampApp();

  app.register("upload.avatar", {
    input: s.object({
      userId: s.string(),
      avatar: s.file({ maxSizeBytes: 1024, allowedMimeTypes: ["image/png"] }),
    }),
    output: s.object({
      userId: s.string(),
      fileName: s.string(),
      size: s.number(),
    }),
    handler: ({ input, ctx }) => {
      assert.equal(ctx.files.length, 1);
      return {
        userId: input.userId,
        fileName: input.avatar.name,
        size: input.avatar.data.byteLength,
      };
    },
  });

  await app.start();

  const file = {
    name: "pic.png",
    mimeType: "image/png",
    size: 4,
    data: new Uint8Array([0x89, 0x50, 0x4e, 0x47]),
  };

  const ok = await app.invoke(
    "upload.avatar",
    { userId: "u_1", avatar: { __lampFileRef: 0 } },
    { files: [file] }
  );
  assert.deepEqual(ok, { userId: "u_1", fileName: "pic.png", size: 4 });

  await assert.rejects(
    () =>
      app.invoke("upload.avatar", {
        userId: "u_1",
        avatar: { name: "bad.exe", mimeType: "application/x-msdownload", data: new Uint8Array([1]) },
      }),
    (err: unknown) => isLampError(err) && err.code === "INVALID_ARGUMENT"
  );

  await app.stop();
});

test("ctx.parallel delegates subtasks to NativeHostBridge.runParallel when available", async () => {
  const recorded: number[] = [];
  const mockBridge: NativeHostBridge = {
    registerOperation() {},
    runParallel(tasks) {
      recorded.push(tasks.length);
      return tasks.map((t) => {
        const arg = JSON.parse(t.inputJson);
        return JSON.stringify(Number(arg) * 10);
      });
    },
  };

  const prev = getNativeHostBridge();
  setNativeHostBridge(mockBridge);
  try {
    const app = createLampApp();
    app.register("jobs.batch", {
      input: s.object({ nums: s.array(s.number()) }),
      output: s.array(s.number()),
      handler: ({ input, ctx }) => ctx.parallel(input.nums, (n) => n * 10),
    });

    await app.start();
    const res = await app.invoke<number[]>("jobs.batch", { nums: [1, 2, 3, 4] });
    assert.deepEqual(res, [10, 20, 30, 40]);
    assert.deepEqual(recorded, [4]);
    await app.stop();
  } finally {
    setNativeHostBridge(prev);
  }
});

test("server SDK exports contain zero HTTP or client symbols", async () => {
  const pkg = await import("../src/index.ts");
  assert.equal("createLampClient" in pkg, false);
  assert.equal("startLampNodeServer" in pkg, false);
  assert.equal(typeof new LampError("boom").toJSON, "function");
});

test("createLampApp allowOrigin contract supports boolean, expose/callback, and native bridge sync", () => {
  const recordedPolicies: string[] = [];
  const mockBridge: NativeHostBridge = {
    registerOperation() {},
    configureOriginAllowance(policyJson) {
      recordedPolicies.push(policyJson);
    },
  };

  const prev = getNativeHostBridge();
  setNativeHostBridge(mockBridge);
  try {
    // 1. Default is false -> deny_all
    const defaultApp = createLampApp();
    assert.deepEqual(defaultApp.originPolicy(), { mode: "deny_all", origins: [] });
    assert.equal(defaultApp.isOriginAllowed("https://example.com"), false);

    // 2. allowOrigin: true -> allow_all
    const allowAllApp = createLampApp({ allowOrigin: true });
    assert.deepEqual(allowAllApp.originPolicy(), { mode: "allow_all", origins: [] });
    assert.equal(allowAllApp.isOriginAllowed("https://any.example.org"), true);

    // 3. allowOrigin: (expose, callback) => boolean returning true
    let callbackCount = 0;
    const customApp = createLampApp({
      allowOrigin: (expose, callback) => {
        expose(["https://App.Example.com:8443/", "http://localhost:3000"]);
        callback();
        callbackCount += 1;
        return true;
      },
    });
    assert.equal(callbackCount, 1);
    assert.deepEqual(customApp.originPolicy(), {
      mode: "allow_list",
      origins: ["https://app.example.com:8443", "http://localhost:3000"],
    });
    assert.equal(customApp.isOriginAllowed("https://app.example.com:8443"), true);
    assert.equal(customApp.isOriginAllowed("http://localhost:3000/"), true);
    assert.equal(customApp.isOriginAllowed("https://evil.example.com"), false);

    // 4. allowOrigin returning false -> deny_all (CORS error)
    const deniedFnApp = createLampApp({
      allowOrigin: (expose, callback) => {
        expose(["https://allowed.example.com"]);
        callback();
        return false;
      },
    });
    assert.deepEqual(deniedFnApp.originPolicy(), { mode: "deny_all", origins: [] });
    assert.equal(deniedFnApp.isOriginAllowed("https://allowed.example.com"), false);

    assert.equal(recordedPolicies.length, 4);
    assert.deepEqual(JSON.parse(recordedPolicies[2]), {
      mode: "allow_list",
      origins: ["https://app.example.com:8443", "http://localhost:3000"],
    });
  } finally {
    setNativeHostBridge(prev);
  }
});

test("createLampRegistry and createLampRouter support flat and nested registry routes", async () => {
  const userRegistry = createLampRegistry().add("get", {
    description: "Get user by id",
    input: s.object({ id: s.string() }),
    output: s.object({ id: s.string(), scope: s.string() }),
    handler: ({ input }) => ({ id: input.id, scope: "global" }),
  });

  const localUsersRegistry = createLampRegistry().add("get", {
    description: "Get local user by id",
    input: s.object({ id: s.string() }),
    output: s.object({ id: s.string(), scope: s.string() }),
    handler: ({ input }) => ({ id: input.id, scope: "local" }),
  });

  const localUsersRouter = createLampRouter({
    description: "Local users module",
    target: "local_users",
    registry: [localUsersRegistry],
  });

  const userRouter = createLampRouter({
    description: "Users route module",
    target: "users",
    registry: [userRegistry],
    nesting: localUsersRouter,
  });

  const app = createLampApp();
  app.registerRouter(userRouter);

  const info = await app.start();
  assert.deepEqual(info.operations, ["users.get", "users.local_users.get"]);

  assert.deepEqual(await app.invoke("users.get", { id: "u_1" }), {
    id: "u_1",
    scope: "global",
  });
  assert.deepEqual(await app.invoke("users.local_users.get", { id: "u_2" }), {
    id: "u_2",
    scope: "local",
  });

  await app.stop();
});

test("createLampRegistry enforces single-segment suffixes and duplicate checks", () => {
  const registry = createLampRegistry();

  assert.throws(
    () => registry.add("users.get", { handler: () => null }),
    (err: unknown) => isLampError(err) && err.identifier === "INVALID_OPERATION_SUFFIX"
  );
  assert.throws(
    () => registry.add("", { handler: () => null }),
    (err: unknown) => isLampError(err) && err.identifier === "INVALID_OPERATION_SUFFIX"
  );

  registry.add("get", { handler: () => ({ ok: true }) });
  assert.throws(
    () => registry.add("get", { handler: () => ({ ok: true }) }),
    (err: unknown) => isLampError(err) && err.code === "ALREADY_EXISTS"
  );
});

test("createLampRouter and registerRouter merge options with proper precedence and prefix", async () => {
  const adminRegistry = createLampRegistry()
    .add("list", {
      tags: ["read"],
      handler: () => ["a"],
    })
    .add("delete", {
      auth: { roles: ["superadmin"] },
      cache: false,
      execution: { timeoutMs: 5000 },
      tags: ["danger"],
      handler: () => ({ deleted: true }),
    });

  const adminRouter = createLampRouter({
    description: "Admin routes",
    target: "admin",
    registry: [adminRegistry],
    options: {
      auth: { allowAnonymous: false, roles: ["admin"] },
      cache: { ttl: 45 },
      execution: { timeoutMs: 10000 },
      tags: ["admin"],
    },
  });

  const app = createLampApp({ cache: { enabled: true, maxTtlLimitSeconds: 120 } });
  app.registerRouter(adminRouter, {
    prefix: "v1",
    tags: ["api"],
    execution: { mode: "adaptive" },
  });

  const ops = new Map(app.operations().map((op) => [op.id, op]));
  const listOp = ops.get("v1.admin.list");
  const deleteOp = ops.get("v1.admin.delete");

  assert.ok(listOp);
  assert.equal(listOp.cache.enabled, true);
  assert.equal(listOp.cache.ttlSeconds, 45);
  assert.equal(listOp.execution.mode, "adaptive");
  assert.equal(listOp.execution.timeoutMs, 10000);
  assert.equal(listOp.auth.allowAnonymous, false);
  assert.deepEqual(listOp.auth.roles, ["admin"]);
  assert.deepEqual(listOp.tags, ["api", "admin", "read"]);

  assert.ok(deleteOp);
  assert.equal(deleteOp.cache.enabled, false);
  assert.equal(deleteOp.execution.timeoutMs, 5000);
  assert.deepEqual(deleteOp.auth.roles, ["superadmin"]);
  assert.deepEqual(deleteOp.tags, ["api", "admin", "danger"]);

  await app.start();
  assert.throws(
    () => app.registerRouter(adminRouter),
    (err: unknown) => isLampError(err) && err.code === "FAILED_PRECONDITION"
  );
  await app.stop();
});

test("createLampRouter detects circular nesting and invalid targets", () => {
  assert.throws(
    () => createLampRouter({ target: "" }),
    (err: unknown) => isLampError(err) && err.identifier === "INVALID_ROUTER_TARGET"
  );

  const childNested: ReturnType<typeof createLampRouter>[] = [];
  const child = createLampRouter({
    target: "child",
    nesting: childNested,
  });
  const parent = createLampRouter({
    target: "parent",
    nesting: [child],
  });
  // Mutate internal array reference? No, nesting is copied, so test direct self-cycle via object spread
  const cyclic: any = {
    target: "loop",
    options: {},
    registries: [],
    nested: [],
  };
  cyclic.nested = [cyclic];
  const root = createLampRouter({
    target: "root",
    nesting: [cyclic],
  });

  assert.throws(
    () => root.resolveOperations(),
    (err: unknown) => isLampError(err) && err.identifier === "CIRCULAR_ROUTER_NESTING"
  );
  assert.equal(parent.target, "parent");
});
