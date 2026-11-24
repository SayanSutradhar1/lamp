#!/usr/bin/env node
/**
 * Stages a compiled `lamp_server` binary into `npm/<platform>-<arch>/bin/`.
 *
 * Usage:
 *   node npm/stage-binary.mjs <platform-arch> <path-to-lamp_server>
 * Example:
 *   node npm/stage-binary.mjs win32-x64 build-msvc-v8/host/lamp_server.exe
 */

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const [target, sourceBin] = process.argv.slice(2);
if (!target || !sourceBin) {
  console.error("Usage: node npm/stage-binary.mjs <platform-arch> <path-to-lamp_server>");
  process.exit(2);
}

const rootDir = path.dirname(fileURLToPath(import.meta.url));
const pkgDir = path.join(rootDir, target);
if (!fs.existsSync(path.join(pkgDir, "package.json"))) {
  console.error(`Unknown platform package directory: ${pkgDir}`);
  process.exit(2);
}

const resolvedSrc = path.resolve(sourceBin);
if (!fs.existsSync(resolvedSrc)) {
  console.error(`Source binary not found: ${resolvedSrc}`);
  process.exit(1);
}

const exeName = target.startsWith("win32-") ? "lamp_server.exe" : "lamp_server";
const destDir = path.join(pkgDir, "bin");
const destBin = path.join(destDir, exeName);

fs.mkdirSync(destDir, { recursive: true });
fs.copyFileSync(resolvedSrc, destBin);

if (!target.startsWith("win32-")) {
  fs.chmodSync(destBin, 0o755);
}

const sizeMb = (fs.statSync(destBin).size / (1024 * 1024)).toFixed(2);
console.log(`Staged ${destBin} (${sizeMb} MB)`);
