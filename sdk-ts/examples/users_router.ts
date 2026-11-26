import {
  LampError,
  createLampApp,
  createLampRegistry,
  createLampRouter,
  s,
} from "../src/index.ts";

const users = new Map<string, { id: string; name: string }>([
  ["u_1", { id: "u_1", name: "Ada Lovelace" }],
]);

const localUsers = new Map<string, { id: string; name: string; localNode: string }>([
  ["lu_1", { id: "lu_1", name: "Grace Hopper", localNode: "node-a" }],
]);

// Registry layer: handles business logic with relative operation suffixes
const userRegistry = createLampRegistry().add("get", {
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

const localUserRegistry = createLampRegistry().add("get", {
  description: "Fetch a local user profile by ID",
  input: s.object({ id: s.string({ minLength: 1 }) }),
  output: s.object({ id: s.string(), name: s.string(), localNode: s.string() }),
  handler: ({ input }) => {
    const user = localUsers.get(input.id);
    if (!user) {
      throw new LampError(`Local user "${input.id}" not found`, {
        code: "NOT_FOUND",
        identifier: "LOCAL_USER_NOT_FOUND",
      });
    }
    return user;
  },
});

// Route layer: manages API request structure, target prefixes, nesting, and shared options
const localUsersRouter = createLampRouter({
  description: "Local users sub-module",
  target: "local_users",
  registry: [localUserRegistry],
});

const userRouter = createLampRouter({
  description: "Users route module",
  target: "users",
  registry: [userRegistry],
  nesting: localUsersRouter,
  options: {
    tags: ["users"],
  },
});

const app = createLampApp({
  transport: "lmp1-binary",
  serialization: "lcb1",
  execution: "parallel-dynamic",
  cache: { enabled: true, defaultTtl: 60, maxTtlLimitSeconds: 300 },
});

app.registerRouter(userRouter);

await app.start();
console.log("Registered operations:", app.operations().map((o) => o.id));
console.log("users.get:", await app.invoke("users.get", { id: "u_1" }));
console.log("users.local_users.get:", await app.invoke("users.local_users.get", { id: "lu_1" }));
await app.stop();
