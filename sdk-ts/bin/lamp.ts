#!/usr/bin/env node
// ============================================================
// Lamp CLI Runner (`lamp serve <entry.ts|js>`)
// Strips TypeScript types for V8 and launches pure-C++ `lamp_server`.
// ============================================================

import { spawn } from "node:child_process";
import fs from "node:fs";
import { stripTypeScriptTypes } from "node:module";
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
    /^\s*import\s+\{([^}]+)\}\s+from\s+["'](?:\.\.?\/[^"']*index\.(?:ts|js)|lamp)["'];?\s*$/gm,
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

  return `(async () => {\n${rewritten}\n})()`;
}

/**
 * Resolves the filesystem path to the compiled C++ `lamp_server` native binary.
 *
 * Honors `process.env.LAMP_SERVER_BIN` first before checking repository build directories.
 *
 * @returns Executable path or binary name when relying on `PATH`.
 */
export function resolveLampServerBinary(): string {
  if (process.env.LAMP_SERVER_BIN && fs.existsSync(process.env.LAMP_SERVER_BIN)) {
    return process.env.LAMP_SERVER_BIN;
  }

  const binDir = path.dirname(fileURLToPath(import.meta.url));
  const repoRoot = path.resolve(binDir, "..", "..");
  const exeName = process.platform === "win32" ? "lamp_server.exe" : "lamp_server";

  const candidates = [
    path.join(repoRoot, "build-msvc-v8", "host", exeName),
    path.join(repoRoot, "build-msvc-v8", "host", "Release", exeName),
    path.join(repoRoot, "build", "host", exeName),
    path.join(repoRoot, "build-ci", "host", exeName),
  ];

  for (const c of candidates) {
    if (fs.existsSync(c)) {
      return c;
    }
  }
  return exeName;
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

  const rawSource = fs.readFileSync(resolvedEntry, "utf8");
  const preparedJs = prepareEntryForEmbeddedV8(rawSource, resolvedEntry);

  const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), "lamp-v8-"));
  const compiledEntry = path.join(tmpDir, "entry.v8.js");
  fs.writeFileSync(compiledEntry, preparedJs, "utf8");

  const binPath = resolveLampServerBinary();
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
