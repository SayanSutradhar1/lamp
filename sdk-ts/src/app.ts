// ============================================================
// Lamp TypeScript SDK - Application Core (`createLampApp`)
// Matches docs/INTERFACE.md canonical design target.
// ============================================================

import { LampError, normalizeLampError } from "./errors.ts";
import { resolveNativeHostBridge } from "./native_bridge.ts";
import { type AnySchema, validateSchema } from "./schema.ts";
import type {
  AppCacheConfig,
  AppSecurityConfig,
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
  PrincipalContext,
  RegisteredOperationMetadata,
  SerializationPolicy,
  TransportPolicy,
} from "./types.ts";

// Mirrors runtime/bridge/operation_descriptor.hpp OperationDescriptor::IsValidId
export function isValidOperationId(value: string): boolean {
  if (value.length === 0 || value.length > 128) {
    return false;
  }
  let previousSeparator = true;
  for (let i = 0; i < value.length; i += 1) {
    const ch = value.charCodeAt(i);
    const isAlpha = (ch >= 65 && ch <= 90) || (ch >= 97 && ch <= 122);
    const isDigit = ch >= 48 && ch <= 57;
    const isSep = ch === 46 || ch === 45 || ch === 95; // '.' '-' '_'
    if (!isAlpha && !isDigit && !isSep) {
      return false;
    }
    if (isSep) {
      if (previousSeparator) {
        return false;
      }
      previousSeparator = true;
    } else {
      previousSeparator = false;
    }
  }
  return !previousSeparator;
}

interface InternalOperationEntry {
  metadata: RegisteredOperationMetadata;
  inputSchema?: AnySchema;
  outputSchema?: AnySchema;
  handler: (args: {
    input: unknown;
    ctx: OperationContext;
    signal: AbortSignal;
  }) => unknown | Promise<unknown>;
  cache: Map<string, { value: unknown; expiresAtMs: number }>;
}

export type LifecycleHook = () => void | Promise<void>;

export interface LampApp {
  register<
    InputSchema extends AnySchema | undefined = undefined,
    OutputSchema extends AnySchema | undefined = undefined,
  >(
    operationId: string,
    definition: OperationDefinition<InputSchema, OutputSchema>
  ): LampApp;

  invoke<Output = unknown, Input = unknown>(
    operationId: string,
    input?: Input,
    options?: InvokeOptions
  ): Promise<Output>;

  start(): Promise<LampStartInfo>;
  stop(): Promise<void>;
  state(): LampAppState;
  operations(): readonly RegisteredOperationMetadata[];
  onStart(hook: LifecycleHook): LampApp;
  onStop(hook: LifecycleHook): LampApp;
}

function normalizeCacheConfig(
  cache: LampAppConfig["cache"]
): Required<Pick<AppCacheConfig, "enabled" | "mode" | "defaultTtl" | "staleWhileRevalidate">> {
  if (typeof cache === "boolean") {
    return {
      enabled: cache,
      mode: cache ? "local" : "disabled",
      defaultTtl: 60,
      staleWhileRevalidate: false,
    };
  }
  const enabled = cache?.enabled ?? (cache?.mode !== undefined && cache.mode !== "disabled");
  return {
    enabled,
    mode: enabled ? (cache?.mode ?? "local") : "disabled",
    defaultTtl: cache?.defaultTtl ?? 60,
    staleWhileRevalidate: cache?.staleWhileRevalidate ?? false,
  };
}

function normalizeOperationCache(
  opCache: boolean | OperationCacheConfig | undefined,
  appCache: ReturnType<typeof normalizeCacheConfig>
): { enabled: boolean; ttlSeconds: number } {
  if (opCache === false) {
    return { enabled: false, ttlSeconds: 0 };
  }
  if (opCache === true) {
    return {
      enabled: appCache.enabled,
      ttlSeconds: appCache.enabled ? appCache.defaultTtl : 0,
    };
  }
  if (opCache === undefined) {
    return { enabled: false, ttlSeconds: 0 };
  }
  const explicitlyDisabled = opCache.enabled === false || opCache.mode === "disabled";
  if (explicitlyDisabled) {
    return { enabled: false, ttlSeconds: 0 };
  }
  const ttl = opCache.ttl ?? appCache.defaultTtl;
  if (ttl <= 0) {
    return { enabled: false, ttlSeconds: 0 };
  }
  return {
    enabled: appCache.enabled || opCache.enabled === true || opCache.ttl !== undefined,
    ttlSeconds: ttl,
  };
}

function normalizeOperationExecution(
  opExec: OperationExecutionConfig | undefined,
  defaultMode: ExecutionPolicy
): Required<Pick<OperationExecutionConfig, "mode" | "concurrency" | "timeoutMs">> {
  return {
    mode: opExec?.mode ?? defaultMode,
    concurrency: opExec?.concurrency ?? "concurrent",
    timeoutMs: opExec?.timeoutMs ?? 30_000,
  };
}

function normalizeOperationAuth(
  opAuth: OperationAuthConfig | undefined,
  securityConfig: AppSecurityConfig
): RegisteredOperationMetadata["auth"] {
  const defaultDeny = securityConfig.defaultDeny ?? false;
  return {
    allowAnonymous: opAuth?.allowAnonymous ?? !defaultDeny,
    roles: [...(opAuth?.roles ?? [])],
    scopes: [...(opAuth?.scopes ?? [])],
    tenants: [...(opAuth?.tenants ?? [])],
  };
}

function stableCacheKey(operationId: string, input: unknown): string {
  return `${operationId}:${JSON.stringify(input ?? null)}`;
}

function enforceAuth(
  policy: RegisteredOperationMetadata["auth"],
  auth: PrincipalContext
): void {
  if (policy.allowAnonymous) {
    return;
  }
  if (!auth.authenticated) {
    throw new LampError("Authentication required for operation", {
      code: "UNAUTHENTICATED",
      identifier: "UNAUTHENTICATED",
    });
  }
  if (policy.tenants.length > 0 && (!auth.tenant || !policy.tenants.includes(auth.tenant))) {
    throw new LampError("Principal tenant is not authorized", {
      code: "PERMISSION_DENIED",
      identifier: "TENANT_FORBIDDEN",
    });
  }
  if (policy.roles.length > 0 && !policy.roles.some((r) => auth.roles.includes(r))) {
    throw new LampError("Principal lacks required role", {
      code: "PERMISSION_DENIED",
      identifier: "ROLE_FORBIDDEN",
    });
  }
  if (policy.scopes.length > 0 && !policy.scopes.every((s) => auth.scopes.includes(s))) {
    throw new LampError("Principal lacks required scope", {
      code: "PERMISSION_DENIED",
      identifier: "SCOPE_FORBIDDEN",
    });
  }
}

export function createLampApp(config: LampAppConfig = {}): LampApp {
  const transport: TransportPolicy = config.transport ?? "auto";
  const serialization: SerializationPolicy = config.serialization ?? "protobuf";
  const execution: ExecutionPolicy = config.execution ?? "adaptive";
  const appCache = normalizeCacheConfig(config.cache);
  const securityConfig: AppSecurityConfig = config.security ?? {};
  const nativeBridge: NativeHostBridge | undefined = resolveNativeHostBridge(
    config.nativeBridge
  );

  let currentState: LampAppState = "created";
  let nextRequestId = 1;
  const registry = new Map<string, InternalOperationEntry>();
  const startHooks: LifecycleHook[] = [];
  const stopHooks: LifecycleHook[] = [];

  async function executeEntry(
    entry: InternalOperationEntry,
    rawInput: unknown,
    options: InvokeOptions = {}
  ): Promise<unknown> {
    const auth: PrincipalContext = {
      authenticated: options.auth?.authenticated ?? false,
      subject: options.auth?.subject,
      tenant: options.auth?.tenant,
      scheme: options.auth?.scheme,
      roles: [...(options.auth?.roles ?? [])],
      scopes: [...(options.auth?.scopes ?? [])],
    };

    enforceAuth(entry.metadata.auth, auth);

    const validatedInput = validateSchema(entry.inputSchema, rawInput, "input");

    if (entry.metadata.cache.enabled && !options.bypassCache) {
      const key = stableCacheKey(entry.metadata.id, validatedInput);
      const cached = entry.cache.get(key);
      if (cached && cached.expiresAtMs > Date.now()) {
        return cached.value;
      }
      if (cached) {
        entry.cache.delete(key);
      }
    }

    const controller = new AbortController();
    const externalSignal = options.signal;
    const onExternalAbort = (): void => {
      controller.abort(new LampError("Operation was cancelled", {
        code: "CANCELLED",
        identifier: "OPERATION_CANCELLED",
      }));
    };

    if (externalSignal?.aborted) {
      throw new LampError("Operation was cancelled", {
        code: "CANCELLED",
        identifier: "OPERATION_CANCELLED",
      });
    }
    externalSignal?.addEventListener("abort", onExternalAbort, { once: true });

    const timeoutMs = options.timeoutMs ?? entry.metadata.execution.timeoutMs;
    let timer: ReturnType<typeof setTimeout> | undefined;
    const timeoutPromise =
      timeoutMs > 0
        ? new Promise<never>((_, reject) => {
            timer = setTimeout(() => {
              const err = new LampError("Operation deadline exceeded", {
                code: "DEADLINE_EXCEEDED",
                identifier: "DEADLINE_EXCEEDED",
              });
              controller.abort(err);
              reject(err);
            }, timeoutMs);
          })
        : undefined;

    const abortPromise = new Promise<never>((_, reject) => {
      controller.signal.addEventListener(
        "abort",
        () => {
          const reason = controller.signal.reason;
          if (reason instanceof LampError) {
            reject(reason);
          } else {
            reject(
              new LampError("Operation was cancelled", {
                code: "CANCELLED",
                identifier: "OPERATION_CANCELLED",
              })
            );
          }
        },
        { once: true }
      );
    });

    const ctx: OperationContext = {
      requestId: nextRequestId++,
      operationId: entry.metadata.id,
      traceId: options.traceId,
      spanId: options.spanId,
      metadata: Object.freeze({ ...(options.metadata ?? {}) }),
      signal: controller.signal,
      auth: Object.freeze(auth),
    };

    try {
      const runPromise = Promise.resolve().then(() =>
        entry.handler({
          input: validatedInput,
          ctx,
          signal: controller.signal,
        })
      );

      const racers: Promise<unknown>[] = [runPromise, abortPromise];
      if (timeoutPromise) {
        racers.push(timeoutPromise);
      }

      const rawOutput = await Promise.race(racers);
      const validatedOutput = validateSchema(entry.outputSchema, rawOutput, "output");

      if (entry.metadata.cache.enabled && !options.bypassCache) {
        const key = stableCacheKey(entry.metadata.id, validatedInput);
        entry.cache.set(key, {
          value: validatedOutput,
          expiresAtMs: Date.now() + entry.metadata.cache.ttlSeconds * 1000,
        });
      }

      return validatedOutput;
    } catch (err) {
      throw normalizeLampError(err);
    } finally {
      if (timer !== undefined) {
        clearTimeout(timer);
      }
      externalSignal?.removeEventListener("abort", onExternalAbort);
    }
  }

  const app: LampApp = {
    register(operationId, definition) {
      if (currentState !== "created" && currentState !== "stopped") {
        throw new LampError(
          `Cannot register operation "${operationId}" while app is ${currentState}`,
          {
            code: "FAILED_PRECONDITION",
            identifier: "APP_ALREADY_STARTED",
          }
        );
      }
      if (!isValidOperationId(operationId)) {
        throw new LampError(`Invalid Lamp operation identifier "${operationId}"`, {
          code: "INVALID_ARGUMENT",
          identifier: "INVALID_OPERATION_ID",
        });
      }
      if (registry.has(operationId)) {
        throw new LampError(`Duplicate operation registration "${operationId}"`, {
          code: "ALREADY_EXISTS",
          identifier: "DUPLICATE_OPERATION_ID",
        });
      }
      if (typeof definition?.handler !== "function") {
        throw new LampError(`Operation "${operationId}" requires a handler function`, {
          code: "INVALID_ARGUMENT",
          identifier: "MISSING_HANDLER",
        });
      }

      const metadata: RegisteredOperationMetadata = {
        id: operationId,
        description: definition.description,
        execution: normalizeOperationExecution(definition.execution, execution),
        cache: normalizeOperationCache(definition.cache, appCache),
        auth: normalizeOperationAuth(definition.auth, securityConfig),
        tags: [...(definition.tags ?? [])],
      };

      const entry: InternalOperationEntry = {
        metadata,
        inputSchema: definition.input,
        outputSchema: definition.output,
        handler: definition.handler as InternalOperationEntry["handler"],
        cache: new Map(),
      };

      registry.set(operationId, entry);

      if (nativeBridge) {
        nativeBridge.registerOperation(
          operationId,
          metadata,
          (rawJsonInput: string, metadataJson: string) => {
            const parsedInput =
              rawJsonInput && rawJsonInput.length > 0 ? JSON.parse(rawJsonInput) : undefined;
            const parsedMeta: Record<string, string> =
              metadataJson && metadataJson.length > 0
                ? (JSON.parse(metadataJson) as Record<string, string>)
                : {};
            const roles = parsedMeta["lamp.auth.roles"]
              ? parsedMeta["lamp.auth.roles"].split(",").filter(Boolean)
              : [];
            const scopes = parsedMeta["lamp.auth.scopes"]
              ? parsedMeta["lamp.auth.scopes"].split(",").filter(Boolean)
              : [];
            return executeEntry(entry, parsedInput, {
              metadata: parsedMeta,
              auth: {
                authenticated: parsedMeta["lamp.auth.authenticated"] === "true",
                subject: parsedMeta["lamp.auth.subject"],
                tenant: parsedMeta["lamp.auth.tenant"],
                scheme: parsedMeta["lamp.auth.scheme"],
                roles,
                scopes,
              },
            });
          }
        );
      }

      return app;
    },

    async invoke<Output = unknown, Input = unknown>(
      operationId: string,
      input?: Input,
      options?: InvokeOptions
    ): Promise<Output> {
      if (currentState !== "running") {
        throw new LampError(
          `Cannot invoke "${operationId}" when application state is "${currentState}"`,
          {
            code: "FAILED_PRECONDITION",
            identifier: "APP_NOT_RUNNING",
          }
        );
      }
      const entry = registry.get(operationId);
      if (!entry) {
        throw new LampError(`Operation "${operationId}" is not registered`, {
          code: "NOT_FOUND",
          identifier: "OPERATION_NOT_FOUND",
        });
      }
      const result = await executeEntry(entry, input, options);
      return result as Output;
    },

    async start(): Promise<LampStartInfo> {
      if (currentState === "running") {
        return {
          transport,
          serialization,
          execution,
          operations: Array.from(registry.keys()),
          nativeBacked: nativeBridge !== undefined,
        };
      }
      if (currentState !== "created" && currentState !== "stopped") {
        throw new LampError(`Cannot start app from state "${currentState}"`, {
          code: "FAILED_PRECONDITION",
          identifier: "INVALID_LIFECYCLE_STATE",
        });
      }
      currentState = "starting";
      try {
        for (const hook of startHooks) {
          await hook();
        }
        nativeBridge?.markStarted?.(
          JSON.stringify({
            transport,
            serialization,
            execution,
            cache: appCache,
          })
        );
        currentState = "running";
        return {
          transport,
          serialization,
          execution,
          operations: Array.from(registry.keys()),
          nativeBacked: nativeBridge !== undefined,
        };
      } catch (err) {
        currentState = "stopped";
        throw normalizeLampError(err);
      }
    },

    async stop(): Promise<void> {
      if (currentState === "stopped" || currentState === "created") {
        currentState = "stopped";
        return;
      }
      currentState = "stopping";
      try {
        for (const entry of registry.values()) {
          entry.cache.clear();
        }
        for (let i = stopHooks.length - 1; i >= 0; i -= 1) {
          await stopHooks[i]();
        }
        nativeBridge?.markStopped?.();
      } finally {
        currentState = "stopped";
      }
    },

    state(): LampAppState {
      return currentState;
    },

    operations(): readonly RegisteredOperationMetadata[] {
      return Array.from(registry.values(), (e) => e.metadata);
    },

    onStart(hook: LifecycleHook): LampApp {
      startHooks.push(hook);
      return app;
    },

    onStop(hook: LifecycleHook): LampApp {
      stopHooks.push(hook);
      return app;
    },
  };

  return app;
}
