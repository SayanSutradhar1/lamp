import { LampError, normalizeLampError } from "./errors.ts";
import { getNativeHostBridge } from "./native_bridge.ts";
import { validateWithSchema } from "./schema.ts";
import type {
  InvokeCallOptions,
  LampAllowOriginConfig,
  LampApp,
  LampAppConfig,
  LampAppState,
  LampFile,
  LampOriginPolicy,
  LampParallelTaskSpec,
  LampStartInfo,
  LifecycleHook,
  OperationContext,
  OperationDefinition,
  OriginConfiguredCallback,
  OriginExposeFn,
  RegisteredOperationMetadata,
} from "./types.ts";

const OPERATION_ID_RE = /^[a-zA-Z0-9_-]+(\.[a-zA-Z0-9_-]+)*$/;
const DEFAULT_MAX_TTL_LIMIT_SECONDS = 300;

interface StoredOperation {
  readonly meta: RegisteredOperationMetadata;
  readonly definition: OperationDefinition<any, any>;
}

/**
 * Validates whether a string follows Lamp's canonical dotted operation identifier syntax.
 *
 * Lamp replaces HTTP URL routes with logical dotted IDs such as `"users.create"` or
 * `"billing.invoices.generate"`.
 *
 * @param id - Candidate operation name.
 * @returns `true` if valid and non-empty.
 */
export function isValidOperationId(id: string): boolean {
  return id.length > 0 && OPERATION_ID_RE.test(id);
}

function hydrateFileReferences(
  val: unknown,
  files: readonly LampFile[],
  depth = 0
): unknown {
  if (
    depth > 32 ||
    files.length === 0 ||
    val === null ||
    typeof val !== "object" ||
    val instanceof Uint8Array ||
    ArrayBuffer.isView(val)
  ) {
    return val;
  }
  if (Array.isArray(val)) {
    return val.map((item) => hydrateFileReferences(item, files, depth + 1));
  }
  const rec = val as Record<string, unknown>;
  if (typeof rec.__lampFileRef === "number" && Number.isInteger(rec.__lampFileRef)) {
    return files[rec.__lampFileRef] ?? val;
  }
  const out: Record<string, unknown> = {};
  for (const [k, v] of Object.entries(rec)) {
    out[k] = hydrateFileReferences(v, files, depth + 1);
  }
  return out;
}

async function runParallelTasks(
  itemsOrTasks: readonly unknown[],
  mapperFn?: (item: any) => any
): Promise<unknown[]> {
  let specs: Array<{ fn: string; input: unknown }>;
  if (typeof mapperFn === "function") {
    const fnStr = mapperFn.toString();
    specs = itemsOrTasks.map((item) => ({ fn: fnStr, input: item }));
  } else {
    specs = itemsOrTasks.map((entry) => {
      if (typeof entry === "function") {
        return { fn: entry.toString(), input: null };
      }
      if (entry && typeof entry === "object" && "fn" in entry) {
        const spec = entry as LampParallelTaskSpec<unknown, unknown>;
        return {
          fn: typeof spec.fn === "function" ? spec.fn.toString() : String(spec.fn),
          input: spec.input,
        };
      }
      throw new LampError("ctx.parallel requires task specs or (items, mapper)", {
        code: "INVALID_ARGUMENT",
        identifier: "PARALLEL_INVALID_ARGS",
      });
    });
  }

  if (specs.length === 0) {
    return [];
  }

  const nativeBridge = getNativeHostBridge();
  if (nativeBridge && typeof nativeBridge.runParallel === "function") {
    const nativePayloads = specs.map((sp) => ({
      fn: sp.fn,
      inputJson: JSON.stringify(sp.input ?? null),
    }));
    const rawResults = nativeBridge.runParallel(nativePayloads);
    return rawResults.map((raw) => (raw.length > 0 ? JSON.parse(raw) : null));
  }

  return Promise.all(
    specs.map(async (sp) => {
      const compiled = new Function(`return (${sp.fn})`)() as (arg: unknown) => unknown;
      return compiled(sp.input);
    })
  );
}

function normalizeOriginString(raw: string): string {
  const trimmed = raw.trim();
  if (trimmed.length === 0) {
    return "";
  }
  if (trimmed === "*") {
    return "*";
  }
  return trimmed.replace(/\/+$/, "").toLowerCase();
}

function resolveOriginPolicy(allowOrigin: LampAllowOriginConfig | undefined): LampOriginPolicy {
  if (allowOrigin === true) {
    return { mode: "allow_all", origins: [] };
  }
  if (typeof allowOrigin === "function") {
    let configuredOrigins: string[] = [];
    const callback: OriginConfiguredCallback = () => {};
    const expose: OriginExposeFn = (origins) => {
      if (!Array.isArray(origins)) {
        throw new LampError("allowOrigin expose() requires an array of origin strings", {
          code: "INVALID_ARGUMENT",
          identifier: "INVALID_ORIGIN_CONFIG",
        });
      }
      configuredOrigins = origins
        .map((entry) => (typeof entry === "string" ? normalizeOriginString(entry) : ""))
        .filter((entry) => entry.length > 0);
      callback(configuredOrigins);
    };
    const allowed = Boolean(allowOrigin(expose, callback));
    if (allowed) {
      return { mode: "allow_list", origins: configuredOrigins };
    }
    return { mode: "deny_all", origins: [] };
  }
  return { mode: "deny_all", origins: [] };
}

/**
 * Creates a new server-side {@link LampApp} operation registry and lifecycle controller.
 *
 * All network I/O, framing (`LMP1`/`LCB1`), dynamic thread scaling, and full-cycle response
 * caching are executed exclusively by the native C++ runtime host (`lamp_server`).
 *
 * @param config - Server application configuration (caching defaults, TTL upper limit, workers, security).
 * @returns Configured {@link LampApp} contract ready for `.register()` and `.start()`.
 */
export function createLampApp(config: LampAppConfig = {}): LampApp {
  let currentState: LampAppState = "created";
  let nextRequestId = 1;
  const operationsMap = new Map<string, StoredOperation>();
  const startHooks: LifecycleHook[] = [];
  const stopHooks: LifecycleHook[] = [];

  const appCacheEnabled =
    typeof config.cache === "boolean"
      ? config.cache
      : Boolean(
          config.cache && (config.cache.enabled ?? config.cache.mode !== "disabled")
        );

  const maxTtlLimitSeconds =
    typeof config.cache === "object" &&
    config.cache &&
    typeof config.cache.maxTtlLimitSeconds === "number" &&
    config.cache.maxTtlLimitSeconds > 0
      ? config.cache.maxTtlLimitSeconds
      : DEFAULT_MAX_TTL_LIMIT_SECONDS;

  const appDefaultTtl =
    typeof config.cache === "object" &&
    config.cache &&
    typeof config.cache.defaultTtl === "number" &&
    config.cache.defaultTtl > 0
      ? Math.min(config.cache.defaultTtl, maxTtlLimitSeconds)
      : Math.min(60, maxTtlLimitSeconds);

  const logEnabled = config.log ?? true;
  const resolvedOriginPolicy = resolveOriginPolicy(config.allowOrigin);
  getNativeHostBridge()?.configureOriginAllowance?.(JSON.stringify(resolvedOriginPolicy));

  async function executeStoredOperation<TOutput>(
    entry: StoredOperation,
    rawInput: unknown,
    options: InvokeCallOptions = {}
  ): Promise<TOutput> {
    const controller = new AbortController();
    const externalSignal = options.signal;
    const onAbort = () => controller.abort(externalSignal?.reason);

    if (externalSignal) {
      if (externalSignal.aborted) {
        controller.abort(externalSignal.reason);
      } else {
        externalSignal.addEventListener("abort", onAbort, { once: true });
      }
    }

    try {
      if (controller.signal.aborted) {
        throw new LampError("Operation aborted before execution", {
          code: "CANCELLED",
          identifier: "REQUEST_ABORTED",
        });
      }

      const files = options.files ?? [];
      const hydrated = hydrateFileReferences(rawInput, files);
      const normalizedInput =
        hydrated === null && entry.definition.input?.kind === "optional"
          ? undefined
          : hydrated;
      const validatedInput = validateWithSchema(
        entry.definition.input,
        normalizedInput,
        "input"
      );

      const meta = options.metadata ?? {};
      const rolesRaw = meta["lamp.auth.roles"] ?? "";
      const scopesRaw = meta["lamp.auth.scopes"] ?? "";

      const ctx: OperationContext = {
        requestId: nextRequestId++,
        operationId: entry.meta.id,
        metadata: meta,
        files,
        signal: controller.signal,
        parallel: runParallelTasks as OperationContext["parallel"],
        auth: {
          authenticated: meta["lamp.auth.authenticated"] === "true",
          subject: meta["lamp.auth.subject"],
          tenant: meta["lamp.auth.tenant"],
          scheme: meta["lamp.auth.scheme"],
          roles: rolesRaw ? rolesRaw.split(",").filter(Boolean) : [],
          scopes: scopesRaw ? scopesRaw.split(",").filter(Boolean) : [],
        },
      };

      if (logEnabled) {
        console.log(`[lamp] -> ${entry.meta.id} (#${ctx.requestId})`);
      }

      const rawOutput = await entry.definition.handler({
        input: validatedInput,
        ctx,
        signal: controller.signal,
      });

      return validateWithSchema(
        entry.definition.output,
        rawOutput,
        "output"
      ) as TOutput;
    } catch (err) {
      throw normalizeLampError(err);
    } finally {
      if (externalSignal) {
        externalSignal.removeEventListener("abort", onAbort);
      }
    }
  }

  const app: LampApp = {
    register(operationId, definition) {
      if (currentState !== "created") {
        throw new LampError(
          `Cannot register "${operationId}" while application state is "${currentState}"`,
          { code: "FAILED_PRECONDITION", identifier: "APP_ALREADY_STARTED" }
        );
      }
      if (!isValidOperationId(operationId)) {
        throw new LampError(`Invalid operation ID "${operationId}"`, {
          code: "INVALID_ARGUMENT",
          identifier: "INVALID_OPERATION_ID",
        });
      }
      if (operationsMap.has(operationId)) {
        throw new LampError(`Duplicate operation "${operationId}"`, {
          code: "ALREADY_EXISTS",
          identifier: "DUPLICATE_OPERATION",
        });
      }
      if (!definition || typeof definition.handler !== "function") {
        throw new LampError(`Operation "${operationId}" requires a handler function`, {
          code: "INVALID_ARGUMENT",
          identifier: "MISSING_HANDLER",
        });
      }

      const rawTtl =
        typeof definition.cache === "object" && definition.cache !== null
          ? definition.cache.ttl
          : definition.cache === true
            ? appDefaultTtl
            : 0;

      const cacheEnabled =
        definition.cache === false
          ? false
          : typeof definition.cache === "object"
            ? (definition.cache.enabled ?? true) && rawTtl > 0
            : definition.cache === true && appCacheEnabled && rawTtl > 0;

      const clampedTtl = cacheEnabled ? Math.min(rawTtl, maxTtlLimitSeconds) : 0;

      const meta: RegisteredOperationMetadata = {
        id: operationId,
        description: definition.description,
        execution: {
          mode: definition.execution?.mode ?? config.execution ?? "parallel-dynamic",
          concurrency: definition.execution?.concurrency ?? "concurrent",
          timeoutMs: definition.execution?.timeoutMs ?? 30000,
        },
        cache: {
          enabled: cacheEnabled,
          ttlSeconds: clampedTtl,
          maxTtlLimitSeconds,
        },
        auth: {
          allowAnonymous:
            definition.auth?.allowAnonymous ?? !(config.security?.defaultDeny ?? false),
          roles: [...(definition.auth?.roles ?? [])],
          scopes: [...(definition.auth?.scopes ?? [])],
          tenants: [...(definition.auth?.tenants ?? [])],
        },
        tags: [...(definition.tags ?? [])],
      };

      const stored: StoredOperation = { meta, definition };
      operationsMap.set(operationId, stored);

      if (logEnabled) {
        console.log(`[lamp] + registered ${operationId}`);
      }

      const bridge = getNativeHostBridge();
      if (bridge && typeof bridge.registerOperation === "function") {
        bridge.registerOperation(
          operationId,
          meta,
          (rawInputJson, metadataJson) => {
            const parsedInput =
              rawInputJson.length > 0 ? JSON.parse(rawInputJson) : undefined;
            const parsedMeta =
              metadataJson.length > 0
                ? (JSON.parse(metadataJson) as Record<string, string>)
                : {};
            return executeStoredOperation(stored, parsedInput, {
              metadata: parsedMeta,
            });
          }
        );
      }

      return app;
    },

    registerRouter(router, options) {
      if (currentState !== "created") {
        throw new LampError(
          `Cannot register router "${router?.target ?? "unknown"}" while application state is "${currentState}"`,
          { code: "FAILED_PRECONDITION", identifier: "APP_ALREADY_STARTED" }
        );
      }
      if (!router || typeof router.resolveOperations !== "function") {
        throw new LampError("registerRouter requires a valid LampRouter instance", {
          code: "INVALID_ARGUMENT",
          identifier: "INVALID_ROUTER",
        });
      }

      const resolvedOps = router.resolveOperations(options);
      for (const op of resolvedOps) {
        app.register(op.operationId, op.definition);
      }

      return app;
    },

    async start(): Promise<LampStartInfo> {
      if (currentState === "running") {
        return {
          transport: config.transport ?? "lmp1-binary",
          serialization: config.serialization ?? "lcb1",
          execution: config.execution ?? "parallel-dynamic",
          operations: Array.from(operationsMap.keys()),
        };
      }
      if (currentState === "stopped") {
        throw new LampError("Cannot restart an already stopped Lamp app", {
          code: "FAILED_PRECONDITION",
          identifier: "APP_ALREADY_STOPPED",
        });
      }

      for (const hook of startHooks) {
        await hook();
      }
      currentState = "running";

      const info: LampStartInfo = {
        transport: config.transport ?? "lmp1-binary",
        serialization: config.serialization ?? "lcb1",
        execution: config.execution ?? "parallel-dynamic",
        operations: Array.from(operationsMap.keys()),
      };

      getNativeHostBridge()?.markStarted?.(JSON.stringify(info));
      return info;
    },

    async invoke<TOutput = unknown>(
      operationId: string,
      input?: unknown,
      options?: InvokeCallOptions
    ): Promise<TOutput> {
      if (currentState !== "running") {
        throw new LampError(
          `Cannot invoke "${operationId}" when application state is "${currentState}"`,
          { code: "FAILED_PRECONDITION", identifier: "APP_NOT_RUNNING" }
        );
      }
      const entry = operationsMap.get(operationId);
      if (!entry) {
        throw new LampError(`Operation "${operationId}" is not registered`, {
          code: "NOT_FOUND",
          identifier: "OPERATION_NOT_FOUND",
        });
      }
      return executeStoredOperation<TOutput>(entry, input, options);
    },

    async stop(): Promise<void> {
      if (currentState === "stopped") {
        return;
      }
      if (currentState === "created") {
        currentState = "stopped";
        return;
      }
      for (let i = stopHooks.length - 1; i >= 0; i -= 1) {
        await stopHooks[i]();
      }
      currentState = "stopped";
      getNativeHostBridge()?.markStopped?.();
    },

    state() {
      return currentState;
    },

    operations() {
      return Array.from(operationsMap.values(), (entry) => entry.meta);
    },

    originPolicy() {
      return {
        mode: resolvedOriginPolicy.mode,
        origins: [...resolvedOriginPolicy.origins],
      };
    },

    isOriginAllowed(origin) {
      if (resolvedOriginPolicy.mode === "allow_all") {
        return true;
      }
      if (resolvedOriginPolicy.mode === "deny_all") {
        return false;
      }
      const normalized = normalizeOriginString(origin);
      if (normalized.length === 0) {
        return false;
      }
      return resolvedOriginPolicy.origins.some(
        (allowed) => allowed === "*" || (normalized !== "null" && allowed === normalized)
      );
    },

    onStart(hook) {
      startHooks.push(hook);
      return app;
    },

    onStop(hook) {
      stopHooks.push(hook);
      return app;
    },
  };

  return app;
}
