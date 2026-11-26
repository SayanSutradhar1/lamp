#!/usr/bin/env node
// ============================================================
// Lamp CLI Runner (`lamp serve <entry.ts|js>`)
// Strips TypeScript types for V8 and launches pure-C++ `lamp_server`.
// ============================================================

import { spawn } from "node:child_process";
import fs from "node:fs";
import { createRequire, stripTypeScriptTypes } from "node:module";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

/**
 * Prepares a user TypeScript or JavaScript backend entry file for execution inside the
 * C++ `lamp_server` embedded V8 isolate by stripping TS type syntax and binding imports
 * from `"lamp"` to `globalThis.Lamp`.
 *
 * @param source - Raw UTF-8 entry file contents.
 * @param filename - Original entry path (used to detect `.ts`/`.mts`/`.cts` extensions).
 * @returns Executable plain JavaScript source evaluated inside V8.
 */
export function prepareEntryForEmbeddedV8(source: string, filename: string): string {
  const isTs = /\.(ts|mts|cts)$/i.test(filename);
  const js = isTs ? stripTypeScriptTypes(source, { mode: "strip" }) : source;

  const rewritten = js.replace(
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

  const routingShim = `
if (typeof globalThis.Lamp?.createLampRegistry !== "function") {
  const __L = globalThis.Lamp;
  const __LampError = __L.LampError;
  const __segmentRe = /^[a-zA-Z0-9_-]+$/;
  const __targetRe = /^[a-zA-Z0-9_-]+(\\.[a-zA-Z0-9_-]+)*$/;
  __L.createLampRegistry = function createLampRegistry() {
    const entriesList = [];
    const seen = new Set();
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
      entries() { return [...entriesList]; },
    };
    return reg;
  };
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
    });
    const registries = toArr(config.registry);
    const nested = toArr(config.nesting);
    const routerOptions = {
      execution: config.options?.execution ? { ...config.options.execution } : undefined,
      cache: config.options?.cache,
      auth: config.options?.auth ? { ...config.options.auth } : undefined,
      tags: config.options?.tags ? [...config.options.tags] : undefined,
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
              for (const entry of reg.entries()) {
                const operationId = \`\${curPath}.\${entry.suffix}\`;
                if (seenIds.has(operationId)) {
                  throw new __LampError(\`Duplicate operation "\${operationId}"\`, { code: "ALREADY_EXISTS", identifier: "DUPLICATE_OPERATION" });
                }
                seenIds.add(operationId);
                resolved.push({
                  operationId,
                  definition: {
                    ...entry.definition,
                    execution: effOpts.execution || entry.definition.execution ? { ...(effOpts.execution || {}), ...(entry.definition.execution || {}) } : undefined,
                    cache: entry.definition.cache !== undefined ? entry.definition.cache : effOpts.cache,
                    auth: effOpts.auth || entry.definition.auth ? { ...(effOpts.auth || {}), ...(entry.definition.auth || {}) } : undefined,
                    tags: mergeTags(effOpts.tags, entry.definition.tags),
                  },
                });
              }
            }
            for (const child of cur.nested || []) { walk(child, curPath, effOpts); }
          } finally { stack.delete(cur); }
        }
        walk(router, mountOptions.prefix || "", { execution: mountOptions.execution, cache: mountOptions.cache, auth: mountOptions.auth, tags: mountOptions.tags });
        return resolved;
      },
    };
    return router;
  };
  const __origCreateApp = __L.createLampApp;
  __L.createLampApp = function createLampApp(config) {
    const app = __origCreateApp(config);
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

  return `(async () => {\n${routingShim}\n${rewritten}\n})()`;
}

/**
 * Resolves the filesystem path to the compiled C++ `lamp_server` native binary.
 *
 * Resolution order:
 * 1. `process.env.LAMP_SERVER_BIN`
 * 2. Installed platform package (`@lamp/<platform>-<arch>/bin/lamp_server(.exe)`)
 * 3. Local package `bin/` directory
 * 4. Repository CMake build directories (for local development)
 *
 * @returns Executable path to `lamp_server`.
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

function main(): void {
  const args = process.argv.slice(2);
  if (args.length === 0 || args[0] === "--help" || args[0] === "-h") {
    console.log(
      "Usage: lamp serve <entry.ts|entry.js> [--host 127.0.0.1] [--port 3000] " +
        "[--min-threads 2] [--max-threads 16] [--max-cache-ttl-ms 300000] [--max-cache-entries 4096]"
    );
    process.exit(0);
  }

  const cmd = args[0];
  if (cmd !== "serve") {
    console.error(`Unknown subcommand "${cmd}". Expected "serve".`);
    process.exit(2);
  }

  const entryArg = args[1];
  if (!entryArg) {
    console.error("Missing <entry.ts|entry.js> file path.");
    process.exit(2);
  }

  let host = "127.0.0.1";
  let port = "3000";
  const passThroughFlags: string[] = [];

  for (let i = 2; i < args.length; i += 1) {
    const flag = args[i];
    if (flag === "--host" && i + 1 < args.length) {
      host = args[++i];
    } else if (flag === "--port" && i + 1 < args.length) {
      port = args[++i];
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

  const rawSource = fs.readFileSync(resolvedEntry, "utf8");
  const preparedJs = prepareEntryForEmbeddedV8(rawSource, resolvedEntry);

  const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), "lamp-v8-"));
  const compiledEntry = path.join(tmpDir, "entry.v8.js");
  fs.writeFileSync(compiledEntry, preparedJs, "utf8");

  const child = spawn(
    binPath,
    ["--entry", compiledEntry, "--host", host, "--port", port, ...passThroughFlags],
    { stdio: "inherit" }
  );

  const cleanup = () => {
    try {
      fs.rmSync(tmpDir, { recursive: true, force: true });
    } catch {
      // Ignore temporary cleanup failures on Windows handle release
    }
  };

  process.on("SIGINT", () => child.kill("SIGINT"));
  process.on("SIGTERM", () => child.kill("SIGTERM"));

  child.on("error", (err) => {
    cleanup();
    console.error(`Failed to launch lamp_server (${binPath}): ${err.message}`);
    process.exit(1);
  });

  child.on("exit", (code) => {
    cleanup();
    process.exit(code ?? 0);
  });
}

const currentFile = fs.realpathSync(fileURLToPath(import.meta.url));
const invokedFile = process.argv[1] ? fs.realpathSync(process.argv[1]) : "";
if (invokedFile === currentFile) {
  main();
}
