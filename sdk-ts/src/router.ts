import { LampError, type LampErrorHandler } from "./errors.ts";
import type {
  LampRegisterRouterOptions,
  LampRegistry,
  LampRegistryConfig,
  LampRegistryEntry,
  LampRouter,
  LampRouterConfig,
  LampRouterOptions,
  OperationAuthConfig,
  OperationDefinition,
  OperationExecutionConfig,
  ResolvedRouteOperation,
} from "./types.ts";

const ROUTER_SEGMENT_RE = /^[a-zA-Z0-9_-]+$/;
const DOTTED_PREFIX_RE = /^[a-zA-Z0-9_-]+(\.[a-zA-Z0-9_-]+)*$/;

function isValidOperationSuffix(suffix: string): boolean {
  return suffix.length > 0 && ROUTER_SEGMENT_RE.test(suffix);
}

function isValidTarget(target: string): boolean {
  return target.length > 0 && DOTTED_PREFIX_RE.test(target);
}

function toReadonlyArray<T>(value: T | readonly T[] | undefined): readonly T[] {
  if (value === undefined) {
    return [];
  }
  return Array.isArray(value) ? [...value] : [value as T];
}

function mergeExecution(
  base: OperationExecutionConfig | undefined,
  override: OperationExecutionConfig | undefined
): OperationExecutionConfig | undefined {
  if (!base && !override) {
    return undefined;
  }
  return {
    ...base,
    ...override,
  };
}

function mergeAuth(
  base: OperationAuthConfig | undefined,
  override: OperationAuthConfig | undefined
): OperationAuthConfig | undefined {
  if (!base && !override) {
    return undefined;
  }
  return {
    ...base,
    ...override,
  };
}

function mergeTags(
  base: readonly string[] | undefined,
  override: readonly string[] | undefined
): readonly string[] | undefined {
  if ((!base || base.length === 0) && (!override || override.length === 0)) {
    return undefined;
  }
  const seen = new Set<string>();
  for (const tag of base ?? []) {
    seen.add(tag);
  }
  for (const tag of override ?? []) {
    seen.add(tag);
  }
  return Array.from(seen);
}

function mergeRouterOptions(
  parent: LampRouterOptions,
  child: LampRouterOptions
): LampRouterOptions {
  return {
    execution: mergeExecution(parent.execution, child.execution),
    cache: child.cache !== undefined ? child.cache : parent.cache,
    auth: mergeAuth(parent.auth, child.auth),
    tags: mergeTags(parent.tags, child.tags),
    onError: child.onError ?? parent.onError,
  };
}

function mergeOperationWithOptions(
  operationId: string,
  definition: OperationDefinition<any, any>,
  routerOptions: LampRouterOptions,
  registryErrorHandler?: LampErrorHandler
): OperationDefinition<any, any> {
  const effectiveOnError = registryErrorHandler ?? routerOptions.onError;
  const wrappedHandler = effectiveOnError
    ? async (args: Parameters<OperationDefinition<any, any>["handler"]>[0]) => {
        try {
          return await definition.handler(args);
        } catch (err) {
          throw await effectiveOnError(err, {
            operationId,
            requestId: args.ctx.requestId,
          });
        }
      }
    : definition.handler;

  return {
    ...definition,
    execution: mergeExecution(routerOptions.execution, definition.execution),
    cache: definition.cache !== undefined ? definition.cache : routerOptions.cache,
    auth: mergeAuth(routerOptions.auth, definition.auth),
    tags: mergeTags(routerOptions.tags, definition.tags),
    handler: wrappedHandler,
  };
}

/**
 * Creates a business-logic {@link LampRegistry} that collects operations keyed by relative
 * operation suffix (for example `"get"`, `"create"`, `"update"`).
 *
 * Registries are mounted into a {@link LampRouter} via `createLampRouter({ registry: [...] })`.
 *
 * @param config - Optional registry configuration (e.g. `onError`).
 * @returns A new {@link LampRegistry} instance.
 */
export function createLampRegistry(config: LampRegistryConfig = {}): LampRegistry {
  const entriesList: LampRegistryEntry[] = [];
  const seenSuffixes = new Set<string>();
  let registryErrorHandler: LampErrorHandler | undefined = config.onError;

  const registry: LampRegistry = {
    add(operationSuffix, definition) {
      if (typeof operationSuffix !== "string" || !isValidOperationSuffix(operationSuffix)) {
        throw new LampError(
          `Invalid operation suffix "${String(operationSuffix)}". Registry operations must use a single-segment suffix such as "get" rather than a full dotted path.`,
          {
            code: "INVALID_ARGUMENT",
            identifier: "INVALID_OPERATION_SUFFIX",
          }
        );
      }
      if (seenSuffixes.has(operationSuffix)) {
        throw new LampError(
          `Duplicate operation suffix "${operationSuffix}" in registry`,
          {
            code: "ALREADY_EXISTS",
            identifier: "DUPLICATE_OPERATION",
          }
        );
      }
      if (!definition || typeof definition.handler !== "function") {
        throw new LampError(
          `Operation "${operationSuffix}" requires a handler function`,
          {
            code: "INVALID_ARGUMENT",
            identifier: "MISSING_HANDLER",
          }
        );
      }

      seenSuffixes.add(operationSuffix);
      entriesList.push({ suffix: operationSuffix, definition });
      return registry;
    },

    onError(handler) {
      registryErrorHandler = handler;
      return registry;
    },

    errorHandler() {
      return registryErrorHandler;
    },

    entries() {
      return [...entriesList];
    },
  };

  return registry;
}

/**
 * Creates a route-layer {@link LampRouter} that groups one or more {@link LampRegistry} modules
 * and optional nested {@link LampRouter} trees under a relative `target` prefix.
 *
 * @param config - Router module configuration (`target`, `description`, `registry`, `nesting`, `options`).
 * @returns A configured {@link LampRouter} ready to be nested or mounted via `app.registerRouter()`.
 */
export function createLampRouter(config: LampRouterConfig): LampRouter {
  if (!config || typeof config.target !== "string" || !isValidTarget(config.target)) {
    throw new LampError(
      `Invalid router target "${String(config?.target)}". Target must be a non-empty identifier such as "users".`,
      {
        code: "INVALID_ARGUMENT",
        identifier: "INVALID_ROUTER_TARGET",
      }
    );
  }

  const registries = toReadonlyArray(config.registry);
  const nested = toReadonlyArray(config.nesting);
  const routerOptions: LampRouterOptions = {
    execution: config.options?.execution ? { ...config.options.execution } : undefined,
    cache: config.options?.cache,
    auth: config.options?.auth ? { ...config.options.auth } : undefined,
    tags: config.options?.tags ? [...config.options.tags] : undefined,
    onError: config.onError ?? config.options?.onError,
  };

  const router: LampRouter = {
    description: config.description,
    target: config.target,
    options: routerOptions,
    registries,
    nested,

    resolveOperations(mountOptions: LampRegisterRouterOptions = {}): readonly ResolvedRouteOperation[] {
      if (mountOptions.prefix !== undefined && !isValidTarget(mountOptions.prefix)) {
        throw new LampError(
          `Invalid router mount prefix "${mountOptions.prefix}"`,
          {
            code: "INVALID_ARGUMENT",
            identifier: "INVALID_ROUTER_PREFIX",
          }
        );
      }

      const resolved: ResolvedRouteOperation[] = [];
      const seenOperationIds = new Set<string>();
      const activeStack = new Set<LampRouter>();

      function walk(
        current: LampRouter,
        parentPath: string,
        inheritedOptions: LampRouterOptions
      ): void {
        if (activeStack.has(current)) {
          throw new LampError(
            `Circular router nesting detected at target "${current.target}"`,
            {
              code: "INVALID_ARGUMENT",
              identifier: "CIRCULAR_ROUTER_NESTING",
            }
          );
        }

        activeStack.add(current);
        try {
          const currentPath = parentPath ? `${parentPath}.${current.target}` : current.target;
          const effectiveOptions = mergeRouterOptions(inheritedOptions, current.options);

          for (const reg of current.registries) {
            for (const entry of reg.entries()) {
              const operationId = `${currentPath}.${entry.suffix}`;
              if (seenOperationIds.has(operationId)) {
                throw new LampError(
                  `Duplicate operation "${operationId}" resolved within router "${router.target}"`,
                  {
                    code: "ALREADY_EXISTS",
                    identifier: "DUPLICATE_OPERATION",
                  }
                );
              }
              seenOperationIds.add(operationId);
              resolved.push({
                operationId,
                definition: mergeOperationWithOptions(
                  operationId,
                  entry.definition,
                  effectiveOptions,
                  reg.errorHandler()
                ),
              });
            }
          }

          for (const child of current.nested) {
            walk(child, currentPath, effectiveOptions);
          }
        } finally {
          activeStack.delete(current);
        }
      }

      const baseOptions: LampRouterOptions = {
        execution: mountOptions.execution,
        cache: mountOptions.cache,
        auth: mountOptions.auth,
        tags: mountOptions.tags,
        onError: mountOptions.onError,
      };

      walk(router, mountOptions.prefix ?? "", baseOptions);
      return resolved;
    },
  };

  return router;
}
