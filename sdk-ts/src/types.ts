import type { LampSchema } from "./schema.ts";

/** Transport mode exposed by the C++ backend runtime (`"lmp1-binary"` multiplexed TCP/WebSocket or `"auto"`). */
export type LampTransportMode = "lmp1-binary" | "auto";

/** Compact wire codec used by the runtime binary pipeline (`"lcb1"` string-deduplicated binary or `"auto"`). */
export type LampSerializationMode = "lcb1" | "protobuf" | "auto";

/** Execution scheduling strategy selected for handlers (`"parallel-dynamic"` uses `DynamicWorkerPool`). */
export type LampExecutionMode =
  | "parallel-dynamic"
  | "adaptive"
  | "javascript"
  | "native"
  | "v8-worker";

/** Concurrency isolation hint declared per operation. */
export type OperationConcurrencyMode = "concurrent" | "serialized";

/** Lifecycle phase of a {@link LampApp} server contract instance. */
export type LampAppState = "created" | "running" | "stopped";

/** Async or synchronous application startup/shutdown hook callback. */
export type LifecycleHook = () => void | Promise<void>;

/**
 * Binary file payload contract transferred directly in `LCB1` binary frames without base64 bloat.
 */
export interface LampFile {
  /** Internal runtime marker identifying serialized file structures. */
  readonly __lampFile?: true;
  /** Original file name (for example `"avatar.png"`). */
  readonly name: string;
  /** MIME content type (defaults to `"application/octet-stream"`). */
  readonly mimeType: string;
  /** File byte length (`data.byteLength`). */
  readonly size: number;
  /** Raw binary bytes backed directly by a V8 `Uint8Array`. */
  readonly data: Uint8Array;
}

/**
 * Explicit subtask specification passed to {@link OperationContext.parallel}.
 *
 * @typeParam TInput - Subtask argument type.
 * @typeParam TOutput - Subtask return type.
 */
export interface LampParallelTaskSpec<TInput = unknown, TOutput = unknown> {
  /** Self-contained function executed independently inside a C++ worker thread isolate. */
  readonly fn: (input: TInput) => TOutput | Promise<TOutput>;
  /** Serializable input argument forwarded to `fn`. */
  readonly input: TInput;
}

/**
 * Full request-response cycle cache configuration declared at app creation time.
 */
export interface LampAppCacheConfig {
  /** Enable full-cycle binary wire caching (`FullCycleWireCache`). */
  readonly enabled?: boolean;
  /** Caching mode selector (`"local"` L1 wire cache or `"disabled"`). */
  readonly mode?: "local" | "disabled";
  /** Default time-to-live (in seconds) applied to operations that specify `cache: true`. */
  readonly defaultTtl?: number;
  /** Hard upper TTL limit (in seconds); requested TTLs exceeding this cap are clamped natively. */
  readonly maxTtlLimitSeconds?: number;
}

/**
 * Application-level server security configuration enforced natively by the C++ runtime.
 */
export interface LampSecurityConfig {
  /** When `true`, registered operations require explicit authentication metadata unless overridden. */
  readonly defaultDeny?: boolean;
  /** Allowed bearer JWT secret when using built-in HS256 token verification. */
  readonly jwtSecret?: string;
}

/**
 * Dynamic C++ core thread-pool configuration (`core::DynamicWorkerPool`).
 */
export interface LampWorkerPoolConfig {
  /** Minimum warm worker threads maintained at idle. */
  readonly minThreads?: number;
  /** Maximum worker threads spawned dynamically during request or subtask spikes. */
  readonly maxThreads?: number;
}

/**
 * Top-level configuration passed to `createLampApp`.
 */
export interface LampAppConfig {
  /** Wire transport selector (`"lmp1-binary"` recommended; zero REST endpoints). */
  readonly transport?: LampTransportMode;
  /** Binary wire serialization codec (`"lcb1"` compact binary default). */
  readonly serialization?: LampSerializationMode;
  /** Execution strategy across C++ threads and V8 isolates. */
  readonly execution?: LampExecutionMode;
  /** Full-cycle wire response cache toggle or configuration with TTL upper limit. */
  readonly cache?: boolean | LampAppCacheConfig;
  /** Dynamic C++ worker pool sizing bounds. */
  readonly workers?: LampWorkerPoolConfig;
  /** Server security policies enforced before dispatching handlers. */
  readonly security?: LampSecurityConfig;
}

/**
 * Per-operation full-cycle cache policy overriding app defaults.
 */
export interface OperationCacheConfig {
  /** Explicit toggle for caching full serialized responses for this operation. */
  readonly enabled?: boolean;
  /** Time-to-live in seconds (automatically clamped to server `maxTtlLimitSeconds`). */
  readonly ttl: number;
  /** Optional operation-level soft cap note (server hard limit still takes precedence). */
  readonly maxTtlLimitSeconds?: number;
}

/**
 * Per-operation authentication and RBAC/scope policy.
 */
export interface OperationAuthConfig {
  /** Allow unauthenticated callers (`true` unless `security.defaultDeny` is enabled). */
  readonly allowAnonymous?: boolean;
  /** Required caller roles (any matching role satisfies policy unless configured otherwise). */
  readonly roles?: readonly string[];
  /** Required OAuth/session permission scopes. */
  readonly scopes?: readonly string[];
  /** Permitted tenant identifiers when multi-tenancy is active. */
  readonly tenants?: readonly string[];
}

/**
 * Per-operation execution scheduling options.
 */
export interface OperationExecutionConfig {
  /** Preferred execution mode (`"parallel-dynamic"`, `"adaptive"`, `"javascript"`, etc.). */
  readonly mode?: LampExecutionMode;
  /** Concurrency expectation across simultaneous invocations. */
  readonly concurrency?: OperationConcurrencyMode;
  /** Maximum execution deadline in milliseconds enforced by the runtime. */
  readonly timeoutMs?: number;
}

/**
 * Verified caller identity hydrated by the C++ security layer onto `ctx.auth`.
 */
export interface OperationAuthContext {
  /** `true` when the request carried a validated identity. */
  readonly authenticated: boolean;
  /** Subject identifier (for example user ID or service principal). */
  readonly subject?: string;
  /** Authenticated tenant identifier, if any. */
  readonly tenant?: string;
  /** Authentication mechanism name (for example `"bearer"` or `"hmac"`). */
  readonly scheme?: string;
  /** Assigned authorization roles. */
  readonly roles: readonly string[];
  /** Granted permission scopes. */
  readonly scopes: readonly string[];
}

/**
 * Runtime execution context supplied to every registered operation handler.
 */
export interface OperationContext {
  /** Monotonic per-invocation numeric ID assigned by the runtime. */
  readonly requestId: number;
  /** Logical operation name currently being executed. */
  readonly operationId: string;
  /** Trusted transport and runtime metadata entries. */
  readonly metadata: Readonly<Record<string, string>>;
  /** Native binary file attachments delivered alongside the request in `LCB1`. */
  readonly files: readonly LampFile[];
  /** Verified caller authentication state. */
  readonly auth: OperationAuthContext;
  /** Cooperative cancellation signal aborted on timeout or disconnect. */
  readonly signal: AbortSignal;

  /**
   * Splits work into independent subtasks executed concurrently across dynamic C++
   * worker threads (`core::DynamicWorkerPool`).
   *
   * Note: Each subtask runs in an isolated worker context and therefore must be
   * self-contained (passing data explicitly as arguments rather than relying on outer closures).
   *
   * @typeParam TInput - Per-item argument type.
   * @typeParam TOutput - Subtask result type.
   * @param items - Array of input values distributed across worker threads.
   * @param mapper - Self-contained pure function evaluated for each item.
   * @returns Results array aligned in the exact input order.
   */
  parallel<TInput, TOutput>(
    items: readonly TInput[],
    mapper: (item: TInput) => TOutput
  ): Promise<TOutput[]>;

  /**
   * Executes an array of independent task functions or `{ fn, input }` specs across
   * dynamic C++ worker threads.
   *
   * @typeParam TOutput - Subtask return type.
   * @param tasks - Array of parameterless functions or `{ fn, input }` task descriptors.
   * @returns Results array aligned with `tasks`.
   */
  parallel<TOutput>(
    tasks: readonly (LampParallelTaskSpec<any, TOutput> | (() => TOutput))[]
  ): Promise<TOutput[]>;
}

/**
 * Arguments object destructured by every server operation handler function.
 *
 * @typeParam TInput - Schema-validated request payload type.
 */
export interface OperationHandlerArgs<TInput> {
  /** Validated input payload (including any hydrated {@link LampFile} objects). */
  readonly input: TInput;
  /** Runtime execution context (files, auth, metadata, cancellation, `parallel()`). */
  readonly ctx: OperationContext;
  /** Cooperative `AbortSignal` alias matching `ctx.signal`. */
  readonly signal: AbortSignal;
}

/**
 * Handler function type implemented for each registered server operation.
 *
 * @typeParam TInput - Validated input type.
 * @typeParam TOutput - Validated response output type.
 */
export type OperationHandler<TInput, TOutput> = (
  args: OperationHandlerArgs<TInput>
) => TOutput | Promise<TOutput>;

/**
 * Complete contract definition registered via `app.register(operationId, definition)`.
 *
 * @typeParam TInput - Operation input payload type.
 * @typeParam TOutput - Operation response payload type.
 */
export interface OperationDefinition<TInput = unknown, TOutput = unknown> {
  /** Human-readable operation summary surfaced in binary `$lamp.manifest`. */
  readonly description?: string;
  /** Optional input schema validator (`s.object(...)`, `s.file()`, etc.). */
  readonly input?: LampSchema<TInput>;
  /** Optional output schema validator enforced before encoding binary `LCB1` responses. */
  readonly output?: LampSchema<TOutput>;
  /** Execution and timeout overrides. */
  readonly execution?: OperationExecutionConfig;
  /** Full-cycle wire cache toggle or `{ ttl }` policy (capped by server upper TTL limit). */
  readonly cache?: boolean | OperationCacheConfig;
  /** Authentication, role, scope, and tenant requirements. */
  readonly auth?: OperationAuthConfig;
  /** Optional classification tags (for example `["users", "read"]`). */
  readonly tags?: readonly string[];
  /** Operation implementation executed by the Lamp runtime. */
  readonly handler: OperationHandler<TInput, TOutput>;
}

/**
 * Normalized immutable metadata describing a registered server operation.
 */
export interface RegisteredOperationMetadata {
  /** Unique logical operation name (e.g. `"users.create"`). */
  readonly id: string;
  /** Optional operation description. */
  readonly description?: string;
  /** Effective execution mode, concurrency, and timeout bounds. */
  readonly execution: Required<OperationExecutionConfig>;
  /** Effective full-cycle wire cache policy (TTL clamped to configured upper limit). */
  readonly cache: {
    readonly enabled: boolean;
    readonly ttlSeconds: number;
    readonly maxTtlLimitSeconds: number;
  };
  /** Effective authorization policy. */
  readonly auth: Required<OperationAuthConfig>;
  /** Attached classification tags. */
  readonly tags: readonly string[];
}

/**
 * Runtime configuration summary returned when `app.start()` resolves.
 */
export interface LampStartInfo {
  /** Active wire transport mode. */
  readonly transport: LampTransportMode;
  /** Active compact binary serialization mode. */
  readonly serialization: LampSerializationMode;
  /** Active execution scheduling strategy. */
  readonly execution: LampExecutionMode;
  /** Sorted list of registered operation identifiers. */
  readonly operations: readonly string[];
}

/**
 * Optional call parameters for local contract testing via {@link LampApp.invoke}.
 */
export interface InvokeCallOptions {
  /** Trusted metadata map injected into `ctx.metadata` and `ctx.auth`. */
  readonly metadata?: Readonly<Record<string, string>>;
  /** Optional attached binary files passed into `ctx.files`. */
  readonly files?: readonly LampFile[];
  /** External `AbortSignal` forwarded to the handler. */
  readonly signal?: AbortSignal;
}

/**
 * Server-side application contract container managing operation schemas, lifecycle hooks,
 * caching policies, and native C++ bridge registration.
 */
export interface LampApp {
  /**
   * Registers a strongly-typed non-REST operation contract before `start()` is called.
   *
   * @typeParam TInput - Operation input type.
   * @typeParam TOutput - Operation output type.
   * @param operationId - Dotted logical identifier (matching `/^[a-zA-Z0-9_-]+(\.[a-zA-Z0-9_-]+)*$/`).
   * @param definition - Input/output schemas, cache/auth options, and handler callback.
   * @returns `this` app instance for fluent chaining.
   */
  register<TInput = undefined, TOutput = unknown>(
    operationId: string,
    definition: OperationDefinition<TInput, TOutput>
  ): LampApp;

  /**
   * Executes registered `onStart` hooks and signals the C++ native runtime that the app is ready.
   *
   * @returns Resolved startup metadata including registered operation names.
   */
  start(): Promise<LampStartInfo>;

  /**
   * Invokes an operation directly against the server contract (for local unit tests or C++ dispatcher callbacks).
   *
   * @typeParam TOutput - Expected validated response type.
   * @param operationId - Registered operation name.
   * @param input - Input payload passed to handler validation.
   * @param options - Optional metadata, binary files, or cancellation signal.
   * @returns Validated handler return value.
   */
  invoke<TOutput = unknown>(
    operationId: string,
    input?: unknown,
    options?: InvokeCallOptions
  ): Promise<TOutput>;

  /**
   * Runs `onStop` hooks in reverse registration order and marks the app stopped.
   */
  stop(): Promise<void>;

  /** Returns current app lifecycle phase (`"created"`, `"running"`, or `"stopped"`). */
  state(): LampAppState;

  /** Returns normalized metadata snapshots for all registered operations. */
  operations(): readonly RegisteredOperationMetadata[];

  /**
   * Registers a hook executed during `start()` in FIFO order.
   *
   * @param hook - Lifecycle startup function.
   * @returns `this` app instance for fluent chaining.
   */
  onStart(hook: LifecycleHook): LampApp;

  /**
   * Registers a cleanup hook executed during `stop()` in LIFO order.
   *
   * @param hook - Lifecycle shutdown function.
   * @returns `this` app instance for fluent chaining.
   */
  onStop(hook: LifecycleHook): LampApp;
}
