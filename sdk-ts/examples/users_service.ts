// ============================================================
// Lamp TypeScript SDK - Canonical Example (`users.get`)
// ============================================================

import { createLampApp, s } from "../src/index.ts";

const UserInputSchema = s.object({
  id: s.string({ minLength: 1 }),
});

const UserOutputSchema = s.object({
  id: s.string(),
  name: s.string(),
  role: s.enum(["member", "admin"]),
});

async function getUserById(id: string) {
  return {
    id,
    name: `User ${id}`,
    role: id === "1" ? ("admin" as const) : ("member" as const),
  };
}

const app = createLampApp({
  transport: "auto",
  serialization: "protobuf",
  execution: "adaptive",
  cache: { enabled: true },
});

app.register("users.get", {
  description: "Fetch a user by identifier",
  input: UserInputSchema,
  output: UserOutputSchema,
  cache: { ttl: 60 },
  handler: async ({ input }) => {
    return getUserById(input.id);
  },
});

const info = await app.start();
const user = await app.invoke<{ id: string; name: string; role: string }>("users.get", {
  id: "1",
});
console.log("Started operations:", info.operations.join(", "));
console.log("users.get result:", JSON.stringify(user));
await app.stop();
