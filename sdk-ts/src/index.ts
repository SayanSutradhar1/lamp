/**
 * Lamp Server-Side Contract SDK (`lamp`).
 *
 * Declares typed non-REST operation contracts, input/output schemas (including native binary
 * files via `s.file()`), lifecycle hooks, parallel worker splitting (`ctx.parallel`), and
 * full-cycle caching policies executed by the C++ native backend runtime (`lamp_server`).
 *
 * Client contracts live in the separate `lamp-client` package.
 *
 * @packageDocumentation
 */

export { createLampApp, isValidOperationId } from "./app.ts";
export { LampError, isLampError, normalizeLampError } from "./errors.ts";
export { getNativeHostBridge, setNativeHostBridge } from "./native_bridge.ts";
export { s, validateWithSchema } from "./schema.ts";

export type { LampErrorCode, LampErrorOptions } from "./errors.ts";
export type {
  NativeHostBridge,
  NativeParallelTaskPayload,
} from "./native_bridge.ts";
export type {
  FileSchemaOptions,
  InferSchema,
  LampSchema,
  LampSchemaIssue,
  LampSchemaResult,
  NumberSchemaOptions,
  StringSchemaOptions,
} from "./schema.ts";
export type {
  InvokeCallOptions,
  LampApp,
  LampAppCacheConfig,
  LampAppConfig,
  LampAppState,
  LampExecutionMode,
  LampFile,
  LampParallelTaskSpec,
  LampSecurityConfig,
  LampSerializationMode,
  LampStartInfo,
  LampTransportMode,
  LampWorkerPoolConfig,
  LifecycleHook,
  OperationAuthConfig,
  OperationAuthContext,
  OperationCacheConfig,
  OperationConcurrencyMode,
  OperationContext,
  OperationDefinition,
  OperationExecutionConfig,
  OperationHandler,
  OperationHandlerArgs,
  RegisteredOperationMetadata,
} from "./types.ts";
