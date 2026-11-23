/**
 * Binary file structure returned by server handlers or passed directly inside JSON input payloads.
 */
export interface LampBinaryFile {
  /** Original file name (e.g. `"report.pdf"`). */
  readonly name: string;
  /** MIME media type (defaults to `"application/octet-stream"`). */
  readonly mimeType?: string;
  /** Raw binary payload bytes transferred directly in `LCB1` without base64 inflation. */
  readonly data: Uint8Array;
}

/**
 * Accepted client-side file input values: explicit {@link LampBinaryFile}, raw bytes,
 * or standard Web `Blob` / `File` objects (converted asynchronously without blocking).
 */
export type LampInputFile = LampBinaryFile | Blob;

/**
 * Per-invocation options passed to {@link LampClient.call} or {@link LampClient.callWithMeta}.
 */
export interface LampCallOptions {
  /** Optional explicit binary file attachments available in server `ctx.files`. */
  readonly files?: readonly LampInputFile[];
  /** Per-call timeout in milliseconds overriding client defaults. */
  readonly timeoutMs?: number;
  /** Cooperative `AbortSignal` cancelling pending local wait on abort. */
  readonly signal?: AbortSignal;
  /** Chunk size (in bytes) used when asynchronously streaming large binary payloads. */
  readonly streamChunkBytes?: number;
}

/**
 * Wire-level telemetry returned alongside decoded output by {@link LampClient.callWithMeta}.
 */
export interface LampCallResultMeta {
  /** Multiplexed wire request correlation ID. */
  readonly requestId: number;
  /** True when the response was served directly from the C++ `FullCycleWireCache` before reaching V8. */
  readonly cacheHit: boolean;
  /** Total encoded request frame size transmitted (header + LCB1 payload) in bytes. */
  readonly requestBytes: number;
  /** Total received binary response payload size in bytes. */
  readonly responseBytes: number;
  /** Decoded file attachments returned by the server handler, if any. */
  readonly files: readonly Required<LampBinaryFile>[];
}

/**
 * Envelope returned by {@link LampClient.callWithMeta} containing both decoded output and wire telemetry.
 *
 * @typeParam TOutput - Expected decoded result type.
 */
export interface LampCallResponse<TOutput> {
  /** Decoded response payload typed as `TOutput`. */
  readonly data: TOutput;
  /** Wire caching status, byte metrics, and attached binary response files. */
  readonly meta: LampCallResultMeta;
}

/**
 * Operation descriptor entry surfaced by `$lamp.manifest` over binary `LMP1`.
 */
export interface LampManifestOperation {
  /** Logical dotted operation identifier (for example `"users.create"`). */
  readonly id: string;
  /** Whether full-cycle wire caching is enabled for this operation. */
  readonly cacheEnabled: boolean;
  /** Configured server cache TTL in milliseconds (clamped to `cacheMaxTtlMs`). */
  readonly cacheTtlMs: number;
  /** Execution timeout bound in milliseconds. */
  readonly timeoutMs: number;
}

/**
 * Runtime operation catalog returned by {@link LampClient.manifest}.
 */
export interface LampManifest {
  /** Wire protocol banner (`"LMP1/LCB1"`). */
  readonly protocol: string;
  /** Explicit non-REST marker (`false`). */
  readonly restPattern: boolean;
  /** Registered operations advertised by the C++ backend host. */
  readonly operations: readonly LampManifestOperation[];
}

/**
 * Live worker-pool and full-cycle cache telemetry returned by {@link LampClient.stats}.
 */
export interface LampRuntimeStats {
  /** Currently executing worker tasks. */
  readonly activeWorkers: number;
  /** Active threads currently alive in `DynamicWorkerPool`. */
  readonly currentThreads: number;
  /** Highest worker thread count reached due to dynamic load scaling. */
  readonly peakThreads: number;
  /** Cumulative tasks completed by the dynamic pool. */
  readonly completedTasks: number;
  /** Cumulative full-cycle binary cache hits served without invoking V8. */
  readonly cacheHits: number;
  /** Cumulative cache misses dispatched into V8 execution. */
  readonly cacheMisses: number;
  /** Enforced server hard upper TTL ceiling in milliseconds. */
  readonly cacheMaxTtlMs: number;
}

/**
 * Connection configuration options accepted by {@link createLampClient}.
 */
export interface LampClientOptions {
  /**
   * Server address or endpoint URL (for example `"lmp://127.0.0.1:3000"`, `"127.0.0.1:3000"`,
   * or `"ws://127.0.0.1:3000"` in browsers).
   */
  readonly url: string;
  /** Default request timeout in milliseconds (defaults to `30000`). */
  readonly timeoutMs?: number;
  /** Async binary chunk size threshold when streaming large payloads (defaults to `65536`). */
  readonly streamChunkBytes?: number;
  /** Connect timeout in milliseconds when dialing the persistent socket (defaults to `5000`). */
  readonly connectTimeoutMs?: number;
}

/**
 * Persistent multiplexed binary client speaking `LMP1` + `LCB1` directly with the C++ backend.
 */
export interface LampClient {
  /**
   * Invokes a registered server operation using compact `LCB1` binary encoding over `LMP1`.
   *
   * Automatically converts nested binary files (`Uint8Array`, `LampBinaryFile`, or Web `File`/`Blob`)
   * asynchronously into zero-base64 binary frames.
   *
   * @typeParam TOutput - Expected response type.
   * @param operationId - Dotted logical operation ID (e.g. `"users.create"`).
   * @param input - Serializable request object optionally containing binary files.
   * @param options - Optional explicit file list, timeout, or abort signal.
   * @returns Decoded handler output.
   */
  call<TOutput = unknown>(
    operationId: string,
    input?: unknown,
    options?: LampCallOptions
  ): Promise<TOutput>;

  /**
   * Same as {@link call}, but also surfaces wire telemetry (`cacheHit`, byte counts, response files).
   *
   * @typeParam TOutput - Expected response type.
   * @param operationId - Dotted logical operation ID.
   * @param input - Serializable request object optionally containing binary files.
   * @param options - Optional explicit file list, timeout, or abort signal.
   * @returns Output value plus {@link LampCallResultMeta}.
   */
  callWithMeta<TOutput = unknown>(
    operationId: string,
    input?: unknown,
    options?: LampCallOptions
  ): Promise<LampCallResponse<TOutput>>;

  /**
   * Fetches the registered server operation catalog via `$lamp.manifest` over `LMP1`.
   *
   * @returns Server manifest summary.
   */
  manifest(): Promise<LampManifest>;

  /**
   * Fetches live worker scaling and full-cycle cache counters via `$lamp.stats` over `LMP1`.
   *
   * @returns Live runtime statistics.
   */
  stats(): Promise<LampRuntimeStats>;

  /**
   * Measures roundtrip latency with an `LMP1` wire-level Ping/Pong frame when supported.
   *
   * @returns Roundtrip time in milliseconds.
   */
  ping(): Promise<number>;

  /**
   * Closes the underlying persistent TCP socket or WebSocket connection cleanly.
   */
  close(): Promise<void>;
}
