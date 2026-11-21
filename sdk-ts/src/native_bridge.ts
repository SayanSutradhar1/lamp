// ============================================================
// Lamp TypeScript SDK - Host Bridge Detection & Adaptation
// ============================================================

import type { NativeHostBridge } from "./types.ts";

declare global {
  var __lampNativeBridge: NativeHostBridge | undefined;
}

export function resolveNativeHostBridge(
  explicit?: NativeHostBridge
): NativeHostBridge | undefined {
  if (explicit) {
    return explicit;
  }
  if (
    typeof globalThis === "object" &&
    globalThis !== null &&
    globalThis.__lampNativeBridge &&
    typeof globalThis.__lampNativeBridge.registerOperation === "function"
  ) {
    return globalThis.__lampNativeBridge;
  }
  return undefined;
}
