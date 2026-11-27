#!/usr/bin/env node
// ============================================================
// Lamp CLI Runner
//   lamp serve <entry.ts|js> [--port 3000] [--watch]
//   lamp build [src/app.ts] [--out dist/app.js]
//   lamp add <module[/submodule]>
// ============================================================

import { spawn, type ChildProcess } from "node:child_process";
import fs from "node:fs";
import { createRequire, stripTypeScriptTypes } from "node:module";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const LAMP_RUNTIME_SHIM = `
if (typeof globalThis.Lamp?.createLampRegistry !== "function" || typeof globalThis.Lamp?.createErrorHandler !== "function") {
  const __L = globalThis.Lamp;
  const __LampError = __L.LampError;
  const __isLampError = __L.isLampError;
  const __segmentRe = /^[a-zA-Z0-9_-]+$/;
  const __targetRe = /^[a-zA-Z0-9_-]+(\\.[a-zA-Z0-9_-]+)*$/;

  if (typeof __L.createErrorHandler !== "function") {
    __L.createErrorHandler = function createErrorHandler(config = {}) {
      const shouldLog = config.log ?? true;
      const includeStack = config.includeStack ?? false;
      const defaultCode = config.defaultCode ?? "EXECUTION_FAILED";
      return async (err, ctx) => {
        let normalized = __isLampError(err)
          ? err
          : err instanceof Error
            ? new __LampError(err.message, {
                code: defaultCode,
                identifier: err.name || "JS_EXCEPTION",
                cause: err,
                details: includeStack && err.stack ? { stack: err.stack } : undefined,
              })
            : new __LampError("Operation execution failed with non-Error value", {
                code: defaultCode,
                identifier: "NON_ERROR_THROW",
              });
        if (config.transform) {
          const transformed = config.transform(normalized, ctx);
          if (transformed) normalized = transformed;
        }
        if (shouldLog) {
          console.error(\`[lamp:error] \${ctx.operationId} [\${normalized.code}/\${normalized.identifier}]: \${normalized.message}\`);
        }
        if (config.onError) {
          await config.onError(normalized, ctx);
        }
        return normalized;
      };
    };
  }

  if (typeof __L.createLampRegistry !== "function") {
    __L.createLampRegistry = function createLampRegistry(config = {}) {
      const entriesList = [];
      const seen = new Set();
      let registryErrorHandler = config.onError;
      const reg = {
        add(suffix, def) {
          if (typeof suffix !== "string" || !__segmentRe.test(suffix)) {
            throw new __LampError(\`Invalid operation suffix "\${String(suffix)}"\`, { code: "INVALID_ARGUMENT", identifier: "INVALID_OPERATION_SUFFIX" });
          }
          if (seen.has(suffix)) {
            throw new __LampError(\`Duplicate operation suffix "\${suffix}" in registry\`, { code: "ALREADY_EXISTS", identifier: "DUPLICATE_OPERATION" });
          }
          if (!def || typeof def.handler !== "function") {
            throw new __LampError(\`Operation "\${suffix}" requires a handler function\`, { code: "INVALID_ARGUMENT", identifier: "MISSING_HANDLER" });
          }
          seen.add(suffix);
          entriesList.push({ suffix, definition: def });
          return reg;
        },
        onError(handler) { registryErrorHandler = handler; return reg; },
        errorHandler() { return registryErrorHandler; },
        entries() { return [...entriesList]; },
      };
      return reg;
    };
  }

  if (typeof __L.createLampRouter !== "function") {
    __L.createLampRouter = function createLampRouter(config) {
      if (!config || typeof config.target !== "string" || !__targetRe.test(config.target)) {
        throw new __LampError(\`Invalid router target "\${String(config && config.target)}"\`, { code: "INVALID_ARGUMENT", identifier: "INVALID_ROUTER_TARGET" });
      }
      const toArr = (v) => (v === undefined ? [] : Array.isArray(v) ? [...v] : [v]);
      const mergeTags = (a, b) => (!a?.length && !b?.length ? undefined : Array.from(new Set([...(a || []), ...(b || [])])));
      const mergeOpts = (p, c) => ({
        execution: p.execution || c.execution ? { ...(p.execution || {}), ...(c.execution || {}) } : undefined,
        cache: c.cache !== undefined ? c.cache : p.cache,
        auth: p.auth || c.auth ? { ...(p.auth || {}), ...(c.auth || {}) } : undefined,
        tags: mergeTags(p.tags, c.tags),
        onError: c.onError ?? p.onError,
      });
      const registries = toArr(config.registry);
      const nested = toArr(config.nesting);
      const routerOptions = {
        execution: config.options?.execution ? { ...config.options.execution } : undefined,
        cache: config.options?.cache,
        auth: config.options?.auth ? { ...config.options.auth } : undefined,
        tags: config.options?.tags ? [...config.options.tags] : undefined,
        onError: config.onError ?? config.options?.onError,
      };
      const router = {
        description: config.description,
        target: config.target,
        options: routerOptions,
        registries,
        nested,
        resolveOperations(mountOptions = {}) {
          if (mountOptions.prefix !== undefined && !__targetRe.test(mountOptions.prefix)) {
            throw new __LampError(\`Invalid router mount prefix "\${mountOptions.prefix}"\`, { code: "INVALID_ARGUMENT", identifier: "INVALID_ROUTER_PREFIX" });
          }
          const resolved = [];
          const seenIds = new Set();
          const stack = new Set();
          function walk(cur, parentPath, inheritedOpts) {
            if (stack.has(cur)) {
              throw new __LampError(\`Circular router nesting detected at target "\${cur.target}"\`, { code: "INVALID_ARGUMENT", identifier: "CIRCULAR_ROUTER_NESTING" });
            }
            stack.add(cur);
            try {
              const curPath = parentPath ? \`\${parentPath}.\${cur.target}\` : cur.target;
              const effOpts = mergeOpts(inheritedOpts, cur.options || {});
              for (const reg of cur.registries || []) {
                const regErr = typeof reg.errorHandler === "function" ? reg.errorHandler() : undefined;
                for (const entry of reg.entries()) {
                  const operationId = \`\${curPath}.\${entry.suffix}\`;
                  if (seenIds.has(operationId)) {
                    throw new __LampError(\`Duplicate operation "\${operationId}"\`, { code: "ALREADY_EXISTS", identifier: "DUPLICATE_OPERATION" });
                  }
                  seenIds.add(operationId);
                  const effErr = regErr ?? effOpts.onError;
                  const origHandler = entry.definition.handler;
                  const wrappedHandler = effErr
                    ? async (args) => {
                        try { return await origHandler(args); }
                        catch (err) { throw await effErr(err, { operationId, requestId: args.ctx?.requestId }); }
                      }
                    : origHandler;
                  resolved.push({
                    operationId,
                    definition: {
                      ...entry.definition,
                      execution: effOpts.execution || entry.definition.execution ? { ...(effOpts.execution || {}), ...(entry.definition.execution || {}) } : undefined,
                      cache: entry.definition.cache !== undefined ? entry.definition.cache : effOpts.cache,
                      auth: effOpts.auth || entry.definition.auth ? { ...(effOpts.auth || {}), ...(entry.definition.auth || {}) } : undefined,
                      tags: mergeTags(effOpts.tags, entry.definition.tags),
                      handler: wrappedHandler,
                    },
                  });
                }
              }
              for (const child of cur.nested || []) { walk(child, curPath, effOpts); }
            } finally { stack.delete(cur); }
          }
          walk(router, mountOptions.prefix || "", {
            execution: mountOptions.execution,
            cache: mountOptions.cache,
            auth: mountOptions.auth,
            tags: mountOptions.tags,
            onError: mountOptions.onError,
          });
          return resolved;
        },
      };
      return router;
    };
  }

  const __origCreateApp = __L.createLampApp;
  __L.createLampApp = function createLampApp(config = {}) {
    const app = __origCreateApp(config);
    const shouldLog = config.log ?? true;
    const __origRegister = app.register.bind(app);
    app.register = function register(operationId, definition) {
      const wrappedDef = shouldLog
        ? {
            ...definition,
            handler: (args) => {
              console.log(\`[lamp] -> \${operationId} (#\${args.ctx?.requestId ?? 0})\`);
              return definition.handler(args);
            },
          }
        : definition;
      const res = __origRegister(operationId, wrappedDef);
      if (shouldLog) {
        console.log(\`[lamp] + registered \${operationId}\`);
      }
      return res;
    };
    if (typeof app.registerRouter !== "function") {
      app.registerRouter = function registerRouter(router, options) {
        if (app.state() !== "created") {
          throw new __LampError(\`Cannot register router while application state is "\${app.state()}"\`, { code: "FAILED_PRECONDITION", identifier: "APP_ALREADY_STARTED" });
        }
        for (const op of router.resolveOperations(options)) {
          app.register(op.operationId, op.definition);
        }
        return app;
      };
    }
    return app;
  };
}
`;

function rewriteLampImports(js: string): string {
  return js.replace(
    /^\s*import\s+\{([^}]+)\}\s+from\s+["'](?:\.\.?\/[^"']*index\.(?:ts|js)|lamp(?:-app)?)["'];?\s*$/gm,
    (_full, imports: string) => {
      const cleaned = imports
        .split(",")
        .map((part) => part.trim())
        .filter((part) => part.length > 0 && !part.startsWith("type "))
        .map((part) => part.replace(/\s+as\s+/g, ": "))
        .join(", ");
      return cleaned.length > 0 ? `const { ${cleaned} } = globalThis.Lamp;` : "";
    }
  );
}

/**
 * Prepares a single-file TypeScript or JavaScript backend entry file for execution inside the
 * C++ `lamp_server` embedded V8 isolate.
 */
export function prepareEntryForEmbeddedV8(source: string, filename: string): string {
  const isTs = /\.(ts|mts|cts)$/i.test(filename);
  const js = isTs ? stripTypeScriptTypes(source, { mode: "strip" }) : source;
  const rewritten = rewriteLampImports(js);
  return `(async () => {\n${LAMP_RUNTIME_SHIM}\n${rewritten}\n})()`;
}

function resolveRelativeModule(fromFile: string, specifier: string): string {
  const baseDir = path.dirname(fromFile);
  const rawTarget = path.resolve(baseDir, specifier);
  const candidates = [
    rawTarget,
    `${rawTarget}.ts`,
    `${rawTarget}.mts`,
    `${rawTarget}.js`,
    `${rawTarget}.mjs`,
    path.join(rawTarget, "index.ts"),
    path.join(rawTarget, "index.js"),
  ];
  for (const candidate of candidates) {
    if (fs.existsSync(candidate) && fs.statSync(candidate).isFile()) {
      return fs.realpathSync(candidate);
    }
  }
  throw new Error(`Cannot resolve relative module "${specifier}" imported from "${fromFile}"`);
}

function transformModuleExports(js: string): { code: string; exportNames: string[]; hasDefault: boolean } {
  const exportNames = new Set<string>();
  let hasDefault = false;

  let code = js
    .replace(/^\s*export\s+default\s+/gm, () => {
      hasDefault = true;
      return "const __lamp_default_export = ";
    })
    .replace(/^\s*export\s+(const|let|var)\s+([a-zA-Z0-9_$]+)/gm, (_m, kind: string, name: string) => {
      exportNames.add(name);
      return `${kind} ${name}`;
    })
    .replace(/^\s*export\s+(async\s+)?function\s+([a-zA-Z0-9_$]+)/gm, (_m, asyncKw: string | undefined, name: string) => {
      exportNames.add(name);
      return `${asyncKw ?? ""}function ${name}`;
    })
    .replace(/^\s*export\s+class\s+([a-zA-Z0-9_$]+)/gm, (_m, name: string) => {
      exportNames.add(name);
      return `class ${name}`;
    })
    .replace(/^\s*export\s*\{([^}]+)\};?\s*$/gm, (_m, list: string) => {
      const mappings: string[] = [];
      for (const item of list.split(",")) {
        const trimmed = item.trim();
        if (!trimmed || trimmed.startsWith("type ")) continue;
        const asMatch = trimmed.match(/^([a-zA-Z0-9_$]+)\s+as\s+([a-zA-Z0-9_$]+)$/);
        if (asMatch) {
          mappings.push(`__lamp_extra_exports.${asMatch[2]} = ${asMatch[1]};`);
        } else {
          exportNames.add(trimmed);
        }
      }
      return mappings.join(" ");
    });

  return { code, exportNames: Array.from(exportNames), hasDefault };
}

/**
 * Bundles a multi-file TypeScript/JavaScript Lamp project into a single dependency-optimized
 * low-level JavaScript bundle executable directly by the C++ `lamp_server` V8 runtime.
 */
export function bundleEntryForEmbeddedV8(entryFilePath: string): string {
  const absEntry = fs.realpathSync(path.resolve(entryFilePath));
  const rawEntry = fs.readFileSync(absEntry, "utf8");
  if (rawEntry.startsWith("// @lamp-bundle")) {
    return rawEntry;
  }

  const moduleIds = new Map<string, number>();
  const orderedFiles: string[] = [];
  const visiting = new Set<string>();
  const visited = new Set<string>();
  let nextId = 0;

  function collectDeps(filePath: string): void {
    if (visited.has(filePath)) return;
    if (visiting.has(filePath)) {
      throw new Error(`Circular module import detected at "${filePath}"`);
    }
    visiting.add(filePath);
    const src = fs.readFileSync(filePath, "utf8");
    const isTs = /\.(ts|mts|cts)$/i.test(filePath);
    const stripped = isTs ? stripTypeScriptTypes(src, { mode: "strip" }) : src;
    const importRe = /^\s*import\s+(?:[\s\S]*?\s+from\s+)?["'](\.[^"']+)["'];?\s*$/gm;
    let match: RegExpExecArray | null;
    while ((match = importRe.exec(stripped)) !== null) {
      const depPath = resolveRelativeModule(filePath, match[1]);
      collectDeps(depPath);
    }
    visiting.delete(filePath);
    visited.add(filePath);
    moduleIds.set(filePath, nextId++);
    orderedFiles.push(filePath);
  }

  collectDeps(absEntry);

  const chunks: string[] = [];
  for (const filePath of orderedFiles) {
    const src = fs.readFileSync(filePath, "utf8");
    const isTs = /\.(ts|mts|cts)$/i.test(filePath);
    const stripped = isTs ? stripTypeScriptTypes(src, { mode: "strip" }) : src;
    let js = rewriteLampImports(stripped);

    // Rewrite relative imports to destructure from __lamp_mod_<id>
    js = js.replace(
      /^\s*import\s+\{([^}]+)\}\s+from\s+["'](\.[^"']+)["'];?\s*$/gm,
      (_m, specifiers: string, rel: string) => {
        const depId = moduleIds.get(resolveRelativeModule(filePath, rel));
        const cleaned = specifiers
          .split(",")
          .map((s) => s.trim())
          .filter((s) => s.length > 0 && !s.startsWith("type "))
          .map((s) => s.replace(/\s+as\s+/g, ": "))
          .join(", ");
        return cleaned ? `const { ${cleaned} } = __lamp_mod_${depId};` : "";
      }
    );
    js = js.replace(
      /^\s*import\s+\*\s+as\s+([a-zA-Z0-9_$]+)\s+from\s+["'](\.[^"']+)["'];?\s*$/gm,
      (_m, alias: string, rel: string) => {
        const depId = moduleIds.get(resolveRelativeModule(filePath, rel));
        return `const ${alias} = __lamp_mod_${depId};`;
      }
    );
    js = js.replace(
      /^\s*import\s+([a-zA-Z0-9_$]+)\s+from\s+["'](\.[^"']+)["'];?\s*$/gm,
      (_m, defName: string, rel: string) => {
        const depId = moduleIds.get(resolveRelativeModule(filePath, rel));
        return `const ${defName} = __lamp_mod_${depId}.default;`;
      }
    );
    js = js.replace(
      /^\s*import\s+["'](\.[^"']+)["'];?\s*$/gm,
      ""
    );

    const modId = moduleIds.get(filePath)!;
    const isEntry = filePath === absEntry;
    if (isEntry) {
      chunks.push(`// entry: ${path.basename(filePath)}\n${js}`);
    } else {
      const { code, exportNames, hasDefault } = transformModuleExports(js);
      const returnFields = [...exportNames];
      if (hasDefault) returnFields.push("default: __lamp_default_export");
      chunks.push(
        `// module: ${path.relative(path.dirname(absEntry), filePath).replace(/\\/g, "/")}\n` +
          `const __lamp_mod_${modId} = (() => {\nconst __lamp_extra_exports = {};\n${code}\n` +
          `return { ${returnFields.join(", ")}, ...__lamp_extra_exports };\n})();`
      );
    }
  }

  return `// @lamp-bundle\n(async () => {\n${LAMP_RUNTIME_SHIM}\n${chunks.join("\n\n")}\n})()\n`;
}

/**
 * Resolves the filesystem path to the compiled C++ `lamp_server` native binary.
 */
export function resolveLampServerBinary(): string {
  if (process.env.LAMP_SERVER_BIN && fs.existsSync(process.env.LAMP_SERVER_BIN)) {
    return process.env.LAMP_SERVER_BIN;
  }

  const exeName = process.platform === "win32" ? "lamp_server.exe" : "lamp_server";
  const platformPkg = `@sayansutradhar1/lamp-app-${process.platform}-${process.arch}`;

  try {
    const require = createRequire(import.meta.url);
    const pkgJsonPath = require.resolve(`${platformPkg}/package.json`);
    const pkgBinary = path.join(path.dirname(pkgJsonPath), "bin", exeName);
    if (fs.existsSync(pkgBinary)) {
      return pkgBinary;
    }
  } catch {
    // Platform optionalDependency is not installed; fall through to local candidates.
  }

  const binDir = path.dirname(fileURLToPath(import.meta.url));
  const pkgRoots = [
    path.resolve(binDir, ".."),
    path.resolve(binDir, "..", ".."),
  ];
  for (const root of pkgRoots) {
    const localBin = path.join(root, "bin", exeName);
    if (fs.existsSync(localBin) && localBin !== fileURLToPath(import.meta.url)) {
      return localBin;
    }
  }

  const repoRoots = [
    path.resolve(binDir, "..", ".."),
    path.resolve(binDir, "..", "..", ".."),
  ];
  const candidates: string[] = [];
  for (const repoRoot of repoRoots) {
    candidates.push(
      path.join(repoRoot, "build-msvc-v8", "host", exeName),
      path.join(repoRoot, "build-msvc-v8", "host", "Release", exeName),
      path.join(repoRoot, "build", "host", exeName),
      path.join(repoRoot, "build", "host", "Release", exeName),
      path.join(repoRoot, "build-ci", "host", exeName),
      path.join(repoRoot, "build-ci", "host", "Release", exeName)
    );
  }

  for (const c of candidates) {
    if (fs.existsSync(c)) {
      return c;
    }
  }

  throw new Error(
    `Unable to locate native V8 runtime binary (${exeName}) for ${process.platform}-${process.arch}. ` +
      `Install "${platformPkg}" or set LAMP_SERVER_BIN to a compiled lamp_server executable.`
  );
}

function toCamelCase(raw: string): string {
  return raw.replace(/[-_]+([a-zA-Z0-9])/g, (_m, c: string) => c.toUpperCase());
}

function insertImportAndNesting(
  parentRouterPath: string,
  importPath: string,
  routerVarName: string
): void {
  let content = fs.readFileSync(parentRouterPath, "utf8");
  const importLine = `import { ${routerVarName} } from "${importPath}";`;

  if (!content.includes(importLine)) {
    const lastImportIdx = content.lastIndexOf("import ");
    if (lastImportIdx >= 0) {
      const lineEnd = content.indexOf("\n", lastImportIdx);
      const insertAt = lineEnd >= 0 ? lineEnd + 1 : content.length;
      content = content.slice(0, insertAt) + importLine + "\n" + content.slice(insertAt);
    } else {
      content = `${importLine}\n${content}`;
    }
  }

  const nestingRe = /nesting\s*:\s*\[([^\]]*)\]/;
  if (nestingRe.test(content)) {
    content = content.replace(nestingRe, (_m, inner: string) => {
      const items = inner
        .split(",")
        .map((s) => s.trim())
        .filter(Boolean);
      if (!items.includes(routerVarName)) {
        items.push(routerVarName);
      }
      return `nesting: [${items.join(", ")}]`;
    });
  } else {
    content = content.replace(
      /createLampRouter\(\s*\{/,
      `createLampRouter({\n  nesting: [${routerVarName}],`
    );
  }

  fs.writeFileSync(parentRouterPath, content, "utf8");
}

function runAddCommand(moduleArg: string | undefined): void {
  if (!moduleArg) {
    console.error("Usage: lamp add <moduleName> (e.g. \"user\", \"admin\", or \"user/profile\")");
    process.exit(2);
  }

  const segments = moduleArg
    .split(/[./\\]+/)
    .map((s) => s.trim())
    .filter(Boolean);

  if (segments.length === 0 || segments.some((s) => !/^[a-zA-Z0-9_-]+$/.test(s))) {
    console.error(`Invalid module name "${moduleArg}". Use alphanumeric, "-" or "_" segments.`);
    process.exit(2);
  }

  const cwd = process.cwd();
  const srcDir = path.join(cwd, "src");
  const rootRouterPath = path.join(srcDir, "router.ts");
  if (!fs.existsSync(rootRouterPath)) {
    console.error(`Root router not found at "${rootRouterPath}". Run inside a Lamp project root.`);
    process.exit(1);
  }

  const moduleName = segments[segments.length - 1];
  const camelName = toCamelCase(moduleName);
  const registryVar = `${camelName}Registry`;
  const routerVar = `${camelName}Router`;

  const moduleDir = path.join(srcDir, "modules", ...segments);
  const registryFile = path.join(moduleDir, `${moduleName}.registry.ts`);
  const routerFile = path.join(moduleDir, `${moduleName}.router.ts`);

  if (fs.existsSync(routerFile) || fs.existsSync(registryFile)) {
    console.error(`Module "${segments.join("/")}" already exists at ${moduleDir}`);
    process.exit(1);
  }

  let parentRouterPath: string;
  let importRelPath: string;
  if (segments.length === 1) {
    parentRouterPath = rootRouterPath;
    importRelPath = `./modules/${moduleName}/${moduleName}.router.ts`;
  } else {
    const parentSegments = segments.slice(0, -1);
    const parentName = parentSegments[parentSegments.length - 1];
    parentRouterPath = path.join(srcDir, "modules", ...parentSegments, `${parentName}.router.ts`);
    if (!fs.existsSync(parentRouterPath)) {
      console.error(
        `Parent module "${parentSegments.join("/")}" does not exist. Create it first with "npx lamp add ${parentSegments.join("/")}".`
      );
      process.exit(1);
    }
    importRelPath = `./${moduleName}/${moduleName}.router.ts`;
  }

  fs.mkdirSync(moduleDir, { recursive: true });

  const registryContent = `import { LampError, createErrorHandler, createLampRegistry, s } from "lamp-app";

const ${camelName}ErrorHandler = createErrorHandler({
  log: true,
});

export const ${registryVar} = createLampRegistry({ onError: ${camelName}ErrorHandler })
  .add("get", {
    description: "Fetch ${moduleName} record by ID",
    input: s.object({ id: s.string({ minLength: 1 }) }),
    output: s.object({ id: s.string(), module: s.string() }),
    handler: async ({ input }) => {
      if (input.id === "missing") {
        throw new LampError(\`${moduleName} "\${input.id}" not found\`, {
          code: "NOT_FOUND",
          identifier: "${moduleName.toUpperCase()}_NOT_FOUND",
        });
      }
      return { id: input.id, module: "${moduleName}" };
    },
  });
`;

  const routerContent = `import { createLampRouter } from "lamp-app";
import { ${registryVar} } from "./${moduleName}.registry.ts";

export const ${routerVar} = createLampRouter({
  description: "${moduleName} route module",
  target: "${moduleName}",
  registry: [${registryVar}],
  nesting: [],
});
`;

  fs.writeFileSync(registryFile, registryContent, "utf8");
  fs.writeFileSync(routerFile, routerContent, "utf8");
  insertImportAndNesting(parentRouterPath, importRelPath, routerVar);

  console.log(`Created module "${segments.join("/")}":`);
  console.log(`  - ${path.relative(cwd, registryFile)}`);
  console.log(`  - ${path.relative(cwd, routerFile)}`);
  console.log(`Auto-registered ${routerVar} in ${path.relative(cwd, parentRouterPath)}`);
}

function runBuildCommand(args: string[]): void {
  let entryArg = "src/app.ts";
  let outArg = "dist/app.js";

  for (let i = 0; i < args.length; i += 1) {
    const arg = args[i];
    if (arg === "--out" || arg === "-o") {
      if (i + 1 < args.length) {
        outArg = args[++i];
      }
    } else if (!arg.startsWith("-")) {
      entryArg = arg;
    } else {
      console.error(`Unknown build flag: ${arg}`);
      process.exit(2);
    }
  }

  const resolvedEntry = path.resolve(entryArg);
  if (!fs.existsSync(resolvedEntry)) {
    console.error(`Entry file not found: ${resolvedEntry}`);
    process.exit(1);
  }

  const bundled = bundleEntryForEmbeddedV8(resolvedEntry);
  const resolvedOut = path.resolve(outArg);
  fs.mkdirSync(path.dirname(resolvedOut), { recursive: true });
  fs.writeFileSync(resolvedOut, bundled, "utf8");
  console.log(`Built optimized Lamp bundle: ${path.relative(process.cwd(), resolvedOut)} (${Buffer.byteLength(bundled)} B)`);
}

function runServeCommand(args: string[]): void {
  const entryArg = args[0];
  if (!entryArg) {
    console.error("Missing <entry.ts|entry.js> file path.");
    process.exit(2);
  }

  let host = "127.0.0.1";
  let port = "3000";
  let watch = false;
  const passThroughFlags: string[] = [];

  for (let i = 1; i < args.length; i += 1) {
    const flag = args[i];
    if (flag === "--host" && i + 1 < args.length) {
      host = args[++i];
    } else if (flag === "--port" && i + 1 < args.length) {
      port = args[++i];
    } else if (flag === "--watch" || flag === "-w") {
      watch = true;
    } else if (
      (flag === "--min-threads" ||
        flag === "--max-threads" ||
        flag === "--max-cache-ttl-ms" ||
        flag === "--max-cache-entries") &&
      i + 1 < args.length
    ) {
      passThroughFlags.push(flag, args[++i]);
    } else {
      console.error(`Unknown flag: ${flag}`);
      process.exit(2);
    }
  }

  const resolvedEntry = path.resolve(entryArg);
  if (!fs.existsSync(resolvedEntry)) {
    console.error(`Entry file not found: ${resolvedEntry}`);
    process.exit(1);
  }

  let binPath: string;
  try {
    binPath = resolveLampServerBinary();
  } catch (err) {
    console.error(err instanceof Error ? err.message : String(err));
    process.exit(1);
  }

  const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), "lamp-v8-"));
  const compiledEntry = path.join(tmpDir, "entry.v8.js");

  const cleanup = () => {
    try {
      fs.rmSync(tmpDir, { recursive: true, force: true });
    } catch {
      // Ignore temporary cleanup failures on Windows handle release
    }
  };

  let activeChild: ChildProcess | null = null;
  let shuttingDown = false;
  let restarting = false;

  const startServerProcess = () => {
    const bundledJs = bundleEntryForEmbeddedV8(resolvedEntry);
    fs.writeFileSync(compiledEntry, bundledJs, "utf8");

    const child = spawn(
      binPath,
      ["--entry", compiledEntry, "--host", host, "--port", port, ...passThroughFlags],
      { stdio: "inherit" }
    );
    activeChild = child;

    child.on("error", (err) => {
      if (!watch) {
        cleanup();
        console.error(`Failed to launch lamp_server (${binPath}): ${err.message}`);
        process.exit(1);
      }
      console.error(`[lamp:watch] failed to start lamp_server: ${err.message}`);
    });

    child.on("exit", (code) => {
      if (restarting) {
        restarting = false;
        if (!shuttingDown) {
          startServerProcess();
        }
        return;
      }
      if (!watch || shuttingDown) {
        cleanup();
        process.exit(code ?? 0);
      }
    });
  };

  startServerProcess();

  let watcher: fs.FSWatcher | null = null;
  let debounceTimer: ReturnType<typeof setTimeout> | null = null;

  if (watch) {
    const watchDir = path.dirname(resolvedEntry);
    console.log(`[lamp:watch] watching ${watchDir} for changes...`);
    watcher = fs.watch(watchDir, { recursive: true }, (_event, filename) => {
      if (!filename || !/\.(ts|js|mts|cts)$/i.test(filename)) return;
      if (debounceTimer) clearTimeout(debounceTimer);
      debounceTimer = setTimeout(() => {
        console.log(`[lamp:watch] change detected (${filename}), restarting...`);
        if (activeChild && activeChild.exitCode === null) {
          restarting = true;
          activeChild.kill("SIGTERM");
        } else {
          startServerProcess();
        }
      }, 150);
    });
  }

  const shutdown = () => {
    shuttingDown = true;
    if (debounceTimer) clearTimeout(debounceTimer);
    watcher?.close();
    if (activeChild && activeChild.exitCode === null) {
      activeChild.kill("SIGINT");
    } else {
      cleanup();
      process.exit(0);
    }
  };

  process.on("SIGINT", shutdown);
  process.on("SIGTERM", shutdown);
}

function main(): void {
  const args = process.argv.slice(2);
  if (args.length === 0 || args[0] === "--help" || args[0] === "-h") {
    console.log(
      "Usage:\n" +
        "  lamp serve <entry.ts|entry.js> [--host 127.0.0.1] [--port 3000] [--watch|-w] " +
        "[--min-threads 2] [--max-threads 16] [--max-cache-ttl-ms 300000] [--max-cache-entries 4096]\n" +
        "  lamp build [src/app.ts] [--out dist/app.js]\n" +
        "  lamp add <moduleName>  (e.g. user, admin, user/profile)"
    );
    process.exit(0);
  }

  const cmd = args[0];
  if (cmd === "serve") {
    runServeCommand(args.slice(1));
  } else if (cmd === "build") {
    runBuildCommand(args.slice(1));
  } else if (cmd === "add") {
    runAddCommand(args[1]);
  } else {
    console.error(`Unknown subcommand "${cmd}". Expected "serve", "build", or "add".`);
    process.exit(2);
  }
}

const currentFile = fs.realpathSync(fileURLToPath(import.meta.url));
const invokedFile = process.argv[1] ? fs.realpathSync(process.argv[1]) : "";
if (invokedFile === currentFile) {
  main();
}
