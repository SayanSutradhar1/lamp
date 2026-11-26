#pragma once

// ============================================================
// Lamp Framework
// Host Runtime - Thread-Affine V8 Scheduler, Dynamic Parallelism
// & Compact Binary Bridge (`LampV8RuntimeScheduler`)
//
// File: host/lamp_v8_scheduler.hpp
//
// Purpose:
//   Owns the primary V8 isolate for stateful application scripts,
//   installs __lampNativeBridge + embedded server-side SDK
//   contracts (createLampApp, s including s.file(), LampError,
//   ctx.parallel backed by core::DynamicWorkerPool), converts
//   LCB1 DecodedRpcInvocation <-> V8 values directly (with raw
//   Uint8Array binary files, zero base64), and supports true
//   parallel subtask execution across dynamic C++ worker threads.
// ============================================================

#include "bridge/execution_dispatcher.hpp"
#include "bridge/operation_registry.hpp"
#include "bridge/request_envelope.hpp"
#include "bridge/result_envelope.hpp"
#include "bridge/v8_sdk_host_bridge.hpp"
#include "cache/local_operation_cache.hpp"
#include "core/dynamic_worker_pool.hpp"
#include "core/origin_policy.hpp"
#include "execution/async_execution_contract.hpp"
#include "serialization/serialization_value.hpp"
#include "telemetry/telemetry_dispatcher.hpp"
#include "transport/lamp_binary_protocol.hpp"
#include "transport/lamp_binary_server.hpp"
#include "v8/v8_platform.hpp"

#include <v8.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace lamp::host {

inline constexpr std::string_view kEmbeddedLampSdkBootstrapJs = R"JS(
(() => {
  class LampError extends Error {
    constructor(message, options = {}) {
      super(message);
      this.name = "LampError";
      this.code = options.code ?? "INTERNAL";
      this.identifier = options.identifier ?? this.code;
      this.retryable =
        options.retryable ??
        (this.code === "UNAVAILABLE" || this.code === "RESOURCE_EXHAUSTED");
      this.details = options.details;
    }
    toJSON() {
      return {
        error: true,
        code: this.code,
        identifier: this.identifier,
        message: this.message,
        retryable: this.retryable,
      };
    }
  }

  function isLampError(value) {
    return value instanceof LampError;
  }

  function normalizeLampError(err) {
    if (err instanceof LampError) return err;
    if (err && typeof err === "object" && typeof err.code === "string") {
      return new LampError(
        typeof err.message === "string" ? err.message : "Operation failed",
        {
          code: err.code,
          identifier: typeof err.identifier === "string" ? err.identifier : err.code,
          retryable: Boolean(err.retryable),
        }
      );
    }
    const msg = err instanceof Error && err.message ? err.message : "Operation execution failed";
    return new LampError(msg, {
      code: "EXECUTION_FAILED",
      identifier: "INTERNAL_EXECUTION_ERROR",
    });
  }

  function makeSchema(kind, safeParseFn) {
    return {
      kind,
      safeParse(value) {
        return safeParseFn(value, "$");
      },
      parse(value) {
        const res = safeParseFn(value, "$");
        if (res.ok) return res.value;
        const first = res.issues[0];
        const msg = first ? `${first.path}: ${first.message}` : "Schema validation failed";
        throw new LampError(msg, {
          code: "INVALID_ARGUMENT",
          identifier: "SCHEMA_VALIDATION_FAILED",
          details: { issues: res.issues },
        });
      },
    };
  }

  function safeParseInternal(schema, value, path) {
    if (schema && typeof schema.safeParse === "function") {
      const res = schema.safeParse(value);
      if (res.ok) return res;
      return {
        ok: false,
        issues: res.issues.map((i) => ({
          path: i.path === "$" ? path : `${path}.${i.path.replace(/^\$\.?/, "")}`,
          message: i.message,
        })),
      };
    }
    try {
      return { ok: true, value: schema.parse(value) };
    } catch (err) {
      return {
        ok: false,
        issues: [{ path, message: err instanceof Error ? err.message : "Validation failed" }],
      };
    }
  }

  function isBinaryBuffer(v) {
    return v instanceof Uint8Array || ArrayBuffer.isView(v);
  }

  const s = {
    string(options = {}) {
      return makeSchema("string", (value, path) => {
        if (typeof value !== "string") {
          return { ok: false, issues: [{ path, message: "Expected string" }] };
        }
        if (options.minLength !== undefined && value.length < options.minLength) {
          return { ok: false, issues: [{ path, message: `Length must be >= ${options.minLength}` }] };
        }
        if (options.maxLength !== undefined && value.length > options.maxLength) {
          return { ok: false, issues: [{ path, message: `Length must be <= ${options.maxLength}` }] };
        }
        if (options.pattern !== undefined && !options.pattern.test(value)) {
          return { ok: false, issues: [{ path, message: "String does not match pattern" }] };
        }
        return { ok: true, value };
      });
    },
    number(options = {}) {
      return makeSchema("number", (value, path) => {
        if (typeof value !== "number" || !Number.isFinite(value)) {
          return { ok: false, issues: [{ path, message: "Expected finite number" }] };
        }
        if (options.integer && !Number.isInteger(value)) {
          return { ok: false, issues: [{ path, message: "Expected integer" }] };
        }
        if (options.min !== undefined && value < options.min) {
          return { ok: false, issues: [{ path, message: `Number must be >= ${options.min}` }] };
        }
        if (options.max !== undefined && value > options.max) {
          return { ok: false, issues: [{ path, message: `Number must be <= ${options.max}` }] };
        }
        return { ok: true, value };
      });
    },
    boolean() {
      return makeSchema("boolean", (value, path) =>
        typeof value === "boolean"
          ? { ok: true, value }
          : { ok: false, issues: [{ path, message: "Expected boolean" }] }
      );
    },
    literal(expected) {
      return makeSchema("literal", (value, path) =>
        value === expected
          ? { ok: true, value }
          : { ok: false, issues: [{ path, message: `Expected literal ${String(expected)}` }] }
      );
    },
    enum(values) {
      return makeSchema("enum", (value, path) =>
        typeof value === "string" && values.includes(value)
          ? { ok: true, value }
          : { ok: false, issues: [{ path, message: `Expected one of: ${values.join(", ")}` }] }
      );
    },
    file(options = {}) {
      return makeSchema("file", (value, path) => {
        if (!value || typeof value !== "object" || Array.isArray(value)) {
          return { ok: false, issues: [{ path, message: "Expected file object" }] };
        }
        if (typeof value.name !== "string" || value.name.length === 0) {
          return { ok: false, issues: [{ path: `${path}.name`, message: "Expected non-empty file name" }] };
        }
        if (!isBinaryBuffer(value.data)) {
          return { ok: false, issues: [{ path: `${path}.data`, message: "Expected Uint8Array binary file data" }] };
        }
        const byteLength = value.data.byteLength ?? value.data.length ?? 0;
        const mimeType = typeof value.mimeType === "string" ? value.mimeType : "application/octet-stream";
        if (options.maxSizeBytes !== undefined && byteLength > options.maxSizeBytes) {
          return { ok: false, issues: [{ path: `${path}.data`, message: `File size ${byteLength} exceeds max ${options.maxSizeBytes}` }] };
        }
        if (Array.isArray(options.allowedMimeTypes) && !options.allowedMimeTypes.includes(mimeType)) {
          return { ok: false, issues: [{ path: `${path}.mimeType`, message: `Unsupported MIME type: ${mimeType}` }] };
        }
        return {
          ok: true,
          value: {
            __lampFile: true,
            name: value.name,
            mimeType,
            size: byteLength,
            data: value.data instanceof Uint8Array
              ? value.data
              : new Uint8Array(value.data.buffer, value.data.byteOffset, value.data.byteLength),
          },
        };
      });
    },
    array(itemSchema) {
      return makeSchema("array", (value, path) => {
        if (!Array.isArray(value)) {
          return { ok: false, issues: [{ path, message: "Expected array" }] };
        }
        const out = [];
        for (let i = 0; i < value.length; i++) {
          const res = safeParseInternal(itemSchema, value[i], `${path}[${i}]`);
          if (!res.ok) return res;
          out.push(res.value);
        }
        return { ok: true, value: out };
      });
    },
    object(shape) {
      return makeSchema("object", (value, path) => {
        if (typeof value !== "object" || value === null || Array.isArray(value)) {
          return { ok: false, issues: [{ path, message: "Expected plain object" }] };
        }
        const out = {};
        for (const [key, fieldSchema] of Object.entries(shape)) {
          const res = safeParseInternal(fieldSchema, value[key], `${path}.${key}`);
          if (!res.ok) return res;
          out[key] = res.value;
        }
        return { ok: true, value: out };
      });
    },
    optional(inner) {
      return makeSchema("optional", (value, path) =>
        value === undefined ? { ok: true, value: undefined } : safeParseInternal(inner, value, path)
      );
    },
    nullable(inner) {
      return makeSchema("nullable", (value, path) =>
        value === null ? { ok: true, value: null } : safeParseInternal(inner, value, path)
      );
    },
    unknown() {
      return makeSchema("unknown", (value) => ({ ok: true, value }));
    },
  };

  function validateSchema(schema, value, direction) {
    if (!schema) return value;
    if (typeof schema.safeParse === "function") {
      const res = schema.safeParse(value);
      if (res.ok) return res.value;
      const first = res.issues[0];
      const detail = first ? `${first.path}: ${first.message}` : "Validation failed";
      throw new LampError(
        direction === "input" ? `Invalid operation input (${detail})` : `Invalid operation output (${detail})`,
        {
          code: direction === "input" ? "INVALID_ARGUMENT" : "SERIALIZATION_FAILED",
          identifier: direction === "input" ? "INPUT_VALIDATION_FAILED" : "OUTPUT_VALIDATION_FAILED",
          details: { issues: res.issues },
        }
      );
    }
    try {
      return schema.parse(value);
    } catch (err) {
      if (err instanceof LampError) {
        throw new LampError(err.message, {
          code: direction === "input" ? "INVALID_ARGUMENT" : "SERIALIZATION_FAILED",
          identifier: direction === "input" ? "INPUT_VALIDATION_FAILED" : "OUTPUT_VALIDATION_FAILED",
        });
      }
      throw new LampError(
        direction === "input" ? "Invalid operation input" : "Invalid operation output",
        {
          code: direction === "input" ? "INVALID_ARGUMENT" : "SERIALIZATION_FAILED",
          identifier: direction === "input" ? "INPUT_VALIDATION_FAILED" : "OUTPUT_VALIDATION_FAILED",
        }
      );
    }
  }

  function hydrateFiles(val, files, depth = 0) {
    if (depth > 32 || !files || files.length === 0 || val === null || typeof val !== "object") {
      return val;
    }
    if (isBinaryBuffer(val)) return val;
    if (Array.isArray(val)) {
      return val.map((item) => hydrateFiles(item, files, depth + 1));
    }
    if (typeof val.__lampFileRef === "number" && Number.isInteger(val.__lampFileRef)) {
      const file = files[val.__lampFileRef];
      return file ?? val;
    }
    const out = {};
    for (const [k, v] of Object.entries(val)) {
      out[k] = hydrateFiles(v, files, depth + 1);
    }
    return out;
  }

  async function runParallelSubtasks(itemsOrTasks, mapperFn) {
    let specs;
    if (typeof mapperFn === "function" && Array.isArray(itemsOrTasks)) {
      const fnSource = mapperFn.toString();
      specs = itemsOrTasks.map((item) => ({ fn: fnSource, input: item }));
    } else if (Array.isArray(itemsOrTasks)) {
      specs = itemsOrTasks.map((entry) => {
        if (typeof entry === "function") {
          return { fn: entry.toString(), input: null };
        }
        return {
          fn: typeof entry.fn === "function" ? entry.fn.toString() : String(entry.fn ?? ""),
          input: entry.input ?? null,
        };
      });
    } else {
      throw new LampError("ctx.parallel requires an array of tasks or (items, fn)", {
        code: "INVALID_ARGUMENT",
        identifier: "PARALLEL_INVALID_ARGS",
      });
    }

    if (specs.length === 0) {
      return [];
    }

    if (
      globalThis.__lampNativeBridge &&
      typeof globalThis.__lampNativeBridge.runParallel === "function"
    ) {
      const serialized = specs.map((sp) => ({
        fn: sp.fn,
        inputJson: JSON.stringify(sp.input ?? null),
      }));
      const jsonResults = globalThis.__lampNativeBridge.runParallel(serialized);
      return jsonResults.map((raw) => (raw && raw.length > 0 ? JSON.parse(raw) : null));
    }

    const out = [];
    for (const sp of specs) {
      const compiled = new Function(`return (${sp.fn})`)();
      out.push(await compiled(sp.input));
    }
    return out;
  }

  const registeredApps = [];
  const allOps = new Map();

  function buildContext(operationId, requestId, files, parsedMeta) {
    const roles = parsedMeta["lamp.auth.roles"]
      ? String(parsedMeta["lamp.auth.roles"]).split(",").filter(Boolean)
      : [];
    const scopes = parsedMeta["lamp.auth.scopes"]
      ? String(parsedMeta["lamp.auth.scopes"]).split(",").filter(Boolean)
      : [];
    const signal = { aborted: false, addEventListener() {}, removeEventListener() {} };
    return {
      ctx: {
        requestId,
        operationId,
        metadata: parsedMeta,
        files: Array.isArray(files) ? files : [],
        signal,
        parallel: runParallelSubtasks,
        auth: {
          authenticated: parsedMeta["lamp.auth.authenticated"] === "true",
          subject: parsedMeta["lamp.auth.subject"],
          tenant: parsedMeta["lamp.auth.tenant"],
          scheme: parsedMeta["lamp.auth.scheme"],
          roles,
          scopes,
        },
      },
      signal,
    };
  }

  globalThis.__lampInvokeOperation = (operationId, rawInput, filesArray, metaObj, reqId) => {
    const entry = allOps.get(operationId);
    if (!entry) {
      throw new LampError(`Operation "${operationId}" is not registered`, {
        code: "NOT_FOUND",
        identifier: "OPERATION_NOT_FOUND",
      });
    }
    const { definition } = entry;
    const files = Array.isArray(filesArray) ? filesArray : [];
    const parsedMeta = metaObj && typeof metaObj === "object" ? metaObj : {};
    const hydrated = hydrateFiles(rawInput, files);
    const normalizedInput =
      hydrated === null && definition.input && definition.input.kind === "optional"
        ? undefined
        : hydrated;
    const validatedInput = validateSchema(definition.input, normalizedInput, "input");
    const { ctx, signal } = buildContext(operationId, reqId ?? 1, files, parsedMeta);

    try {
      const res = definition.handler({ input: validatedInput, ctx, signal });
      if (res && typeof res.then === "function") {
        return res.then(
          (out) => validateSchema(definition.output, out, "output"),
          (err) => { throw normalizeLampError(err); }
        );
      }
      return validateSchema(definition.output, res, "output");
    } catch (err) {
      throw normalizeLampError(err);
    }
  };

  function createLampApp(config = {}) {
    let currentState = "created";
    let nextRequestId = 1;
    const ops = new Map();
    const startHooks = [];
    const stopHooks = [];
    const appCacheEnabled =
      typeof config.cache === "boolean"
        ? config.cache
        : Boolean(config.cache && (config.cache.enabled ?? config.cache.mode !== "disabled"));
    const appCacheTtl =
      typeof config.cache === "object" && config.cache && config.cache.defaultTtl
        ? config.cache.defaultTtl
        : 60;

    let originPolicy = { mode: "deny_all", origins: [] };
    if (config.allowOrigin === true) {
      originPolicy = { mode: "allow_all", origins: [] };
    } else if (typeof config.allowOrigin === "function") {
      let configuredOrigins = [];
      const callback = () => {};
      const expose = (origins) => {
        if (!Array.isArray(origins)) {
          throw new LampError("allowOrigin expose() requires an array of origin strings", {
            code: "INVALID_ARGUMENT",
            identifier: "INVALID_ORIGIN_CONFIG",
          });
        }
        configuredOrigins = origins
          .map((item) => (typeof item === "string" ? item.trim() : ""))
          .filter((item) => item.length > 0);
        callback(configuredOrigins);
      };
      const allowed = Boolean(config.allowOrigin(expose, callback));
      if (allowed) {
        originPolicy = { mode: "allow_list", origins: configuredOrigins };
      }
    }
    globalThis.__lampNativeBridge?.configureOriginAllowance?.(JSON.stringify(originPolicy));

    const app = {
      register(operationId, definition) {
        if (!definition || typeof definition.handler !== "function") {
          throw new LampError(`Operation "${operationId}" requires a handler function`, {
            code: "INVALID_ARGUMENT",
            identifier: "MISSING_HANDLER",
          });
        }
        const ttl =
          typeof definition.cache === "object" && definition.cache && definition.cache.ttl !== undefined
            ? definition.cache.ttl
            : (definition.cache === true ? appCacheTtl : 0);
        const cacheEnabled =
          definition.cache === false
            ? false
            : Boolean((appCacheEnabled && ttl > 0) || (typeof definition.cache === "object" && definition.cache.enabled && ttl > 0));

        const meta = {
          id: operationId,
          description: definition.description,
          execution: {
            mode: definition.execution?.mode ?? config.execution ?? "adaptive",
            concurrency: definition.execution?.concurrency ?? "concurrent",
            timeoutMs: definition.execution?.timeoutMs ?? 30000,
          },
          cache: {
            enabled: cacheEnabled,
            ttlSeconds: cacheEnabled ? ttl : 0,
          },
          auth: {
            allowAnonymous: definition.auth?.allowAnonymous ?? !(config.security?.defaultDeny ?? false),
            roles: definition.auth?.roles ?? [],
            scopes: definition.auth?.scopes ?? [],
            tenants: definition.auth?.tenants ?? [],
          },
          tags: definition.tags ?? [],
        };

        ops.set(operationId, { meta, definition });
        allOps.set(operationId, { meta, definition });

        if (globalThis.__lampNativeBridge && typeof globalThis.__lampNativeBridge.registerOperation === "function") {
          globalThis.__lampNativeBridge.registerOperation(
            operationId,
            meta,
            (rawJsonInput, metadataJson) => {
              const parsedInput = rawJsonInput && rawJsonInput.length > 0 ? JSON.parse(rawJsonInput) : undefined;
              const parsedMeta = metadataJson && metadataJson.length > 0 ? JSON.parse(metadataJson) : {};
              return globalThis.__lampInvokeOperation(
                operationId,
                parsedInput,
                [],
                parsedMeta,
                nextRequestId++
              );
            }
          );
        }

        return app;
      },

      registerRouter(router, options) {
        if (currentState !== "created") {
          throw new LampError(
            `Cannot register router "${router && router.target ? router.target : "unknown"}" while application state is "${currentState}"`,
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

      async start() {
        if (currentState === "running") {
          return { operations: Array.from(ops.keys()) };
        }
        for (const hook of startHooks) {
          await hook();
        }
        currentState = "running";
        globalThis.__lampNativeBridge?.markStarted?.(JSON.stringify({
          transport: config.transport ?? "lmp1-binary",
          serialization: config.serialization ?? "lcb1",
          execution: config.execution ?? "parallel-dynamic",
        }));
        return { operations: Array.from(ops.keys()) };
      },

      async stop() {
        if (currentState === "stopped") return;
        for (let i = stopHooks.length - 1; i >= 0; i--) {
          await stopHooks[i]();
        }
        currentState = "stopped";
        globalThis.__lampNativeBridge?.markStopped?.();
      },

      state() { return currentState; },
      operations() { return Array.from(ops.values(), (e) => e.meta); },
      onStart(fn) { startHooks.push(fn); return app; },
      onStop(fn) { stopHooks.push(fn); return app; },
    };

    registeredApps.push(app);
    return app;
  }

  function createLampRegistry() {
    const entriesList = [];
    const seenSuffixes = new Set();
    const segmentRe = /^[a-zA-Z0-9_-]+$/;
    const registry = {
      add(operationSuffix, definition) {
        if (typeof operationSuffix !== "string" || !segmentRe.test(operationSuffix)) {
          throw new LampError(
            `Invalid operation suffix "${String(operationSuffix)}"`,
            { code: "INVALID_ARGUMENT", identifier: "INVALID_OPERATION_SUFFIX" }
          );
        }
        if (seenSuffixes.has(operationSuffix)) {
          throw new LampError(
            `Duplicate operation suffix "${operationSuffix}" in registry`,
            { code: "ALREADY_EXISTS", identifier: "DUPLICATE_OPERATION" }
          );
        }
        if (!definition || typeof definition.handler !== "function") {
          throw new LampError(
            `Operation "${operationSuffix}" requires a handler function`,
            { code: "INVALID_ARGUMENT", identifier: "MISSING_HANDLER" }
          );
        }
        seenSuffixes.add(operationSuffix);
        entriesList.push({ suffix: operationSuffix, definition });
        return registry;
      },
      entries() {
        return [...entriesList];
      },
    };
    return registry;
  }

  function createLampRouter(config) {
    const targetRe = /^[a-zA-Z0-9_-]+(\.[a-zA-Z0-9_-]+)*$/;
    if (!config || typeof config.target !== "string" || !targetRe.test(config.target)) {
      throw new LampError(
        `Invalid router target "${String(config && config.target)}"`,
        { code: "INVALID_ARGUMENT", identifier: "INVALID_ROUTER_TARGET" }
      );
    }
    const toArr = (v) => (v === undefined ? [] : Array.isArray(v) ? [...v] : [v]);
    const mergeTags = (a, b) => {
      if ((!a || a.length === 0) && (!b || b.length === 0)) return undefined;
      return Array.from(new Set([...(a || []), ...(b || [])]));
    };
    const mergeOpts = (parent, child) => ({
      execution: parent.execution || child.execution ? { ...(parent.execution || {}), ...(child.execution || {}) } : undefined,
      cache: child.cache !== undefined ? child.cache : parent.cache,
      auth: parent.auth || child.auth ? { ...(parent.auth || {}), ...(child.auth || {}) } : undefined,
      tags: mergeTags(parent.tags, child.tags),
    });

    const registries = toArr(config.registry);
    const nested = toArr(config.nesting);
    const routerOptions = {
      execution: config.options && config.options.execution ? { ...config.options.execution } : undefined,
      cache: config.options ? config.options.cache : undefined,
      auth: config.options && config.options.auth ? { ...config.options.auth } : undefined,
      tags: config.options && config.options.tags ? [...config.options.tags] : undefined,
    };

    const router = {
      description: config.description,
      target: config.target,
      options: routerOptions,
      registries,
      nested,
      resolveOperations(mountOptions = {}) {
        if (mountOptions.prefix !== undefined && !targetRe.test(mountOptions.prefix)) {
          throw new LampError(`Invalid router mount prefix "${mountOptions.prefix}"`, {
            code: "INVALID_ARGUMENT",
            identifier: "INVALID_ROUTER_PREFIX",
          });
        }
        const resolved = [];
        const seenIds = new Set();
        const stack = new Set();
        function walk(cur, parentPath, inheritedOpts) {
          if (stack.has(cur)) {
            throw new LampError(`Circular router nesting detected at target "${cur.target}"`, {
              code: "INVALID_ARGUMENT",
              identifier: "CIRCULAR_ROUTER_NESTING",
            });
          }
          stack.add(cur);
          try {
            const curPath = parentPath ? `${parentPath}.${cur.target}` : cur.target;
            const effOpts = mergeOpts(inheritedOpts, cur.options || {});
            for (const reg of cur.registries || []) {
              for (const entry of reg.entries()) {
                const operationId = `${curPath}.${entry.suffix}`;
                if (seenIds.has(operationId)) {
                  throw new LampError(`Duplicate operation "${operationId}"`, {
                    code: "ALREADY_EXISTS",
                    identifier: "DUPLICATE_OPERATION",
                  });
                }
                seenIds.add(operationId);
                resolved.push({
                  operationId,
                  definition: {
                    ...entry.definition,
                    execution: effOpts.execution || entry.definition.execution
                      ? { ...(effOpts.execution || {}), ...(entry.definition.execution || {}) }
                      : undefined,
                    cache: entry.definition.cache !== undefined ? entry.definition.cache : effOpts.cache,
                    auth: effOpts.auth || entry.definition.auth
                      ? { ...(effOpts.auth || {}), ...(entry.definition.auth || {}) }
                      : undefined,
                    tags: mergeTags(effOpts.tags, entry.definition.tags),
                  },
                });
              }
            }
            for (const child of cur.nested || []) {
              walk(child, curPath, effOpts);
            }
          } finally {
            stack.delete(cur);
          }
        }
        walk(router, mountOptions.prefix || "", {
          execution: mountOptions.execution,
          cache: mountOptions.cache,
          auth: mountOptions.auth,
          tags: mountOptions.tags,
        });
        return resolved;
      },
    };
    return router;
  }

  globalThis.Lamp = { createLampApp, createLampRegistry, createLampRouter, s, LampError, isLampError };
  globalThis.createLampApp = createLampApp;
  globalThis.createLampRegistry = createLampRegistry;
  globalThis.createLampRouter = createLampRouter;
  globalThis.s = s;
  globalThis.LampError = LampError;
  globalThis.isLampError = isLampError;
  globalThis.__lampStopAllApps = async () => {
    for (const app of registeredApps) {
      await app.stop();
    }
  };
})();
)JS";

namespace detail {

// Pooled stateless V8 isolates used by core::DynamicWorkerPool threads to
// execute split handler subtasks (ctx.parallel) concurrently without lock contention.
class V8SubtaskIsolatePool final {
public:
    explicit V8SubtaskIsolatePool(std::size_t max_isolates)
        : max_isolates_(max_isolates > 0 ? max_isolates : 4U) {}

    ~V8SubtaskIsolatePool() noexcept {
        Shutdown();
    }

    V8SubtaskIsolatePool(const V8SubtaskIsolatePool&) = delete;
    V8SubtaskIsolatePool& operator=(const V8SubtaskIsolatePool&) = delete;

    void Shutdown() noexcept {
        std::vector<Slot*> to_dispose;
        {
            std::lock_guard lock(mutex_);
            shutting_down_ = true;
            for (auto& slot : slots_) {
                to_dispose.push_back(slot.get());
            }
        }
        for (auto* slot : to_dispose) {
            if (slot->isolate != nullptr) {
                slot->isolate->Dispose();
                slot->isolate = nullptr;
            }
            delete slot->allocator;
            slot->allocator = nullptr;
        }
        std::lock_guard lock(mutex_);
        slots_.clear();
    }

    // Executes `fn_source(JSON.parse(input_json))` inside an isolated worker V8
    // context on the calling OS thread and returns JSON.stringify(result).
    std::string RunTask(std::string_view fn_source, std::string_view input_json) {
        Slot* slot = AcquireSlot();
        struct Releaser {
            V8SubtaskIsolatePool* pool;
            Slot* s;
            ~Releaser() { pool->ReleaseSlot(s); }
        } releaser{this, slot};

        ::v8::Locker locker(slot->isolate);
        ::v8::Isolate::Scope isolate_scope(slot->isolate);
        ::v8::HandleScope handle_scope(slot->isolate);
        ::v8::Local<::v8::Context> ctx = ::v8::Context::New(slot->isolate);
        ::v8::Context::Scope context_scope(ctx);
        ::v8::TryCatch try_catch(slot->isolate);

        std::string wrapper;
        wrapper.reserve(fn_source.size() + input_json.size() + 128);
        wrapper.append("(() => { const __fn = (");
        wrapper.append(fn_source);
        wrapper.append("); const __arg = ");
        wrapper.append(input_json.empty() ? "null" : input_json);
        wrapper.append("; const __out = __fn(__arg); return JSON.stringify(__out === undefined ? null : __out); })()");

        ::v8::Local<::v8::String> src;
        if (!::v8::String::NewFromUtf8(
                slot->isolate,
                wrapper.data(),
                ::v8::NewStringType::kNormal,
                static_cast<int>(wrapper.size())
            ).ToLocal(&src)) {
            throw std::runtime_error("Subtask JS source encoding failed");
        }

        ::v8::Local<::v8::Script> script;
        if (!::v8::Script::Compile(ctx, src).ToLocal(&script)) {
            ::v8::String::Utf8Value err(slot->isolate, try_catch.Exception());
            throw std::runtime_error(*err ? *err : "Subtask compile error");
        }

        ::v8::Local<::v8::Value> res;
        if (!script->Run(ctx).ToLocal(&res)) {
            ::v8::String::Utf8Value err(slot->isolate, try_catch.Exception());
            throw std::runtime_error(*err ? *err : "Subtask execution error");
        }

        if (res->IsString()) {
            ::v8::String::Utf8Value utf8(slot->isolate, res);
            return *utf8 ? std::string(*utf8, utf8.length()) : std::string("null");
        }
        return "null";
    }

private:
    struct Slot final {
        ::v8::ArrayBuffer::Allocator* allocator = nullptr;
        ::v8::Isolate* isolate = nullptr;
        bool in_use = false;
    };

    Slot* AcquireSlot() {
        std::unique_lock lock(mutex_);
        for (;;) {
            if (shutting_down_) {
                throw std::runtime_error("Subtask isolate pool is shutting down");
            }
            for (auto& s : slots_) {
                if (!s->in_use) {
                    s->in_use = true;
                    return s.get();
                }
            }
            if (slots_.size() < max_isolates_) {
                auto slot = std::make_unique<Slot>();
                slot->allocator = ::v8::ArrayBuffer::Allocator::NewDefaultAllocator();
                ::v8::Isolate::CreateParams params;
                params.array_buffer_allocator = slot->allocator;
                slot->isolate = ::v8::Isolate::New(params);
                slot->in_use = true;
                Slot* ptr = slot.get();
                slots_.push_back(std::move(slot));
                return ptr;
            }
            cv_.wait(lock);
        }
    }

    void ReleaseSlot(Slot* slot) noexcept {
        {
            std::lock_guard lock(mutex_);
            slot->in_use = false;
        }
        cv_.notify_one();
    }

    std::size_t max_isolates_ = 8;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<std::unique_ptr<Slot>> slots_;
    bool shutting_down_ = false;
};

[[nodiscard]]
inline ::v8::Local<::v8::Value> SerializationValueToV8(
    ::v8::Isolate* isolate,
    ::v8::Local<::v8::Context> ctx,
    const serialization::Value& val
) {
    using K = serialization::ValueKind;
    switch (val.Kind()) {
        case K::Null:
            return ::v8::Null(isolate);
        case K::Bool:
            return ::v8::Boolean::New(isolate, val.AsBool().value_or(false));
        case K::Int64:
            return ::v8::Number::New(isolate, static_cast<double>(*val.AsInt64()));
        case K::UInt64:
            return ::v8::Number::New(isolate, static_cast<double>(*val.AsUInt64()));
        case K::Double:
            return ::v8::Number::New(isolate, *val.AsDouble());
        case K::String: {
            const auto& s = *val.AsString();
            return ::v8::String::NewFromUtf8(
                isolate,
                s.data(),
                ::v8::NewStringType::kNormal,
                static_cast<int>(s.size())
            ).ToLocalChecked();
        }
        case K::Bytes: {
            const auto* bytes = val.AsBytes();
            auto ab = ::v8::ArrayBuffer::New(isolate, bytes->size());
            if (!bytes->empty()) {
                std::memcpy(ab->GetBackingStore()->Data(), bytes->data(), bytes->size());
            }
            return ::v8::Uint8Array::New(ab, 0, bytes->size());
        }
        case K::Array: {
            const auto* arr = val.AsArray();
            auto out = ::v8::Array::New(isolate, static_cast<int>(arr->size()));
            for (std::size_t i = 0; i < arr->size(); ++i) {
                (void)out->Set(ctx, static_cast<std::uint32_t>(i), SerializationValueToV8(isolate, ctx, (*arr)[i]));
            }
            return out;
        }
        case K::Object: {
            const auto* obj = val.AsObject();
            auto out = ::v8::Object::New(isolate);
            for (const auto& [k, v] : *obj) {
                auto v8_key = ::v8::String::NewFromUtf8(
                    isolate,
                    k.data(),
                    ::v8::NewStringType::kNormal,
                    static_cast<int>(k.size())
                ).ToLocalChecked();
                (void)out->Set(ctx, v8_key, SerializationValueToV8(isolate, ctx, v));
            }
            return out;
        }
    }
    return ::v8::Null(isolate);
}

[[nodiscard]]
inline ::v8::Local<::v8::Array> FilesToV8Array(
    ::v8::Isolate* isolate,
    ::v8::Local<::v8::Context> ctx,
    const std::vector<transport::LampFilePayload>& files
) {
    auto arr = ::v8::Array::New(isolate, static_cast<int>(files.size()));
    for (std::size_t i = 0; i < files.size(); ++i) {
        const auto& f = files[i];
        auto obj = ::v8::Object::New(isolate);

        auto ab = ::v8::ArrayBuffer::New(isolate, f.data.size());
        if (!f.data.empty()) {
            std::memcpy(ab->GetBackingStore()->Data(), f.data.data(), f.data.size());
        }
        auto u8 = ::v8::Uint8Array::New(ab, 0, f.data.size());

        (void)obj->Set(ctx, ::v8::String::NewFromUtf8Literal(isolate, "__lampFile"), ::v8::Boolean::New(isolate, true));
        (void)obj->Set(
            ctx,
            ::v8::String::NewFromUtf8Literal(isolate, "name"),
            ::v8::String::NewFromUtf8(isolate, f.name.data(), ::v8::NewStringType::kNormal, static_cast<int>(f.name.size())).ToLocalChecked()
        );
        (void)obj->Set(
            ctx,
            ::v8::String::NewFromUtf8Literal(isolate, "mimeType"),
            ::v8::String::NewFromUtf8(isolate, f.mime_type.data(), ::v8::NewStringType::kNormal, static_cast<int>(f.mime_type.size())).ToLocalChecked()
        );
        (void)obj->Set(
            ctx,
            ::v8::String::NewFromUtf8Literal(isolate, "size"),
            ::v8::Number::New(isolate, static_cast<double>(f.data.size()))
        );
        (void)obj->Set(ctx, ::v8::String::NewFromUtf8Literal(isolate, "data"), u8);

        (void)arr->Set(ctx, static_cast<std::uint32_t>(i), obj);
    }
    return arr;
}

inline bool V8ToSerializationValue(
    ::v8::Isolate* isolate,
    ::v8::Local<::v8::Context> ctx,
    ::v8::Local<::v8::Value> val,
    serialization::Value& out,
    std::vector<transport::LampFilePayload>& out_files,
    int depth = 0
) {
    if (depth > 32) {
        return false;
    }
    if (val.IsEmpty() || val->IsNull() || val->IsUndefined()) {
        out = serialization::Value::MakeNull();
        return true;
    }
    if (val->IsBoolean()) {
        out = serialization::Value(val->BooleanValue(isolate));
        return true;
    }
    if (val->IsNumber()) {
        const double d = val->NumberValue(ctx).FromMaybe(0.0);
        if (std::isfinite(d) && std::floor(d) == d &&
            d >= static_cast<double>((std::numeric_limits<std::int64_t>::min)()) &&
            d <= static_cast<double>((std::numeric_limits<std::int64_t>::max)())) {
            out = serialization::Value(static_cast<std::int64_t>(d));
        } else {
            out = serialization::Value(d);
        }
        return true;
    }
    if (val->IsString()) {
        ::v8::String::Utf8Value utf8(isolate, val);
        out = serialization::Value(*utf8 ? std::string(*utf8, utf8.length()) : std::string{});
        return true;
    }
    if (val->IsArrayBufferView()) {
        auto view = val.As<::v8::ArrayBufferView>();
        std::vector<std::uint8_t> bytes(view->ByteLength());
        if (!bytes.empty()) {
            view->CopyContents(bytes.data(), bytes.size());
        }
        out = serialization::Value(std::move(bytes));
        return true;
    }
    if (val->IsArray()) {
        auto arr = val.As<::v8::Array>();
        serialization::Value::Array items;
        items.reserve(arr->Length());
        for (std::uint32_t i = 0; i < arr->Length(); ++i) {
            ::v8::Local<::v8::Value> elem;
            if (!arr->Get(ctx, i).ToLocal(&elem)) return false;
            serialization::Value item_val;
            if (!V8ToSerializationValue(isolate, ctx, elem, item_val, out_files, depth + 1)) {
                return false;
            }
            items.push_back(std::move(item_val));
        }
        out = serialization::Value::MakeArray(std::move(items));
        return true;
    }
    if (val->IsObject()) {
        auto obj = val.As<::v8::Object>();

        // Detect LampFile object ({ __lampFile: true, name, mimeType, data: Uint8Array })
        ::v8::Local<::v8::Value> file_flag;
        if (obj->Get(ctx, ::v8::String::NewFromUtf8Literal(isolate, "__lampFile")).ToLocal(&file_flag) &&
            file_flag->IsTrue()) {
            ::v8::Local<::v8::Value> name_v, mime_v, data_v;
            if (obj->Get(ctx, ::v8::String::NewFromUtf8Literal(isolate, "name")).ToLocal(&name_v) &&
                obj->Get(ctx, ::v8::String::NewFromUtf8Literal(isolate, "data")).ToLocal(&data_v) &&
                data_v->IsArrayBufferView()) {
                transport::LampFilePayload fp;
                if (name_v->IsString()) {
                    ::v8::String::Utf8Value n_u8(isolate, name_v);
                    fp.name = *n_u8 ? std::string(*n_u8, n_u8.length()) : "file.bin";
                }
                if (obj->Get(ctx, ::v8::String::NewFromUtf8Literal(isolate, "mimeType")).ToLocal(&mime_v) &&
                    mime_v->IsString()) {
                    ::v8::String::Utf8Value m_u8(isolate, mime_v);
                    fp.mime_type = *m_u8 ? std::string(*m_u8, m_u8.length()) : "application/octet-stream";
                }
                auto view = data_v.As<::v8::ArrayBufferView>();
                fp.data.resize(view->ByteLength());
                if (!fp.data.empty()) {
                    view->CopyContents(fp.data.data(), fp.data.size());
                }
                const std::size_t idx = out_files.size();
                out_files.push_back(std::move(fp));

                serialization::Value::Object ref_obj;
                ref_obj.emplace_back("__lampFileRef", serialization::Value(static_cast<std::int64_t>(idx)));
                out = serialization::Value::MakeObject(std::move(ref_obj));
                return true;
            }
        }

        ::v8::Local<::v8::Array> keys;
        if (!obj->GetOwnPropertyNames(ctx, ::v8::PropertyFilter::ONLY_ENUMERABLE).ToLocal(&keys)) {
            return false;
        }
        serialization::Value::Object entries;
        entries.reserve(keys->Length());
        for (std::uint32_t i = 0; i < keys->Length(); ++i) {
            ::v8::Local<::v8::Value> k_val;
            if (!keys->Get(ctx, i).ToLocal(&k_val)) continue;
            ::v8::Local<::v8::Value> v_val;
            if (!obj->Get(ctx, k_val).ToLocal(&v_val)) continue;
            if (v_val->IsUndefined() || v_val->IsFunction()) continue;

            ::v8::String::Utf8Value k_u8(isolate, k_val);
            serialization::Value child;
            if (!V8ToSerializationValue(isolate, ctx, v_val, child, out_files, depth + 1)) {
                return false;
            }
            entries.emplace_back(*k_u8 ? std::string(*k_u8, k_u8.length()) : std::string{}, std::move(child));
        }
        out = serialization::Value::MakeObject(std::move(entries));
        return true;
    }

    out = serialization::Value::MakeNull();
    return true;
}

} // namespace detail

struct LampV8SchedulerConfig final {
    std::string script_source;
    std::string script_name = "app.js";
    std::size_t max_pending_tasks = 4096;
    std::size_t max_parallel_isolates = 16;
    std::shared_ptr<core::DynamicWorkerPool> worker_pool;
    std::shared_ptr<core::OriginAllowancePolicy> origin_policy;
};

class LampV8RuntimeScheduler final
    : public execution::IAsyncExecutionScheduler {
public:
    explicit LampV8RuntimeScheduler(
        std::shared_ptr<bridge::OperationRegistry> registry,
        std::shared_ptr<cache::LocalOperationCache> cache =
            std::make_shared<cache::LocalOperationCache>(),
        std::shared_ptr<telemetry::MetricsRegistry> metrics =
            std::make_shared<telemetry::MetricsRegistry>(),
        std::shared_ptr<telemetry::Tracer> tracer =
            std::make_shared<telemetry::Tracer>(),
        LampV8SchedulerConfig config = {}
    )
        : registry_(std::move(registry)),
          cache_(std::move(cache)),
          metrics_(std::move(metrics)),
          tracer_(std::move(tracer)),
          config_(std::move(config)),
          subtask_pool_(std::make_unique<detail::V8SubtaskIsolatePool>(
              config_.max_parallel_isolates > 0 ? config_.max_parallel_isolates : 16U)) {
        if (!registry_) {
            throw std::invalid_argument("LampV8RuntimeScheduler requires OperationRegistry");
        }
        core::DynamicWorkerPoolConfig sub_cfg;
        sub_cfg.min_threads = 2;
        sub_cfg.max_threads = config_.max_parallel_isolates > 0 ? config_.max_parallel_isolates : 16U;
        subtask_worker_pool_ = std::make_unique<core::DynamicWorkerPool>(sub_cfg);
    }

    ~LampV8RuntimeScheduler() override {
        Shutdown();
        if (subtask_worker_pool_) {
            subtask_worker_pool_->Shutdown(true);
        }
        if (subtask_pool_) {
            subtask_pool_->Shutdown();
        }
    }

    LampV8RuntimeScheduler(const LampV8RuntimeScheduler&) = delete;
    LampV8RuntimeScheduler& operator=(const LampV8RuntimeScheduler&) = delete;

    void Start() {
        bool expected = false;
        if (!started_.compare_exchange_strong(expected, true)) {
            return;
        }

        lamp::v8::V8Platform::instance().initialize({});

        std::promise<void> ready_promise;
        auto ready_future = ready_promise.get_future();

        worker_thread_ = std::thread([this, &ready_promise] {
            WorkerMain(ready_promise);
        });

        ready_future.get();
    }

    // Executes a decoded binary RPC invocation (LCB1 + raw binary files) on
    // the V8 scheduler and returns an encoded LCB1 response + cache TTL.
    [[nodiscard]]
    transport::BinaryRpcDispatchResult ExecuteBinary(
        const transport::DecodedRpcInvocation& invocation,
        std::uint64_t request_id
    ) {
        std::promise<transport::BinaryRpcDispatchResult> promise;
        auto fut = promise.get_future();

        {
            std::lock_guard lock(queue_mutex_);
            if (!running_.load(std::memory_order_acquire) || stopping_) {
                transport::DecodedRpcResult err;
                err.ok = false;
                err.error_code = "UNAVAILABLE";
                err.error_identifier = "RUNTIME_STOPPED";
                err.error_message = "V8 runtime scheduler is stopped";
                err.retryable = true;
                return transport::BinaryRpcDispatchResult{
                    false,
                    transport::LcbCodec::EncodeRpcResponse(err),
                    std::chrono::milliseconds{0}
                };
            }
            if (queue_.size() + binary_queue_.size() >= config_.max_pending_tasks) {
                transport::DecodedRpcResult err;
                err.ok = false;
                err.error_code = "RESOURCE_EXHAUSTED";
                err.error_identifier = "QUEUE_FULL";
                err.error_message = "Runtime execution queue is full";
                err.retryable = true;
                return transport::BinaryRpcDispatchResult{
                    false,
                    transport::LcbCodec::EncodeRpcResponse(err),
                    std::chrono::milliseconds{0}
                };
            }
            binary_queue_.push_back(QueuedBinaryTask{
                &invocation,
                request_id,
                std::move(promise)
            });
        }
        queue_cv_.notify_one();
        return fut.get();
    }

    [[nodiscard]]
    execution::AsyncSubmitResult Submit(
        bridge::RequestEnvelope request,
        execution::AsyncScheduleOptions options,
        execution::AsyncTaskHandle& output
    ) override {
        if (request.IsCancelled()) {
            return execution::AsyncSubmitResult::Failure(
                execution::AsyncSubmitStatus::Cancelled,
                "Request cancelled prior to submission"
            );
        }
        if (options.reject_expired && request.IsExpired()) {
            return execution::AsyncSubmitResult::Failure(
                execution::AsyncSubmitStatus::DeadlineExceeded,
                "Request deadline already expired"
            );
        }

        auto completion = std::make_shared<execution::AsyncTaskCompletion>(request);
        if (!completion->MarkQueued()) {
            return execution::AsyncSubmitResult::Failure(
                execution::AsyncSubmitStatus::InternalError,
                "Unable to queue task completion"
            );
        }

        {
            std::lock_guard lock(queue_mutex_);
            if (!running_.load(std::memory_order_acquire) || stopping_) {
                return execution::AsyncSubmitResult::Failure(
                    execution::AsyncSubmitStatus::SchedulerStopped,
                    "V8 runtime scheduler is stopped"
                );
            }
            if (queue_.size() + binary_queue_.size() >= config_.max_pending_tasks) {
                return execution::AsyncSubmitResult::Failure(
                    execution::AsyncSubmitStatus::QueueFull,
                    "V8 operation queue is full"
                );
            }
            queue_.push_back(QueuedTask{std::move(request), completion});
        }

        output = execution::AsyncTaskHandle(completion);
        queue_cv_.notify_one();
        return execution::AsyncSubmitResult::Success();
    }

    [[nodiscard]]
    bool IsRunning() const noexcept override {
        return running_.load(std::memory_order_acquire);
    }

    [[nodiscard]]
    std::size_t PendingTasks() const noexcept override {
        std::lock_guard lock(queue_mutex_);
        return queue_.size() + binary_queue_.size();
    }

    [[nodiscard]]
    std::size_t ActiveTasks() const noexcept override {
        return active_tasks_.load(std::memory_order_acquire);
    }

    void Shutdown() override {
        std::deque<QueuedTask> remaining;
        std::deque<QueuedBinaryTask> remaining_bin;
        {
            std::lock_guard lock(queue_mutex_);
            if (stopping_) {
                return;
            }
            stopping_ = true;
            running_.store(false, std::memory_order_release);
            remaining.swap(queue_);
            remaining_bin.swap(binary_queue_);
        }
        queue_cv_.notify_all();

        for (auto& task : remaining) {
            if (task.completion->MarkRunning()) {
                (void)task.completion->Complete(
                    bridge::ResultEnvelope::Cancelled(task.request)
                );
            }
        }
        for (auto& btask : remaining_bin) {
            transport::DecodedRpcResult err;
            err.ok = false;
            err.error_code = "CANCELLED";
            err.error_identifier = "RUNTIME_STOPPED";
            err.error_message = "V8 runtime scheduler stopped";
            btask.promise.set_value(transport::BinaryRpcDispatchResult{
                false,
                transport::LcbCodec::EncodeRpcResponse(err),
                std::chrono::milliseconds{0}
            });
        }

        if (worker_thread_.joinable()) {
            worker_thread_.join();
        }
        if (subtask_worker_pool_) {
            subtask_worker_pool_->Shutdown(true);
        }
        if (subtask_pool_) {
            subtask_pool_->Shutdown();
        }
    }

    [[nodiscard]]
    std::size_t PeakSubtaskThreads() const noexcept {
        return subtask_worker_pool_ ? subtask_worker_pool_->PeakThreadCount() : 0U;
    }

private:
    struct QueuedTask final {
        bridge::RequestEnvelope request;
        std::shared_ptr<execution::AsyncTaskCompletion> completion;
    };

    struct QueuedBinaryTask final {
        const transport::DecodedRpcInvocation* invocation = nullptr;
        std::uint64_t request_id = 0;
        std::promise<transport::BinaryRpcDispatchResult> promise;
    };

    static void ConsoleLogCallback(const ::v8::FunctionCallbackInfo<::v8::Value>& info) {
        auto* isolate = info.GetIsolate();
        auto context = isolate->GetCurrentContext();
        for (int i = 0; i < info.Length(); ++i) {
            if (i > 0) std::cout << ' ';
            if (info[i]->IsObject() && !info[i]->IsFunction()) {
                ::v8::Local<::v8::String> json_str;
                if (::v8::JSON::Stringify(context, info[i]).ToLocal(&json_str)) {
                    ::v8::String::Utf8Value jutf8(isolate, json_str);
                    if (*jutf8) {
                        std::cout << *jutf8;
                        continue;
                    }
                }
            }
            ::v8::String::Utf8Value utf8(isolate, info[i]);
            if (*utf8) std::cout << *utf8;
        }
        std::cout << '\n';
    }

    static void OnRunParallel(const ::v8::FunctionCallbackInfo<::v8::Value>& info) {
        auto* isolate = info.GetIsolate();
        if (info.Data().IsEmpty()) {
            isolate->ThrowError("Missing scheduler reference");
            return;
        }
        auto data_val = info.Data().As<::v8::Value>();
        if (!data_val->IsExternal()) {
            isolate->ThrowError("Missing scheduler external reference");
            return;
        }
        auto* self = static_cast<LampV8RuntimeScheduler*>(
            data_val.As<::v8::External>()->Value(::v8::kExternalPointerTypeTagDefault)
        );
        if (!self || info.Length() < 1 || !info[0]->IsArray()) {
            isolate->ThrowError("runParallel expects an array of { fn, inputJson }");
            return;
        }

        auto context = isolate->GetCurrentContext();
        auto arr = info[0].As<::v8::Array>();
        const std::uint32_t count = arr->Length();

        struct TaskSpec {
            std::string fn;
            std::string input_json;
        };
        std::vector<TaskSpec> specs;
        specs.reserve(count);

        for (std::uint32_t i = 0; i < count; ++i) {
            ::v8::Local<::v8::Value> elem;
            if (!arr->Get(context, i).ToLocal(&elem) || !elem->IsObject()) {
                isolate->ThrowError("Each parallel task must be an object");
                return;
            }
            auto obj = elem.As<::v8::Object>();
            ::v8::Local<::v8::Value> fn_v, in_v;
            if (!obj->Get(context, ::v8::String::NewFromUtf8Literal(isolate, "fn")).ToLocal(&fn_v) ||
                !fn_v->IsString()) {
                isolate->ThrowError("Parallel task requires fn string");
                return;
            }
            ::v8::String::Utf8Value fn_u8(isolate, fn_v);
            std::string in_json = "null";
            if (obj->Get(context, ::v8::String::NewFromUtf8Literal(isolate, "inputJson")).ToLocal(&in_v) &&
                in_v->IsString()) {
                ::v8::String::Utf8Value in_u8(isolate, in_v);
                if (*in_u8) in_json.assign(*in_u8, in_u8.length());
            }
            specs.push_back(TaskSpec{
                *fn_u8 ? std::string(*fn_u8, fn_u8.length()) : std::string{},
                std::move(in_json)
            });
        }

        std::vector<std::function<std::string()>> callables;
        callables.reserve(specs.size());
        for (const auto& sp : specs) {
            callables.emplace_back([pool = self->subtask_pool_.get(),
                                    fn = sp.fn,
                                    in = sp.input_json]() {
                return pool->RunTask(fn, in);
            });
        }

        try {
            auto results = self->subtask_worker_pool_->RunParallel<std::string>(std::move(callables));
            auto out = ::v8::Array::New(isolate, static_cast<int>(results.size()));
            for (std::size_t i = 0; i < results.size(); ++i) {
                auto v = ::v8::String::NewFromUtf8(
                    isolate,
                    results[i].data(),
                    ::v8::NewStringType::kNormal,
                    static_cast<int>(results[i].size())
                ).ToLocalChecked();
                (void)out->Set(context, static_cast<std::uint32_t>(i), v);
            }
            info.GetReturnValue().Set(out);
        } catch (const std::exception& ex) {
            const std::string msg = ex.what();
            isolate->ThrowError(
                ::v8::String::NewFromUtf8(isolate, msg.c_str()).ToLocalChecked()
            );
        }
    }

    void InstallConsoleAndParallel(::v8::Isolate* isolate, ::v8::Local<::v8::Context> context) {
        auto console_obj = ::v8::Object::New(isolate);
        ::v8::Local<::v8::Function> log_fn;
        if (::v8::Function::New(context, &LampV8RuntimeScheduler::ConsoleLogCallback).ToLocal(&log_fn)) {
            (void)console_obj->Set(context, ::v8::String::NewFromUtf8Literal(isolate, "log"), log_fn);
            (void)console_obj->Set(context, ::v8::String::NewFromUtf8Literal(isolate, "info"), log_fn);
            (void)console_obj->Set(context, ::v8::String::NewFromUtf8Literal(isolate, "error"), log_fn);
        }
        (void)context->Global()->Set(context, ::v8::String::NewFromUtf8Literal(isolate, "console"), console_obj);

        ::v8::Local<::v8::Value> bridge_val;
        if (context->Global()->Get(context, ::v8::String::NewFromUtf8Literal(isolate, "__lampNativeBridge")).ToLocal(&bridge_val) &&
            bridge_val->IsObject()) {
            auto self_ext = ::v8::External::New(isolate, this, ::v8::kExternalPointerTypeTagDefault);
            ::v8::Local<::v8::Function> par_fn;
            if (::v8::Function::New(context, &LampV8RuntimeScheduler::OnRunParallel, self_ext).ToLocal(&par_fn)) {
                (void)bridge_val.As<::v8::Object>()->Set(
                    context,
                    ::v8::String::NewFromUtf8Literal(isolate, "runParallel"),
                    par_fn
                );
            }
            ::v8::Local<::v8::Function> origin_fn;
            if (::v8::Function::New(context, &LampV8RuntimeScheduler::OnConfigureOriginAllowance, self_ext).ToLocal(&origin_fn)) {
                (void)bridge_val.As<::v8::Object>()->Set(
                    context,
                    ::v8::String::NewFromUtf8Literal(isolate, "configureOriginAllowance"),
                    origin_fn
                );
            }
        }
    }

    static void OnConfigureOriginAllowance(const ::v8::FunctionCallbackInfo<::v8::Value>& info) {
        auto* isolate = info.GetIsolate();
        if (info.Data().IsEmpty() || !info.Data()->IsValue() ||
            info.Length() < 1 || !info[0]->IsString()) {
            return;
        }
        auto data_val = info.Data().As<::v8::Value>();
        if (!data_val->IsExternal()) {
            return;
        }
        auto* self = static_cast<LampV8RuntimeScheduler*>(
            data_val.As<::v8::External>()->Value(::v8::kExternalPointerTypeTagDefault)
        );
        if (self == nullptr || !self->config_.origin_policy) {
            return;
        }

        auto context = isolate->GetCurrentContext();
        ::v8::Local<::v8::Value> parsed_val;
        if (!::v8::JSON::Parse(context, info[0].As<::v8::String>()).ToLocal(&parsed_val) ||
            !parsed_val->IsObject()) {
            return;
        }
        auto obj = parsed_val.As<::v8::Object>();

        core::OriginAllowanceConfig policy_cfg;
        policy_cfg.mode = core::OriginAllowanceMode::DenyAll;

        ::v8::Local<::v8::Value> mode_val;
        if (obj->Get(context, ::v8::String::NewFromUtf8Literal(isolate, "mode")).ToLocal(&mode_val) &&
            mode_val->IsString()) {
            ::v8::String::Utf8Value mode_u8(isolate, mode_val);
            const std::string_view mode_sv = *mode_u8 ? std::string_view(*mode_u8, mode_u8.length()) : std::string_view{};
            if (mode_sv == "allow_all") {
                policy_cfg.mode = core::OriginAllowanceMode::AllowAll;
            } else if (mode_sv == "allow_list") {
                policy_cfg.mode = core::OriginAllowanceMode::AllowList;
            } else {
                policy_cfg.mode = core::OriginAllowanceMode::DenyAll;
            }
        }

        ::v8::Local<::v8::Value> origins_val;
        if (obj->Get(context, ::v8::String::NewFromUtf8Literal(isolate, "origins")).ToLocal(&origins_val) &&
            origins_val->IsArray()) {
            auto arr = origins_val.As<::v8::Array>();
            const std::uint32_t len = arr->Length();
            policy_cfg.allowedOrigins.reserve(len);
            for (std::uint32_t i = 0; i < len; ++i) {
                ::v8::Local<::v8::Value> item;
                if (arr->Get(context, i).ToLocal(&item) && item->IsString()) {
                    ::v8::String::Utf8Value item_u8(isolate, item);
                    if (*item_u8) {
                        policy_cfg.allowedOrigins.emplace_back(*item_u8, item_u8.length());
                    }
                }
            }
        }

        self->config_.origin_policy->configure(std::move(policy_cfg));
    }

    static std::string FormatV8Exception(
        ::v8::Isolate* isolate,
        ::v8::Local<::v8::Context> context,
        ::v8::TryCatch& try_catch
    ) {
        ::v8::String::Utf8Value exception(isolate, try_catch.Exception());
        std::string msg = *exception ? std::string(*exception) : std::string("Unknown JS error");
        auto message = try_catch.Message();
        if (!message.IsEmpty()) {
            const int line = message->GetLineNumber(context).FromMaybe(-1);
            if (line >= 0) {
                msg += " (line " + std::to_string(line) + ")";
            }
        }
        return msg;
    }

    static bool EvalScript(
        ::v8::Isolate* isolate,
        ::v8::Local<::v8::Context> context,
        std::string_view code,
        std::string_view origin_name,
        std::string& error_out
    ) {
        ::v8::TryCatch try_catch(isolate);
        ::v8::Local<::v8::String> source;
        if (!::v8::String::NewFromUtf8(
                isolate,
                code.data(),
                ::v8::NewStringType::kNormal,
                static_cast<int>(code.size())
            ).ToLocal(&source)) {
            error_out = "Failed to encode JS source as UTF-8";
            return false;
        }

        ::v8::Local<::v8::String> name;
        if (!::v8::String::NewFromUtf8(
                isolate,
                origin_name.data(),
                ::v8::NewStringType::kNormal,
                static_cast<int>(origin_name.size())
            ).ToLocal(&name)) {
            name = ::v8::String::NewFromUtf8Literal(isolate, "lamp.js");
        }

        ::v8::ScriptOrigin origin(name);
        ::v8::Local<::v8::Script> script;
        if (!::v8::Script::Compile(context, source, &origin).ToLocal(&script)) {
            error_out = FormatV8Exception(isolate, context, try_catch);
            return false;
        }

        ::v8::Local<::v8::Value> eval_result;
        if (!script->Run(context).ToLocal(&eval_result)) {
            error_out = FormatV8Exception(isolate, context, try_catch);
            return false;
        }

        isolate->PerformMicrotaskCheckpoint();

        if (!eval_result.IsEmpty() && eval_result->IsPromise()) {
            auto promise = eval_result.As<::v8::Promise>();
            for (int i = 0; i < 64 && promise->State() == ::v8::Promise::kPending; ++i) {
                isolate->PerformMicrotaskCheckpoint();
            }
            if (promise->State() == ::v8::Promise::kRejected) {
                ::v8::String::Utf8Value rej_utf8(isolate, promise->Result());
                error_out = *rej_utf8 ? std::string(*rej_utf8) : std::string("Startup promise rejected");
                return false;
            }
        }
        return true;
    }

    transport::BinaryRpcDispatchResult ExecuteBinaryOnIsolate(
        ::v8::Isolate* isolate,
        ::v8::Local<::v8::Context> context,
        const transport::DecodedRpcInvocation& invocation,
        std::uint64_t request_id
    ) {
        auto make_err = [](std::string code, std::string id, std::string msg, bool retryable = false) {
            transport::DecodedRpcResult res;
            res.ok = false;
            res.error_code = std::move(code);
            res.error_identifier = std::move(id);
            res.error_message = std::move(msg);
            res.retryable = retryable;
            return transport::BinaryRpcDispatchResult{
                false,
                transport::LcbCodec::EncodeRpcResponse(res),
                std::chrono::milliseconds{0}
            };
        };

        auto op_reg = registry_->Find(invocation.operation_id);
        if (!op_reg) {
            return make_err("NOT_FOUND", "OPERATION_NOT_FOUND",
                            "Operation '" + invocation.operation_id + "' is not registered");
        }

        const auto cache_ttl = (op_reg->descriptor.cache.mode != bridge::CacheMode::Disabled)
            ? std::chrono::duration_cast<std::chrono::milliseconds>(op_reg->descriptor.cache.ttl)
            : std::chrono::milliseconds{0};

        ::v8::HandleScope handle_scope(isolate);
        ::v8::Context::Scope context_scope(context);
        ::v8::TryCatch try_catch(isolate);

        ::v8::Local<::v8::Value> invoke_val;
        if (!context->Global()->Get(context, ::v8::String::NewFromUtf8Literal(isolate, "__lampInvokeOperation")).ToLocal(&invoke_val) ||
            !invoke_val->IsFunction()) {
            return make_err("INTERNAL", "MISSING_INVOKE_HOOK", "__lampInvokeOperation missing");
        }

        auto invoke_fn = invoke_val.As<::v8::Function>();
        auto v8_op = ::v8::String::NewFromUtf8(
            isolate,
            invocation.operation_id.data(),
            ::v8::NewStringType::kNormal,
            static_cast<int>(invocation.operation_id.size())
        ).ToLocalChecked();
        auto v8_input = detail::SerializationValueToV8(isolate, context, invocation.input);
        auto v8_files = detail::FilesToV8Array(isolate, context, invocation.files);
        auto v8_meta = ::v8::Object::New(isolate);
        auto v8_req_id = ::v8::Number::New(isolate, static_cast<double>(request_id));

        ::v8::Local<::v8::Value> args[5] = {v8_op, v8_input, v8_files, v8_meta, v8_req_id};
        ::v8::Local<::v8::Value> result;
        if (!invoke_fn->Call(context, context->Global(), 5, args).ToLocal(&result)) {
            return MapV8ExceptionToBinary(isolate, context, try_catch.Exception());
        }

        if (result->IsPromise()) {
            auto promise = result.As<::v8::Promise>();
            for (int i = 0; i < 64 && promise->State() == ::v8::Promise::kPending; ++i) {
                isolate->PerformMicrotaskCheckpoint();
            }
            if (promise->State() == ::v8::Promise::kPending) {
                return make_err("DEADLINE_EXCEEDED", "ASYNC_UNSETTLED",
                                "Operation promise did not settle within microtask window");
            }
            if (promise->State() == ::v8::Promise::kRejected) {
                return MapV8ExceptionToBinary(isolate, context, promise->Result());
            }
            result = promise->Result();
        }

        transport::DecodedRpcResult rpc_res;
        rpc_res.ok = true;
        if (!detail::V8ToSerializationValue(isolate, context, result, rpc_res.output, rpc_res.files)) {
            return make_err("SERIALIZATION_FAILED", "OUTPUT_ENCODE_FAILED",
                            "Failed to convert operation output into LCB1 value");
        }

        return transport::BinaryRpcDispatchResult{
            true,
            transport::LcbCodec::EncodeRpcResponse(rpc_res),
            cache_ttl
        };
    }

    static transport::BinaryRpcDispatchResult MapV8ExceptionToBinary(
        ::v8::Isolate* isolate,
        ::v8::Local<::v8::Context> context,
        ::v8::Local<::v8::Value> thrown
    ) {
        transport::DecodedRpcResult res;
        res.ok = false;
        res.error_code = "EXECUTION_FAILED";
        res.error_identifier = "JS_EXECUTION_ERROR";
        res.error_message = "JavaScript operation failed";
        res.retryable = false;

        if (!thrown.IsEmpty() && thrown->IsObject()) {
            auto obj = thrown.As<::v8::Object>();
            auto get_str = [&](const char* key) -> std::string {
                ::v8::Local<::v8::Value> v;
                if (obj->Get(context, ::v8::String::NewFromUtf8(isolate, key).ToLocalChecked()).ToLocal(&v) &&
                    v->IsString()) {
                    ::v8::String::Utf8Value u8(isolate, v);
                    return *u8 ? std::string(*u8, u8.length()) : std::string{};
                }
                return {};
            };
            const auto c = get_str("code");
            const auto id = get_str("identifier");
            const auto msg = get_str("message");
            if (!c.empty()) res.error_code = c;
            if (!id.empty()) res.error_identifier = id;
            if (!msg.empty()) res.error_message = msg;

            ::v8::Local<::v8::Value> rv;
            if (obj->Get(context, ::v8::String::NewFromUtf8Literal(isolate, "retryable")).ToLocal(&rv) &&
                rv->IsBoolean()) {
                res.retryable = rv->BooleanValue(isolate);
            }
        } else if (!thrown.IsEmpty()) {
            ::v8::String::Utf8Value u8(isolate, thrown);
            if (*u8) res.error_message = *u8;
        }

        return transport::BinaryRpcDispatchResult{
            false,
            transport::LcbCodec::EncodeRpcResponse(res),
            std::chrono::milliseconds{0}
        };
    }

    void WorkerMain(std::promise<void>& ready_promise) {
        ::v8::Isolate::CreateParams create_params;
        create_params.array_buffer_allocator =
            ::v8::ArrayBuffer::Allocator::NewDefaultAllocator();

        ::v8::Isolate* isolate = ::v8::Isolate::New(create_params);
        {
            ::v8::Isolate::Scope isolate_scope(isolate);
            ::v8::HandleScope handle_scope(isolate);
            ::v8::Local<::v8::Context> context = ::v8::Context::New(isolate);
            ::v8::Context::Scope context_scope(context);

            auto v8_bridge = std::make_shared<bridge::V8SdkHostBridge>(
                isolate,
                context,
                registry_
            );

            std::string init_error;
            if (!v8_bridge->InstallGlobalBridge()) {
                init_error = "Failed to install globalThis.__lampNativeBridge";
            } else {
                InstallConsoleAndParallel(isolate, context);
                if (!EvalScript(isolate, context, kEmbeddedLampSdkBootstrapJs, "lamp-sdk-bootstrap.js", init_error)) {
                    init_error = "Lamp SDK bootstrap error: " + init_error;
                } else if (!config_.script_source.empty() &&
                           !EvalScript(isolate, context, config_.script_source, config_.script_name, init_error)) {
                    init_error = "Entry script error (" + config_.script_name + "): " + init_error;
                }
            }

            if (!init_error.empty()) {
                try {
                    ready_promise.set_exception(
                        std::make_exception_ptr(std::runtime_error(init_error))
                    );
                } catch (...) {
                }
                v8_bridge.reset();
            } else {
                auto base_dispatcher = std::make_shared<bridge::ExecutionDispatcher>(
                    registry_,
                    v8_bridge,
                    bridge::ExecutionDispatcherConfig{},
                    cache_
                );
                telemetry::TelemetryDispatcher telemetry_dispatcher(
                    std::move(base_dispatcher),
                    metrics_,
                    tracer_
                );

                running_.store(true, std::memory_order_release);
                ready_promise.set_value();

                while (true) {
                    bool has_bin = false;
                    QueuedBinaryTask btask;
                    QueuedTask item;
                    {
                        std::unique_lock lock(queue_mutex_);
                        queue_cv_.wait(lock, [this] {
                            return stopping_ || !binary_queue_.empty() || !queue_.empty();
                        });
                        if (stopping_ && binary_queue_.empty() && queue_.empty()) {
                            break;
                        }
                        if (!binary_queue_.empty()) {
                            btask = std::move(binary_queue_.front());
                            binary_queue_.pop_front();
                            has_bin = true;
                        } else {
                            item = std::move(queue_.front());
                            queue_.pop_front();
                        }
                    }

                    active_tasks_.fetch_add(1, std::memory_order_acq_rel);
                    if (has_bin) {
                        auto out = ExecuteBinaryOnIsolate(isolate, context, *btask.invocation, btask.request_id);
                        btask.promise.set_value(std::move(out));
                    } else if (item.completion->MarkRunning()) {
                        bridge::ResultEnvelope result;
                        if (item.completion->CancellationRequested()) {
                            result = bridge::ResultEnvelope::Cancelled(item.request);
                        } else if (item.request.IsExpired()) {
                            result = bridge::ResultEnvelope::DeadlineExceeded(item.request);
                        } else {
                            result = telemetry_dispatcher.Dispatch(item.request);
                        }
                        (void)item.completion->Complete(std::move(result));
                    }
                    active_tasks_.fetch_sub(1, std::memory_order_acq_rel);
                }

                std::string stop_err;
                (void)EvalScript(
                    isolate,
                    context,
                    "if (typeof globalThis.__lampStopAllApps === 'function') { globalThis.__lampStopAllApps(); }",
                    "lamp-shutdown.js",
                    stop_err
                );
                v8_bridge.reset();
            }
        }

        isolate->Dispose();
        delete create_params.array_buffer_allocator;
    }

    std::shared_ptr<bridge::OperationRegistry> registry_;
    std::shared_ptr<cache::LocalOperationCache> cache_;
    std::shared_ptr<telemetry::MetricsRegistry> metrics_;
    std::shared_ptr<telemetry::Tracer> tracer_;
    LampV8SchedulerConfig config_;
    std::unique_ptr<detail::V8SubtaskIsolatePool> subtask_pool_;
    std::unique_ptr<core::DynamicWorkerPool> subtask_worker_pool_;

    std::atomic<bool> started_{false};
    std::atomic<bool> running_{false};
    std::atomic<std::size_t> active_tasks_{0};

    mutable std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<QueuedTask> queue_;
    std::deque<QueuedBinaryTask> binary_queue_;
    bool stopping_ = false;
    std::thread worker_thread_;
};

} // namespace lamp::host
