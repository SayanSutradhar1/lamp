// ============================================================
// Lamp TypeScript SDK - Public Entry Point
// ============================================================

export { createLampApp, isValidOperationId, type LampApp, type LifecycleHook } from "./app.ts";
export {
  LampError,
  isLampError,
  normalizeLampError,
  type LampErrorCode,
  type LampErrorOptions,
} from "./errors.ts";
export { resolveNativeHostBridge } from "./native_bridge.ts";
export {
  s,
  validateSchema,
  type AnySchema,
  type ExternalParseSchema,
  type InferSchema,
  type LampSchema,
  type NumberSchemaOptions,
  type ObjectShape,
  type SchemaIssue,
  type SchemaParseResult,
  type StringSchemaOptions,
} from "./schema.ts";
export type {
  AppCacheConfig,
  AppSecurityConfig,
  CacheModePolicy,
  ConcurrencyPolicy,
  ExecutionPolicy,
  InvokeOptions,
  LampAppConfig,
  LampAppState,
  LampStartInfo,
  NativeHostBridge,
  OperationAuthConfig,
  OperationCacheConfig,
  OperationContext,
  OperationDefinition,
  OperationExecutionConfig,
  OperationHandlerArgs,
  PrincipalContext,
  RegisteredOperationMetadata,
  SerializationPolicy,
  TransportPolicy,
} from "./types.ts";
