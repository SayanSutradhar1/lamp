import { LampError, createLampApp, s } from "../src/index.ts";

const users = new Map<string, { id: string; name: string }>([
  ["u_1", { id: "u_1", name: "Ada Lovelace" }],
]);

const app = createLampApp({
  transport: "lmp1-binary",
  serialization: "lcb1",
  execution: "parallel-dynamic",
  cache: { enabled: true, defaultTtl: 60, maxTtlLimitSeconds: 300 },
});

app.register("users.get", {
  description: "Fetch a user profile by ID",
  input: s.object({ id: s.string({ minLength: 1 }) }),
  output: s.object({ id: s.string(), name: s.string() }),
  cache: { ttl: 60 },
  handler: ({ input }) => {
    const user = users.get(input.id);
    if (!user) {
      throw new LampError(`User "${input.id}" not found`, {
        code: "NOT_FOUND",
        identifier: "USER_NOT_FOUND",
      });
    }
    return user;
  },
});

await app.start();
console.log("Registered operations:", app.operations().map((o) => o.id));
console.log("Sample result:", await app.invoke("users.get", { id: "u_1" }));
await app.stop();
