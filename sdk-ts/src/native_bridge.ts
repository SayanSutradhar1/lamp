import type { LampStartInfo, RegisteredOperationMetadata } from "./types.ts";

/**
 * Serialized parallel subtask payload passed from V8 handlers into the C++
 * `core::DynamicWorkerPool` via {@link NativeHostBridge.runParallel}.
 */
export interface NativeParallelTaskPayload {
  /** Self-contained JavaScript function source serialized via `fn.toString()`. */
  readonly fn: string;
  /** JSON-encoded argument forwarded into `fn(arg)` inside the worker isolate. */
  readonly inputJson: string;
}

/**
 * Native runtime bridge contract injected as `globalThis.__lampNativeBridge` when
 * application scripts execute inside the C++ `lamp_server` runtime.
 */
export interface NativeHostBridge {
  /**
   * Registers an operation descriptor and callback with the C++ `OperationRegistry`.
   *
   * @param id - Logical operation identifier (e.g. `"users.create"`).
   * @param meta - Normalized execution, caching, and security metadata.
   * @param invoker - Entry callback invoked by the C++ scheduler.
   * @returns Optional opaque numeric handler ID assigned natively.
   */
  registerOperation(
    id: string,
    meta: RegisteredOperationMetadata,
    invoker: (rawInputJson: string, metadataJson: string) => unknown
  ): number | void;

  /**
   * Optional C++ worker-pool parallel fan-out hook backed by `core::DynamicWorkerPool`.
   *
   * @param tasks - Serialized subtasks dispatched across dynamic worker threads.
   * @returns JSON-encoded return values aligned one-to-one with `tasks`.
   */
  runParallel?(tasks: readonly NativeParallelTaskPayload[]): string[];

  /**
   * Notifies the native host that all lifecycle `onStart` hooks have succeeded.
   *
   * @param infoJson - JSON-encoded {@link LampStartInfo}.
   */
  markStarted?(infoJson: string): void;

  /**
   * Notifies the native host that application shutdown hooks completed.
   */
  markStopped?(): void;

  /**
   * Configures the C++ runtime's origin allowance / CORS policy.
   *
   * @param policyJson - JSON-encoded {@link LampOriginPolicy}.
   */
  configureOriginAllowance?(policyJson: string): void;
}

declare global {
  // eslint-disable-next-line no-var
  var __lampNativeBridge: NativeHostBridge | undefined;
}

/**
 * Returns the active {@link NativeHostBridge} injected by the C++ runtime, if present.
 *
 * @returns The global bridge instance when running inside `lamp_server`, or `undefined` in contract unit tests.
 */
export function getNativeHostBridge(): NativeHostBridge | undefined {
  return globalThis.__lampNativeBridge;
}

/**
 * Installs or replaces the {@link NativeHostBridge} global hook (primarily used in runtime tests).
 *
 * @param bridge - Bridge implementation to register globally, or `undefined` to clear it.
 */
export function setNativeHostBridge(bridge: NativeHostBridge | undefined): void {
  globalThis.__lampNativeBridge = bridge;
}
