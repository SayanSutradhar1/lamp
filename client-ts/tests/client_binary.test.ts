import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

import {
  prepareEntryForEmbeddedV8,
  resolveLampServerBinary,
} from "../../sdk-ts/bin/lamp.ts";
import { createLampClient as createBrowserLampClient } from "../src/browser_client.ts";
import {
  LmpFrameDecoder,
  createLampClient,
  encodeRpcRequestStreamAsync,
  isLampClientError,
} from "../src/index.ts";

const TEST_DIR = path.dirname(fileURLToPath(import.meta.url));
const REPO_ROOT = path.resolve(TEST_DIR, "..", "..");

async function waitForServerReady(
  port: number,
  timeoutMs = 8000
): Promise<void> {
  const client = createLampClient({ url: `lmp://127.0.0.1:${port}`, connectTimeoutMs: 400 });
  const deadline = Date.now() + timeoutMs;
  try {
    while (Date.now() < deadline) {
      try {
        await client.ping();
        return;
      } catch {
        await new Promise((r) => setTimeout(r, 80));
      }
    }
    throw new Error(`Timed out waiting for lamp_server on port ${port}`);
  } finally {
    await client.close();
  }
}

test("async binary request streaming yields multiple non-blocking chunks cleanly", async () => {
  const bigBytes = new Uint8Array(24 * 1024);
  for (let i = 0; i < bigBytes.byteLength; i += 1) {
    bigBytes[i] = i & 0xff;
  }

  const gen = encodeRpcRequestStreamAsync(
    "files.upload",
    42,
    { label: "streamed", file: { name: "blob.bin", mimeType: "application/octet-stream", data: bigBytes } },
    [],
    4096
  );

  const chunks: Uint8Array[] = [];
  let summary: { totalBytes: number; hasFiles: boolean } | undefined;
  while (true) {
    const step = await gen.next();
    if (step.done) {
      summary = step.value;
      break;
    }
    chunks.push(step.value);
  }

  assert.ok(chunks.length >= 6);
  assert.equal(summary?.hasFiles, true);

  const dec = new LmpFrameDecoder();
  for (const ch of chunks) dec.append(ch);
  const frame = dec.tryNextFrame();
  if (!frame) throw new Error("Expected decoded LMP1 frame");
  assert.equal(frame.header.requestId, 42);
  assert.equal(frame.header.payloadSize + 14, summary?.totalBytes);
});

test("live C++ lamp_server binary TCP + WebSocket E2E with file upload, wire cache & parallel subtasks", async () => {
  const bin = resolveLampServerBinary();
  if (!fs.existsSync(bin) && bin === path.basename(bin)) {
    throw new Error("Compiled lamp_server binary not found");
  }

  const port = 19421 + Math.floor(Math.random() * 1000);
  const entryTs = `
import { LampError, createLampApp, s } from "lamp";

const store = new Map();
let evalCount = 0;

const app = createLampApp({
  transport: "lmp1-binary",
  serialization: "lcb1",
  execution: "parallel-dynamic",
  cache: { enabled: true, defaultTtl: 30, maxTtlLimitSeconds: 5 },
});

app.register("users.create", {
  input: s.object({ name: s.string({ minLength: 2 }) }),
  output: s.object({ id: s.string(), name: s.string() }),
  handler: ({ input }) => {
    const id = "u_" + (store.size + 1);
    const u = { id, name: input.name };
    store.set(id, u);
    return u;
  },
});

app.register("users.getCached", {
  input: s.object({ id: s.string() }),
  output: s.object({ id: s.string(), name: s.string(), evals: s.number() }),
  cache: { ttl: 999 },
  handler: ({ input }) => {
    evalCount += 1;
    const found = store.get(input.id);
    if (!found) {
      throw new LampError("User missing", { code: "NOT_FOUND", identifier: "USER_MISSING" });
    }
    return { id: found.id, name: found.name, evals: evalCount };
  },
});

app.register("files.echoSize", {
  input: s.object({ title: s.string(), payload: s.file({ maxSizeBytes: 1024 * 1024 }) }),
  output: s.object({ title: s.string(), name: s.string(), bytes: s.number(), head: s.number() }),
  handler: ({ input }) => ({
    title: input.title,
    name: input.payload.name,
    bytes: input.payload.data.byteLength,
    head: input.payload.data[0] ?? -1,
  }),
});

app.register("compute.squares", {
  input: s.object({ values: s.array(s.number()) }),
  output: s.object({ result: s.array(s.number()) }),
  handler: async ({ input, ctx }) => {
    const result = await ctx.parallel(input.values, (n) => n * n);
    return { result };
  },
});

await app.start();
`;

  const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), "lamp-client-e2e-"));
  const compiledPath = path.join(tmpDir, "app.v8.js");
  fs.writeFileSync(compiledPath, prepareEntryForEmbeddedV8(entryTs, "app.ts"), "utf8");

  const proc = spawn(
    bin,
    [
      "--entry",
      compiledPath,
      "--host",
      "127.0.0.1",
      "--port",
      String(port),
      "--min-threads",
      "2",
      "--max-threads",
      "6",
      "--max-cache-ttl-ms",
      "5000",
    ],
    { cwd: REPO_ROOT, stdio: ["ignore", "pipe", "pipe"] }
  );

  let stderrLog = "";
  proc.stderr?.on("data", (d) => {
    stderrLog += d.toString();
  });

  const client = createLampClient({ url: `lmp://127.0.0.1:${port}` });
  const browserClient = createBrowserLampClient({ url: `ws://127.0.0.1:${port}` });

  try {
    await waitForServerReady(port);

    // 1. Non-REST enforcement check
    const httpProbe = await fetch(`http://127.0.0.1:${port}/users`).catch(() => null);
    assert.ok(httpProbe !== null && httpProbe.status === 404);

    // 2. Binary manifest & ping
    const rtt = await client.ping();
    assert.ok(rtt >= 0);

    const manifest = await client.manifest();
    assert.equal(manifest.protocol, "LMP1/LCB1");
    assert.equal(manifest.restPattern, false);
    assert.ok(manifest.operations.some((o) => o.id === "users.create"));

    // 3. Create record & verify FullCycleWireCache
    const created = await client.call<{ id: string; name: string }>("users.create", {
      name: "Grace Hopper",
    });
    assert.deepEqual(created, { id: "u_1", name: "Grace Hopper" });

    const first = await client.callWithMeta<{ id: string; name: string; evals: number }>(
      "users.getCached",
      { id: "u_1" }
    );
    assert.equal(first.meta.cacheHit, false);
    assert.equal(first.data.evals, 1);

    const second = await client.callWithMeta<{ id: string; name: string; evals: number }>(
      "users.getCached",
      { id: "u_1" }
    );
    assert.equal(second.meta.cacheHit, true);
    assert.equal(second.data.evals, 1);

    // 4. Native binary file upload (no base64)
    const rawBin = new Uint8Array([0xab, 0xcd, 0xef, 0x01, 0x02]);
    const fileRes = await client.call<{
      title: string;
      name: string;
      bytes: number;
      head: number;
    }>("files.echoSize", {
      title: "bin-doc",
      payload: { name: "sample.dat", mimeType: "application/octet-stream", data: rawBin },
    });
    assert.deepEqual(fileRes, {
      title: "bin-doc",
      name: "sample.dat",
      bytes: 5,
      head: 0xab,
    });

    // 5. Dynamic C++ worker pool parallel subtask splitting
    const parRes = await client.call<{ result: number[] }>("compute.squares", {
      values: [2, 3, 4, 5, 6],
    });
    assert.deepEqual(parRes, { result: [4, 9, 16, 25, 36] });

    // 6. Browser WebSocket binary LMP1 client check
    if (typeof WebSocket !== "undefined") {
      const wsRes = await browserClient.call<{ result: number[] }>("compute.squares", {
        values: [7, 8],
      });
      assert.deepEqual(wsRes, { result: [49, 64] });
    }

    // 7. Live stats check
    const stats = await client.stats();
    assert.ok(stats.cacheHits >= 1);
    assert.equal(stats.cacheMaxTtlMs, 5000);
    assert.ok(stats.currentThreads >= 2);

    // 8. Semantic LampClientError hydration
    await assert.rejects(
      () => client.call("users.getCached", { id: "missing" }),
      (err: unknown) => isLampClientError(err) && err.code === "NOT_FOUND"
    );
  } finally {
    await client.close();
    await browserClient.close();
    proc.kill("SIGTERM");
    await new Promise((r) => proc.once("exit", r));
    try {
      fs.rmSync(tmpDir, { recursive: true, force: true });
    } catch {
      // Ignore Windows temp handle lock
    }
    if (stderrLog.trim().length > 0) {
      console.error("lamp_server stderr:", stderrLog);
    }
  }
});
