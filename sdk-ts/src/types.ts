// ============================================================
// Lamp TypeScript SDK - Public Application & Operation Types
// ============================================================

import type { AnySchema, InferSchema } from "./schema.ts";

export type TransportPolicy = "auto" | "http" | "binary-rpc";
export type SerializationPolicy = "auto" | "protobuf" | "json" | "binary";
export type ExecutionPolicy = "adaptive" | "native" | "javascript";
export type ConcurrencyPolicy = "concurrent" | "serialized";
export type CacheModePolicy = "disabled" | "local" | "layered";

export interface AppCacheConfig {
  enabled?: boolean;
  mode?: CacheModePolicy;
  defaultTtl?: number; // seconds
  maxEntryBytes?: number;
  staleWhileRevalidate?: boolean;
}

export interface AppSecurityConfig {
  defaultDeny?: boolean;
  maxInputBytes?: number;
  maxOutputBytes?: number;
  rateLimitPerMinute?: number;
}

export interface LampAppConfig {
  transport?: TransportPolicy;
  serialization?: SerializationPolicy;
  execution?: ExecutionPolicy;
  cache?: boolean | AppCacheConfig;
  security?: AppSecurityConfig;
  nativeBridge?: NativeHostBridge;
}

export interface OperationCacheConfig {
  enabled?: boolean;
  mode?: CacheModePolicy;
  ttl?: number; // seconds
  maxEntryBytes?: number;
  staleWhileRevalidate?: boolean;
}

export interface OperationExecutionConfig {
  mode?: ExecutionPolicy;
  concurrency?: ConcurrencyPolicy;
  timeoutMs?: number;
  maxInputBytes?: number;
  maxOutputBytes?: number;
}

export interface OperationAuthConfig {
  allowAnonymous?: boolean;
  roles?: readonly string[];
  scopes?: readonly string[];
  tenants?: readonly string[];
}

export interface PrincipalContext {
  authenticated: boolean;
  subject?: string;
  tenant?: string;
  scheme?: string;
  roles: readonly string[];
  scopes: readonly string[];
}

export interface OperationContext {
  requestId: number;
  operationId: string;
  traceId?: string;
  spanId?: string;
  metadata: Readonly<Record<string, string>>;
  signal: AbortSignal;
  auth: PrincipalContext;
}

export interface OperationHandlerArgs<Input> {
  input: Input;
  ctx: OperationContext;
  signal: AbortSignal;
}

export interface OperationDefinition<
  InputSchema extends AnySchema | undefined = undefined,
  OutputSchema extends AnySchema | undefined = undefined,
> {
  description?: string;
  input?: InputSchema;
  output?: OutputSchema;
  cache?: boolean | OperationCacheConfig;
  execution?: OperationExecutionConfig;
  auth?: OperationAuthConfig;
  tags?: readonly string[];
  handler: (
    args: OperationHandlerArgs<
      InputSchema extends AnySchema ? InferSchema<InputSchema> : unknown
    >
  ) =>
    | (OutputSchema extends AnySchema ? InferSchema<OutputSchema> : unknown)
    | Promise<OutputSchema extends AnySchema ? InferSchema<OutputSchema> : unknown>;
}

export interface InvokeOptions {
  traceId?: string;
  spanId?: string;
  metadata?: Record<string, string>;
  timeoutMs?: number;
  signal?: AbortSignal;
  auth?: Partial<PrincipalContext>;
  bypassCache?: boolean;
}

export type LampAppState =
  | "created"
  | "starting"
  | "running"
  | "stopping"
  | "stopped";

export interface LampStartInfo {
  transport: TransportPolicy;
  serialization: SerializationPolicy;
  execution: ExecutionPolicy;
  operations: readonly string[];
  nativeBacked: boolean;
}

export interface RegisteredOperationMetadata {
  id: string;
  description?: string;
  execution: Required<Pick<OperationExecutionConfig, "mode" | "concurrency" | "timeoutMs">>;
  cache: {
    enabled: boolean;
    ttlSeconds: number;
  };
  auth: {
    allowAnonymous: boolean;
    roles: readonly string[];
    scopes: readonly string[];
    tenants: readonly string[];
  };
  tags: readonly string[];
}

export interface NativeHostBridge {
  registerOperation(
    operationId: string,
    descriptor: RegisteredOperationMetadata,
    nativeInvoker: (rawJsonInput: string, metadataJson: string) => unknown | Promise<unknown>
  ): void;
  markStarted?(configJson: string): void;
  markStopped?(): void;
}
