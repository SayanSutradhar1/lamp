import assert from "node:assert/strict";
import test from "node:test";

import {
  LampError,
  createLampApp,
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
