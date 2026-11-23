import { LampError, createLampApp, s } from "../src/index.ts";

interface UserRecord {
  readonly id: string;
  readonly name: string;
  readonly email: string;
}

const users = new Map<string, UserRecord>();

const app = createLampApp({
  transport: "lmp1-binary",
  serialization: "lcb1",
  execution: "parallel-dynamic",
  cache: { enabled: true, defaultTtl: 60, maxTtlLimitSeconds: 300 },
});

app.register("users.create", {
  description: "Creates a user record validated against contract schema",
  input: s.object({
    name: s.string({ minLength: 2, maxLength: 80 }),
    email: s.string({ minLength: 5 }),
  }),
  output: s.object({
    id: s.string(),
    name: s.string(),
    email: s.string(),
  }),
  handler: ({ input }) => {
    const id = `u_${users.size + 1}`;
    const record: UserRecord = { id, name: input.name, email: input.email };
    users.set(id, record);
    return record;
  },
});

app.register("users.get", {
  description: "Reads a user with full-cycle binary wire caching enabled",
  input: s.object({ id: s.string({ minLength: 1 }) }),
  output: s.object({ id: s.string(), name: s.string(), email: s.string() }),
  cache: { ttl: 120 },
  handler: ({ input }) => {
    const found = users.get(input.id);
    if (!found) {
      throw new LampError(`User "${input.id}" not found`, {
        code: "NOT_FOUND",
        identifier: "USER_NOT_FOUND",
      });
    }
    return found;
  },
});

app.register("files.hashBinary", {
  description: "Accepts native binary file bytes without base64 bloat",
  input: s.object({
    tag: s.string(),
    attachment: s.file({ maxSizeBytes: 10 * 1024 * 1024 }),
  }),
  output: s.object({
    tag: s.string(),
    name: s.string(),
    mimeType: s.string(),
    bytes: s.number(),
  }),
  handler: ({ input }) => ({
    tag: input.tag,
    name: input.attachment.name,
    mimeType: input.attachment.mimeType,
    bytes: input.attachment.data.byteLength,
  }),
});

app.register("analytics.parallelSquares", {
  description: "Splits numeric workloads across dynamic C++ worker threads",
  input: s.object({ values: s.array(s.number()) }),
  output: s.object({ squares: s.array(s.number()) }),
  handler: async ({ input, ctx }) => {
    const squares = await ctx.parallel(input.values, (n) => n * n);
    return { squares };
  },
});

await app.start();
