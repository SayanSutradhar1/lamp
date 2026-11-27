export interface ConfigValueDetail {
  readonly value: string;
  readonly behavior: string;
}

export interface ConfigOptionRow {
  readonly option: string;
  readonly type: string;
  readonly defaultVal: string;
  readonly impact: string;
  readonly values?: readonly ConfigValueDetail[];
}

export interface ConfigTable {
  readonly title: string;
  readonly rows: readonly ConfigOptionRow[];
}

export interface DocSubtopic {
  readonly id: string;
  readonly title: string;
  readonly paragraphs: readonly string[];
  readonly bullets?: readonly string[];
  readonly code?: {
    readonly filename?: string;
    readonly language?: string;
    readonly content: string;
  };
  readonly configTable?: ConfigTable;
  readonly callout?: {
    readonly kind: "info" | "tip" | "warning";
    readonly title: string;
    readonly body: string;
  };
}

export type DocGroup =
  | "Foundations"
  | "Core Concepts"
  | "Runtime & Performance"
  | "Tooling & Clients";

export interface DocTopic {
  readonly id: string;
  readonly title: string;
  readonly group: DocGroup;
  readonly summary: string;
  readonly subtopics: readonly DocSubtopic[];
}

export const GITHUB_URL = "https://github.com/SayanSutradhar1/lamp";

export const DOC_TOPICS: readonly DocTopic[] = [
  {
    id: "concept",
    title: "Concept & Philosophy",
    group: "Foundations",
    summary:
      "Low-level overview of why Lamp replaces REST/HTTP with a pure-binary LMP1/LCB1 protocol, embedded V8 isolates, and a two-layer Route + Registry architecture.",
    subtopics: [
      {
        id: "why-not-rest",
        title: "Why Operation-Oriented Binary Instead of REST",
        paragraphs: [
          "In a standard Express/Node.js REST server, every request pays four recurring taxes: (1) parsing verbose HTTP/1.1 text headers on every call, (2) regex/radix matching URL strings and HTTP verbs, (3) repeating JSON object keys across every payload, and (4) base64-encoding binary attachments (+33% size overhead + CPU encode/decode).",
          "Lamp eliminates HTTP REST routing at the transport layer. Every capability is addressed by a logical dotted operation ID (such as app.users.get or app.compute.parallel) carried inside a fixed 14-byte LMP1 multiplexed frame with LCB1 string-interned binary payloads.",
          "Because LMP1 frames carry a 32-bit requestId, multiple in-flight requests share a single persistent TCP or WebSocket connection out-of-order without head-of-line blocking.",
        ],
        bullets: [
          "14-byte fixed LMP1 binary frame header (magic, version, type, flags, requestId, payloadLength).",
          "LCB1 string interning deduplicates repeated field names into compact varint table indices.",
          "2.27x to 2.71x lower wire bandwidth compared to Express JSON REST.",
          "3.91x faster cached reads via C++ FullCycleWireCache before V8 is ever entered.",
        ],
      },
      {
        id: "two-major-layers",
        title: "Two-Layer Application Design",
        paragraphs: [
          "Lamp enforces a strict separation of concerns between request routing and business logic:",
          "1. Route Layer (createLampRouter): Acts as the manager of the API request tree. It owns the relative target prefix, nests child routers, attaches shared policies (execution, cache, auth, tags), and defines route-level error handlers.",
          "2. Registry Layer (createLampRegistry): Holds pure business-logic operation definitions keyed by single-segment suffixes (get, list, create, delete) alongside input/output schemas and registry-level error handlers.",
          "At startup, app.registerRouter(rootRouter) compiles the entire tree into flat, O(1)-lookup dotted operation IDs registered directly into the C++ OperationRegistry.",
        ],
        callout: {
          kind: "tip",
          title: "Canonical Dotted Paths",
          body: "Mounting registry suffix 'get' inside router target 'users' nested under root router 'app' compiles to 'app.users.get' in the C++ OperationRegistry.",
        },
      },
    ],
  },
  {
    id: "quick-start",
    title: "Quick Start",
    group: "Foundations",
    summary:
      "Scaffold a new Lamp backend with npx create-lamp-app, generate modules with npx lamp add, and run in watch or production bundle mode.",
    subtopics: [
      {
        id: "scaffold-project",
        title: "1. Scaffold with create-lamp-app",
        paragraphs: [
          "Run npx create-lamp-app@latest to generate a clean TypeScript backend. No client SDK or sample client scripts are injected—only lamp-app and TypeScript.",
          "Three files are created under src/: app.ts (application bootstrap), router.ts (root router with target 'app'), and registry.ts (root registry with ping).",
        ],
        code: {
          filename: "terminal",
          language: "bash",
          content: `npx create-lamp-app@latest my-server
cd my-server`,
        },
      },
      {
        id: "add-modules",
        title: "2. Add Feature Modules & Submodules",
        paragraphs: [
          "Use npx lamp add <module> to scaffold <name>.router.ts and <name>.registry.ts under src/modules/<name>/. The CLI automatically inserts the ES import and appends the router into the parent router's nesting array.",
          "Nested submodules (e.g. users/local_users) auto-register into their parent module's router (users.router.ts).",
        ],
        code: {
          filename: "terminal",
          language: "bash",
          content: `npx lamp add users
npx lamp add users/local_users`,
        },
      },
      {
        id: "dev-build-start",
        title: "3. Watch Mode, Production Build & Start",
        paragraphs: [
          "npm run dev starts lamp_server with --watch, recursively watching src/ and restarting cleanly on .ts/.js changes.",
          "npm run build runs lamp build, which resolves all relative imports, strips TypeScript types via Node's native stripTypeScriptTypes, wraps each module in an isolated scope, and emits a single dependency-optimized dist/app.js bundle.",
        ],
        code: {
          filename: "terminal",
          language: "bash",
          content: `npm run dev       # lamp serve src/app.ts --port 3000 --watch
npm run build     # lamp build src/app.ts --out dist/app.js
npm start         # lamp serve dist/app.js --port 3000`,
        },
      },
    ],
  },
  {
    id: "architecture",
    title: "API Architecture & Execution Flow",
    group: "Core Concepts",
    summary:
      "Low-level request lifecycle from client binary frame to C++ LampBinaryServer, OriginPolicy, SecurityGateway, FullCycleWireCache, V8 main isolate, and DynamicWorkerPool.",
    subtopics: [
      {
        id: "execution-flow",
        title: "Low-Level Request Execution Flow",
        paragraphs: [
          "1. Socket & Frame Read: LampBinaryServer accepts persistent TCP (lmp://) or WebSocket (ws://) connections. Binary WebSocket frames (opcode 0x02) and raw TCP streams both feed the same LMP1 frame decoder.",
          "2. LCB1 Decode: The LCB1 codec extracts the operation ID string, input payload, and any attached binary file buffers (Uint8Array) without base64 decoding.",
          "3. Origin & Security Gateways: For browser WebSocket connections, core::OriginPolicy checks the Origin header before dispatch. SecurityGateway validates bearer/HMAC credentials and checks roles, scopes, and tenants.",
          "4. Full-Cycle Wire Cache Check: CacheKeyBuilder computes a deterministic key from (operationId, canonical input). If a non-expired entry exists in FullCycleWireCache, the pre-serialized LCB1 response bytes are written straight to the socket—zero V8 entry.",
          "5. V8 Main Isolate Dispatch: On cache miss, the main V8 isolate hydrates __lampFileRef pointers into ctx.files, validates input against the operation's s.* schema, invokes the handler, validates output, and serializes the LCB1 response.",
          "6. Optional Parallel Offload: If the handler calls ctx.parallel(items, fn), subtasks are queued onto core::DynamicWorkerPool where each worker thread owns a stateless V8 isolate (V8SubtaskIsolatePool), executing in true OS-thread parallelism.",
        ],
        code: {
          filename: "execution-pipeline.txt",
          language: "bash",
          content: `Client (lmp:// or ws://)
  -> LMP1 14B Frame + LCB1 Codec (zero-copy file bytes)
  -> LampBinaryServer (C++20 non-blocking I/O)
  -> OriginPolicy + SecurityGateway (RBAC / scopes / tenants / JWT)
  -> FullCycleWireCache [HIT -> immediate LCB1 frame, V8 skipped]
  -> Main V8 Isolate (hydrate files -> input schema -> handler -> output schema)
     -> ctx.parallel() -> DynamicWorkerPool (V8SubtaskIsolatePool threads)
  -> FullCycleWireCache [STORE w/ TTL clamped to maxTtlLimitSeconds]
  -> LMP1 Response Frame`,
        },
      },
    ],
  },
  {
    id: "lamp-app",
    title: "Lamp App (createLampApp)",
    group: "Core Concepts",
    summary:
      "The root application container managing transport, serialization, execution scheduling, logging, wire caching, worker thread bounds, security, and origin policy.",
    subtopics: [
      {
        id: "create-lamp-app-usage",
        title: "Creating & Bootstrapping the App",
        paragraphs: [
          "createLampApp(config) initializes the application state machine (created -> running -> stopped). All operations and routers must be registered while in the created state; attempting to register after app.start() throws FAILED_PRECONDITION (APP_ALREADY_STARTED).",
          "onStart(hook) callbacks execute in FIFO order before the C++ runtime begins accepting connections. onStop(hook) callbacks execute in LIFO order during graceful shutdown.",
        ],
        code: {
          filename: "src/app.ts",
          language: "ts",
          content: `import { createLampApp } from "lamp-app";
import { rootRouter } from "./router.ts";

const app = createLampApp({
  transport: "lmp1-binary",
  serialization: "lcb1",
  execution: "parallel-dynamic",
  log: true,
  cache: { enabled: true, defaultTtl: 60, maxTtlLimitSeconds: 300 },
  workers: { minThreads: 2, maxThreads: 16 },
  allowOrigin: (expose, callback) => {
    expose(["http://localhost:5173"]);
    callback();
    return true;
  },
});

app.onStart(() => console.log("App starting..."));
app.registerRouter(rootRouter);
await app.start();`,
        },
      },
      {
        id: "lamp-app-config",
        title: "LampAppConfig & Every Option Value",
        paragraphs: [
          "Below is every configuration option on createLampApp(config), including the exact runtime effect of each accepted value:",
        ],
        configTable: {
          title: "LampAppConfig Options & Values",
          rows: [
            {
              option: "transport",
              type: `"lmp1-binary" | "auto"`,
              defaultVal: `"lmp1-binary"`,
              impact: "Selects the wire transport server.",
              values: [
                { value: `"lmp1-binary"`, behavior: "Enforces pure-binary LMP1 14-byte multiplexed framing over TCP & WebSocket; rejects HTTP REST." },
                { value: `"auto"`, behavior: "Allows the native host to select the optimal binary transport available (resolves to lmp1-binary)." },
              ],
            },
            {
              option: "serialization",
              type: `"lcb1" | "protobuf" | "auto"`,
              defaultVal: `"lcb1"`,
              impact: "Selects the payload binary codec.",
              values: [
                { value: `"lcb1"`, behavior: "String-interned compact binary codec with native Uint8Array/LampFile support and minimal wire size." },
                { value: `"protobuf"`, behavior: "Uses runtime/serialization ProtobufCodec for schema-bound Protobuf wire encoding." },
                { value: `"auto"`, behavior: "Negotiates codec automatically (defaults to lcb1)." },
              ],
            },
            {
              option: "execution",
              type: `LampExecutionMode`,
              defaultVal: `"parallel-dynamic"`,
              impact: "Default execution scheduler for handlers.",
              values: [
                { value: `"parallel-dynamic"`, behavior: "Enables elastic DynamicWorkerPool scaling for ctx.parallel subtasks alongside main-isolate dispatch." },
                { value: `"adaptive"`, behavior: "Dynamically chooses between inline fast-path and worker pool based on runtime load." },
                { value: `"javascript"`, behavior: "Runs handlers strictly on the main V8 isolate." },
                { value: `"native"`, behavior: "Dispatches to registered native C++ handlers when available." },
                { value: `"v8-worker"`, behavior: "Routes execution through pooled V8 worker isolates." },
              ],
            },
            {
              option: "log",
              type: "boolean",
              defaultVal: "true",
              impact: "Controls console operation logging.",
              values: [
                { value: "true", behavior: "Logs [lamp] + registered <id> at startup and [lamp] -> <id> (#reqId) on every invocation." },
                { value: "false", behavior: "Silences all [lamp] registration and per-request logs for maximum throughput." },
              ],
            },
            {
              option: "cache",
              type: "boolean | LampAppCacheConfig",
              defaultVal: "false",
              impact: "Controls C++ FullCycleWireCache.",
              values: [
                { value: "false", behavior: "Disables full-cycle wire caching globally." },
                { value: "true", behavior: "Enables wire caching with defaultTtl = 60s and maxTtlLimitSeconds = 300s." },
                { value: "{ enabled, mode, defaultTtl, maxTtlLimitSeconds }", behavior: "Fine-grained control: mode ('local' | 'disabled'), defaultTtl for cache: true ops, and hard maxTtlLimitSeconds cap." },
              ],
            },
            {
              option: "workers",
              type: "{ minThreads?: number; maxThreads?: number }",
              defaultVal: "2 / CPU cores",
              impact: "Sizes core::DynamicWorkerPool.",
              values: [
                { value: "minThreads", behavior: "Minimum warm worker threads kept alive at idle to avoid cold-spawn latency." },
                { value: "maxThreads", behavior: "Hard ceiling on worker threads spawned during CPU spikes." },
              ],
            },
            {
              option: "security",
              type: "{ defaultDeny?: boolean; jwtSecret?: string }",
              defaultVal: "defaultDeny: false",
              impact: "Configures C++ SecurityGateway.",
              values: [
                { value: "defaultDeny: true", behavior: "All operations require authentication unless an operation explicitly sets auth: { allowAnonymous: true }." },
                { value: "defaultDeny: false", behavior: "Operations allow anonymous callers unless roles/scopes/tenants or allowAnonymous: false are set." },
                { value: "jwtSecret", behavior: "HS256 secret used natively in C++ to verify bearer JWT tokens and populate ctx.auth." },
              ],
            },
            {
              option: "allowOrigin",
              type: "boolean | AllowOriginResolver",
              defaultVal: "false",
              impact: "Controls browser WebSocket cross-origin access.",
              values: [
                { value: "false", behavior: "Denies all cross-origin browser requests (mode: deny_all)." },
                { value: "true", behavior: "Allows any origin (mode: allow_all, recommended only for local dev)." },
                { value: "(expose, callback) => boolean", behavior: "Calls expose(['https://...']) and callback(); returning true activates allow_list, returning false denies all." },
              ],
            },
          ],
        },
      },
    ],
  },
  {
    id: "routing",
    title: "Lamp Router & Nested Routing",
    group: "Core Concepts",
    summary:
      "How createLampRouter builds hierarchical route trees, merges options across nesting levels, detects cycles, and mounts onto LampApp.",
    subtopics: [
      {
        id: "router-layer",
        title: "Route Layer & Nesting Mechanics",
        paragraphs: [
          "createLampRouter(config) creates a route node with a relative target segment (e.g. 'users'). It holds zero business logic itself—instead it aggregates registries and child routers.",
          "When app.registerRouter(router, mountOptions) runs, resolveOperations() performs a depth-first traversal using an active-stack Set to detect circular nesting (throwing CIRCULAR_ROUTER_NESTING if a cycle exists) and a seen-ID Set to catch collisions.",
        ],
        code: {
          filename: "src/modules/users/users.router.ts",
          language: "ts",
          content: `import { createLampRouter } from "lamp-app";
import { usersRegistry } from "./users.registry.ts";
import { localUsersRouter } from "./local_users/local_users.router.ts";

export const usersRouter = createLampRouter({
  description: "Users route module",
  target: "users",
  registry: [usersRegistry],
  nesting: [localUsersRouter],
  options: {
    tags: ["users"],
    cache: { ttl: 60 },
  },
});`,
        },
      },
      {
        id: "router-config-table",
        title: "LampRouterConfig & Option Precedence",
        paragraphs: [
          "Options cascade in order: mountOptions -> parentRouter.options -> childRouter.options -> operation.definition. More specific layers override less specific ones; tags are merged as a deduplicated union.",
        ],
        configTable: {
          title: "LampRouterConfig & LampRouterOptions",
          rows: [
            {
              option: "target",
              type: "string (required)",
              defaultVal: "—",
              impact: "Relative segment (e.g. 'users' or 'v1.users') prepended to all attached registries and nested routers.",
            },
            {
              option: "registry",
              type: "LampRegistry | LampRegistry[]",
              defaultVal: "[]",
              impact: "Accepts a single registry or an array of registries mounted at this router's target path.",
            },
            {
              option: "nesting",
              type: "LampRouter | LampRouter[]",
              defaultVal: "[]",
              impact: "Child routers mounted under this router's target. Supports arbitrary nesting depth.",
            },
            {
              option: "options.execution",
              type: "OperationExecutionConfig",
              defaultVal: "inherited",
              impact: "Shallow-merged with parent and per-operation execution settings (mode, concurrency, timeoutMs).",
            },
            {
              option: "options.cache",
              type: "boolean | OperationCacheConfig",
              defaultVal: "inherited",
              impact: "Child or operation cache value overrides parent when defined.",
            },
            {
              option: "options.auth",
              type: "OperationAuthConfig",
              defaultVal: "inherited",
              impact: "Shallow-merged per-field (allowAnonymous, roles, scopes, tenants) across router hierarchy.",
            },
            {
              option: "options.tags",
              type: "string[]",
              defaultVal: "[]",
              impact: "Deduplicated union across mount, parent router, child router, and operation.",
            },
            {
              option: "onError / options.onError",
              type: "LampErrorHandler",
              defaultVal: "undefined",
              impact: "Route-level error handler inherited by nested routers and registries unless overridden.",
            },
          ],
        },
      },
    ],
  },
  {
    id: "registry",
    title: "Registry Layer & Path Structure",
    group: "Core Concepts",
    summary:
      "How createLampRegistry isolates business logic using single-segment suffixes and compiles into canonical dotted operation IDs.",
    subtopics: [
      {
        id: "registry-suffix-convention",
        title: "Single-Segment Suffix Validation",
        paragraphs: [
          "registry.add(operationSuffix, definition) enforces that operationSuffix matches /^[a-zA-Z0-9_-]+$/ (no dots, slashes, or spaces). Passing 'users.get' throws INVALID_OPERATION_SUFFIX.",
          "This guarantees registries remain location-independent and reusable under any router target or mount prefix.",
        ],
        code: {
          filename: "src/modules/users/users.registry.ts",
          language: "ts",
          content: `import { createLampRegistry, s } from "lamp-app";

export const usersRegistry = createLampRegistry()
  .add("get", {
    input: s.object({ id: s.string({ minLength: 1 }) }),
    output: s.object({ id: s.string(), name: s.string() }),
    handler: ({ input }) => ({ id: input.id, name: "Ada Lovelace" }),
  })
  .add("delete", {
    input: s.object({ id: s.string({ minLength: 1 }) }),
    handler: ({ input }) => ({ deleted: true, id: input.id }),
  });`,
        },
      },
      {
        id: "path-resolution",
        title: "Canonical Dotted Path Resolution",
        paragraphs: [
          "Paths join [mountPrefix, ...routerTargets, registrySuffix] with '.':",
        ],
        configTable: {
          title: "Path Resolution Mapping",
          rows: [
            {
              option: "rootRouter ('app') + ping",
              type: "root operation",
              defaultVal: "app.ping",
              impact: "Mounted directly on root router.",
            },
            {
              option: "app -> users + get",
              type: "module operation",
              defaultVal: "app.users.get",
              impact: "Standard feature module.",
            },
            {
              option: "app -> users -> local_users + get",
              type: "nested submodule",
              defaultVal: "app.users.local_users.get",
              impact: "Multi-level nested submodule.",
            },
            {
              option: "registerRouter(usersRouter, { prefix: 'v2' }) + get",
              type: "prefixed mount",
              defaultVal: "v2.users.get",
              impact: "Mount-time prefix override.",
            },
          ],
        },
      },
    ],
  },
  {
    id: "operations",
    title: "Operations, Schemas & Handlers",
    group: "Core Concepts",
    summary:
      "Schema validation with s.*, native binary file hydration with s.file(), and OperationContext inside handlers.",
    subtopics: [
      {
        id: "operation-definition",
        title: "OperationDefinition & Handler Context",
        paragraphs: [
          "Handlers receive { input, ctx, signal }. Input is validated before execution; output is validated before LCB1 encoding. Any __lampFileRef placeholders in input are hydrated into LampFile objects ({ name, mimeType, size, data: Uint8Array }).",
        ],
        code: {
          filename: "files.registry.ts",
          language: "ts",
          content: `filesRegistry.add("upload", {
  input: s.object({
    label: s.string({ minLength: 1 }),
    file: s.file({ maxSizeBytes: 5 * 1024 * 1024 }),
  }),
  output: s.object({ name: s.string(), size: s.number() }),
  handler: ({ input, ctx }) => ({
    name: input.file.name,
    size: input.file.data.byteLength,
  }),
});`,
        },
      },
      {
        id: "operation-config-table",
        title: "OperationDefinition Options & Values",
        paragraphs: [
          "Per-operation settings override router and app defaults:",
        ],
        configTable: {
          title: "OperationDefinition Fields",
          rows: [
            {
              option: "execution.concurrency",
              type: `"concurrent" | "serialized"`,
              defaultVal: `"concurrent"`,
              impact: "Controls simultaneous execution isolation.",
              values: [
                { value: `"concurrent"`, behavior: "Allows multiple simultaneous invocations of this operation to run in parallel." },
                { value: `"serialized"`, behavior: "Serializes invocations for operations mutating non-thread-safe shared state." },
              ],
            },
            {
              option: "execution.timeoutMs",
              type: "number",
              defaultVal: "30000",
              impact: "Deadline in milliseconds; aborts ctx.signal and returns DEADLINE_EXCEEDED if exceeded.",
            },
            {
              option: "cache",
              type: "boolean | { enabled?: boolean; ttl: number }",
              defaultVal: "false",
              impact: "Per-operation cache override.",
              values: [
                { value: "false", behavior: "Explicitly bypasses cache even if router enables caching." },
                { value: "true", behavior: "Uses app's defaultTtl if app-level cache is enabled." },
                { value: "{ ttl: number }", behavior: "Caches for ttl seconds (clamped to server maxTtlLimitSeconds)." },
              ],
            },
            {
              option: "auth",
              type: "OperationAuthConfig",
              defaultVal: "inherited",
              impact: "Per-operation access control.",
              values: [
                { value: "allowAnonymous", behavior: "true permits unauthenticated calls; false requires valid credentials." },
                { value: "roles / scopes / tenants", behavior: "Arrays of required caller roles, permission scopes, or tenant IDs." },
              ],
            },
          ],
        },
      },
    ],
  },
  {
    id: "error-handling",
    title: "Error Handling (createErrorHandler)",
    group: "Core Concepts",
    summary:
      "Creating reusable error normalizers with createErrorHandler and scoping them at registry level or route level.",
    subtopics: [
      {
        id: "create-error-handler",
        title: "Registry-Level & Route-Level Error Handlers",
        paragraphs: [
          "createErrorHandler(config) builds a LampErrorHandler that catches any thrown value, normalizes it to LampError, optionally transforms/redacts it, logs it, and invokes an async onError hook.",
          "Precedence: registry.onError / createLampRegistry({ onError }) overrides router.options.onError / createLampRouter({ onError }), which overrides parent router onError.",
        ],
        code: {
          filename: "users.registry.ts",
          language: "ts",
          content: `import { LampError, createErrorHandler, createLampRegistry } from "lamp-app";

const usersErrorHandler = createErrorHandler({
  log: true,
  includeStack: false,
  defaultCode: "INTERNAL",
  transform: (err, ctx) =>
    new LampError(\`[\${ctx.operationId}] \${err.message}\`, {
      code: err.code,
      identifier: err.identifier,
    }),
});

export const usersRegistry = createLampRegistry({ onError: usersErrorHandler });`,
        },
      },
      {
        id: "error-handler-config",
        title: "LampErrorHandlerConfig Options & Values",
        paragraphs: [
          "Configuration options for createErrorHandler:",
        ],
        configTable: {
          title: "LampErrorHandlerConfig",
          rows: [
            {
              option: "log",
              type: "boolean",
              defaultVal: "true",
              impact: "Controls console.error output on failure.",
              values: [
                { value: "true", behavior: "Logs [lamp:error] <op> [<code>/<identifier>]: <message>." },
                { value: "false", behavior: "Suppresses automatic error logging." },
              ],
            },
            {
              option: "includeStack",
              type: "boolean",
              defaultVal: "false",
              impact: "Controls stack trace attachment on normalized non-Lamp errors.",
              values: [
                { value: "true", behavior: "Stores err.stack in details.stack (useful in development)." },
                { value: "false", behavior: "Omits stack traces from error details (recommended in production)." },
              ],
            },
            {
              option: "defaultCode",
              type: "LampErrorCode",
              defaultVal: `"EXECUTION_FAILED"`,
              impact: "Canonical error category for plain JS Error throws (e.g. INVALID_ARGUMENT, NOT_FOUND, INTERNAL, UNAVAILABLE).",
            },
            {
              option: "transform",
              type: "(err: LampError, ctx) => LampError | void",
              defaultVal: "undefined",
              impact: "Mutates or replaces the LampError returned to the client.",
            },
            {
              option: "onError",
              type: "(err: LampError, ctx) => void | Promise<void>",
              defaultVal: "undefined",
              impact: "Async/sync side-effect callback for telemetry, metrics, or external alerting.",
            },
          ],
        },
      },
    ],
  },
  {
    id: "caching",
    title: "Full-Cycle Wire Caching",
    group: "Runtime & Performance",
    summary:
      "Zero-V8 binary response caching in C++ FullCycleWireCache with deterministic key building and server-clamped TTLs.",
    subtopics: [
      {
        id: "wire-cache-how",
        title: "How FullCycleWireCache Works",
        paragraphs: [
          "FullCycleWireCache stores final LCB1 binary response frames directly in C++ sharded LRU+TTL memory. Cache hits bypass V8 entirely and averaged 0.59 ms in 20-run benchmarks (3.91x faster than uncached reads).",
          "TTLs exceeding maxTtlLimitSeconds are clamped automatically by the server to prevent unbounded stale data.",
        ],
        code: {
          filename: "cache.ts",
          language: "ts",
          content: `const app = createLampApp({
  cache: { enabled: true, defaultTtl: 60, maxTtlLimitSeconds: 120 },
});

usersRegistry.add("get", {
  input: s.object({ id: s.string() }),
  cache: { ttl: 60 },
  handler: ({ input }) => findUser(input.id),
});`,
        },
      },
    ],
  },
  {
    id: "parallelism",
    title: "Parallelism & DynamicWorkerPool",
    group: "Runtime & Performance",
    summary:
      "True multi-core JavaScript execution via ctx.parallel and isolated C++ worker V8 isolates.",
    subtopics: [
      {
        id: "ctx-parallel",
        title: "Using ctx.parallel",
        paragraphs: [
          "ctx.parallel(items, mapper) distributes subtasks across core::DynamicWorkerPool threads, each running an isolated V8 context (V8SubtaskIsolatePool). Concurrent light reads stay at ~1.3 ms p95 even under heavy CPU bursts.",
        ],
        code: {
          filename: "compute.registry.ts",
          language: "ts",
          content: `computeRegistry.add("parallel", {
  input: s.object({ items: s.array(s.number()) }),
  output: s.object({ results: s.array(s.number()) }),
  handler: async ({ input, ctx }) => ({
    results: await ctx.parallel(input.items, (n) => (n * 2654435761) >>> 0),
  }),
});`,
        },
        callout: {
          kind: "warning",
          title: "Pure Isolated Subtasks",
          body: "Subtasks passed to ctx.parallel execute in separate V8 isolates and cannot capture outer JS closures. Pass all inputs via the item argument.",
        },
      },
    ],
  },
  {
    id: "security",
    title: "Security & Origin Policy",
    group: "Runtime & Performance",
    summary:
      "Native C++ origin allowance (allowOrigin), JWT verification, and RBAC/scope/tenant enforcement.",
    subtopics: [
      {
        id: "allow-origin",
        title: "Origin Allowance (allowOrigin)",
        paragraphs: [
          "allowOrigin configures core::OriginPolicy natively in C++ before any WebSocket or binary operation is dispatched.",
        ],
        code: {
          filename: "src/app.ts",
          language: "ts",
          content: `const app = createLampApp({
  allowOrigin: (expose, callback) => {
    expose(["http://localhost:5173", "https://app.example.com"]);
    callback();
    return true;
  },
});`,
        },
      },
    ],
  },
  {
    id: "cli",
    title: "Lamp CLI & Bundler",
    group: "Tooling & Clients",
    summary:
      "Complete reference for create-lamp-app, lamp add, lamp serve --watch, and lamp build.",
    subtopics: [
      {
        id: "cli-commands",
        title: "CLI Command Reference",
        paragraphs: [
          "The lamp CLI handles project scaffolding, module generation, watch-mode restarts, and multi-file TypeScript bundling:",
        ],
        configTable: {
          title: "Lamp CLI Commands",
          rows: [
            {
              option: "npx create-lamp-app <name> [--yes]",
              type: "scaffolder",
              defaultVal: "—",
              impact: "Creates package.json, tsconfig.json, src/app.ts, src/router.ts, and src/registry.ts.",
            },
            {
              option: "npx lamp add <module[/sub]>",
              type: "generator",
              defaultVal: "—",
              impact: "Creates <name>.router.ts and <name>.registry.ts and auto-registers into parent router.",
            },
            {
              option: "lamp serve <entry> [--port 3000] [--watch|-w]",
              type: "runtime",
              defaultVal: "port 3000",
              impact: "Bundles TypeScript and launches C++ lamp_server; --watch restarts automatically on file edits.",
            },
            {
              option: "lamp build [src/app.ts] [--out dist/app.js]",
              type: "bundler",
              defaultVal: "dist/app.js",
              impact: "Compiles all TypeScript modules into a single dependency-optimized low-level JS bundle.",
            },
          ],
        },
      },
    ],
  },
  {
    id: "client-integration",
    title: "Client Integration (lamp-client)",
    group: "Tooling & Clients",
    summary:
      "Connect Node.js (lmp:// TCP) and Browser/React/Vite (ws:// binary WebSocket) applications to a Lamp backend.",
    subtopics: [
      {
        id: "node-client",
        title: "Node.js TCP Client (lmp://)",
        paragraphs: [
          "Install lamp-client in your consumer service and connect over persistent multiplexed TCP using createLampClient({ url: 'lmp://127.0.0.1:3000' }).",
        ],
        code: {
          filename: "client.ts",
          language: "ts",
          content: `import { createLampClient, isLampClientError } from "lamp-client";

const client = createLampClient({ url: "lmp://127.0.0.1:3000" });

try {
  const manifest = await client.manifest();
  const { data, meta } = await client.callWithMeta("app.users.get", { id: "u_1" });
  console.log(data, { cacheHit: meta.cacheHit, bytes: meta.responseBytes });
} catch (err) {
  if (isLampClientError(err)) {
    console.error(err.code, err.identifier, err.message);
  }
} finally {
  await client.close();
}`,
        },
      },
      {
        id: "browser-client",
        title: "Browser / Vite React Client (ws://)",
        paragraphs: [
          "Import from 'lamp-client/browser' for zero-Node-dependency binary WebSocket communication in React/Vite apps. Ensure allowOrigin on the server permits your frontend origin.",
        ],
        code: {
          filename: "App.tsx",
          language: "tsx",
          content: `import { createLampClient } from "lamp-client/browser";

const client = createLampClient({ url: "ws://127.0.0.1:3000" });

const uploaded = await client.callWithMeta("app.files.upload", {
  label: "avatar",
  file: selectedFile,
});

const stats = await client.stats();`,
        },
      },
    ],
  },
];

export const DOC_GROUPS: readonly DocGroup[] = [
  "Foundations",
  "Core Concepts",
  "Runtime & Performance",
  "Tooling & Clients",
];
