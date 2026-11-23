/**
 * Node Runtime Binary Adapter (`@lamp/node-adapter`).
 *
 * Bridges Node-only ecosystem packages (such as `mongoose`) that depend on Node core
 * sockets onto Lamp's non-REST `LMP1` + `LCB1` TCP & binary WebSocket wire protocol.
 *
 * @packageDocumentation
 */

import { createHash } from "node:crypto";
import net from "node:net";
import { normalizeLampError, type LampApp } from "lamp";
import {
  LmpFrameDecoder,
  LmpFrameFlags,
  LmpFrameType,
  encodeLmpFrame,
} from "lamp-client";
import {
  decodeRpcRequestBinary,
  encodeRpcResponseBinary,
  encodeWebSocketBinaryFrame,
} from "./binary_wire.ts";

const WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
const DEFAULT_MAX_TTL_LIMIT_MS = 300_000;

/**
 * Configuration accepted by {@link serveLampNodeAdapter}.
 */
export interface ServeLampNodeAdapterOptions {
  /** Interface address to bind (defaults to `"127.0.0.1"`). */
  readonly host?: string;
  /** TCP port to listen on (use `0` in automated tests for ephemeral port binding). */
  readonly port?: number;
  /** Hard upper TTL bound (ms) enforced for full-cycle wire caching (defaults to `300000`). */
  readonly maxCacheTtlMs?: number;
  /** Optional readiness probe reported in `$lamp.stats` (for example database health). */
  readonly readinessCheck?: () => boolean | Promise<boolean>;
}

/**
 * Handle for a running `@lamp/node-adapter` binary TCP + WebSocket server.
 */
export interface LampNodeAdapterHandle {
  /** Bound TCP port assigned by the operating system. */
  readonly port: number;
  /** Binary connection URL (`lmp://<host>:<port>`) usable with `lamp-client`. */
  readonly url: string;
  /** Browser WebSocket URL (`ws://<host>:<port>`) usable with `lamp-client/browser`. */
  readonly wsUrl: string;
  /** Gracefully closes all active sockets, shuts down the server, and invokes `app.stop()`. */
  close(): Promise<void>;
}

interface CacheEntry {
  readonly expiresAtMs: number;
  readonly encodedResponse: Uint8Array;
}

/**
 * Starts a persistent non-REST `LMP1`/`LCB1` TCP & binary WebSocket server adapter backed
 * by the given {@link LampApp} in the current Node.js process.
 *
 * Plain HTTP REST requests are rejected with `404 Not Found`. Both raw Node `lamp-client`
 * connections (`lmp://...`) and browser `lamp-client/browser` WebSocket connections (`ws://...`)
 * are supported on the same port.
 *
 * @param app - Configured server-side {@link LampApp} contract instance.
 * @param options - Bind address, port, full-cycle cache ceiling, and readiness probe.
 * @returns Active {@link LampNodeAdapterHandle} once listening.
 */
export async function serveLampNodeAdapter(
  app: LampApp,
  options: ServeLampNodeAdapterOptions = {}
): Promise<LampNodeAdapterHandle> {
  const host = options.host ?? "127.0.0.1";
  const requestedPort = options.port ?? 3000;
  const maxCacheTtlMs = Math.max(1000, options.maxCacheTtlMs ?? DEFAULT_MAX_TTL_LIMIT_MS);

  if (app.state() === "created") {
    await app.start();
  }

  const opMetaById = new Map(app.operations().map((meta) => [meta.id, meta]));
  const wireCache = new Map<string, CacheEntry>();
  let cacheHits = 0;
  let cacheMisses = 0;
  let completedTasks = 0;
  let activeWorkers = 0;

  const activeSockets = new Set<net.Socket>();

  function sendLmpFrame(
    sock: net.Socket,
    isWs: boolean,
    frameBytes: Uint8Array
  ): void {
    if (sock.destroyed || !sock.writable) return;
    sock.write(isWs ? encodeWebSocketBinaryFrame(frameBytes) : frameBytes);
  }

  async function dispatchLmpFrame(
    sock: net.Socket,
    isWs: boolean,
    type: number,
    requestId: number,
    payload: Uint8Array
  ): Promise<void> {
    if (type === LmpFrameType.Ping) {
      sendLmpFrame(
        sock,
        isWs,
        encodeLmpFrame(LmpFrameType.Pong, requestId, payload)
      );
      return;
    }
    if (type !== LmpFrameType.Request) {
      return;
    }

    let decoded;
    try {
      decoded = decodeRpcRequestBinary(payload);
    } catch (err) {
      const lampErr = normalizeLampError(err);
      sendLmpFrame(
        sock,
        isWs,
        encodeLmpFrame(
          LmpFrameType.Error,
          requestId,
          encodeRpcResponseBinary(
            false,
            null,
            lampErr.code,
            lampErr.identifier,
            lampErr.message,
            lampErr.retryable
          )
        )
      );
      return;
    }

    const opMeta = opMetaById.get(decoded.operationId);
    const cacheEnabled = Boolean(opMeta?.cache.enabled && opMeta.cache.ttlSeconds > 0);
    const now = Date.now();
    let cacheKey = "";

    if (cacheEnabled) {
      cacheKey = `${decoded.operationId}|${createHash("sha256").update(payload).digest("hex")}`;
      const hit = wireCache.get(cacheKey);
      if (hit && hit.expiresAtMs > now) {
        cacheHits += 1;
        sendLmpFrame(
          sock,
          isWs,
          encodeLmpFrame(
            LmpFrameType.Response,
            requestId,
            hit.encodedResponse,
            LmpFrameFlags.EndOfStream | LmpFrameFlags.CacheHit
          )
        );
        return;
      }
      if (hit) {
        wireCache.delete(cacheKey);
      }
    }

    if (decoded.operationId === "$lamp.manifest") {
      const manifest = {
        protocol: "LMP1/LCB1",
        restPattern: false,
        operations: app.operations().map((d) => ({
          id: d.id,
          cacheEnabled: d.cache.enabled,
          cacheTtlMs: d.cache.ttlSeconds * 1000,
          timeoutMs: d.execution.timeoutMs,
        })),
      };
      sendLmpFrame(
        sock,
        isWs,
        encodeLmpFrame(
          LmpFrameType.Response,
          requestId,
          encodeRpcResponseBinary(true, manifest)
        )
      );
      return;
    }

    if (decoded.operationId === "$lamp.stats") {
      const ready = options.readinessCheck ? await options.readinessCheck() : true;
      const stats = {
        activeWorkers,
        currentThreads: ready ? 1 : 0,
        peakThreads: 1,
        completedTasks,
        cacheHits,
        cacheMisses,
        cacheMaxTtlMs: maxCacheTtlMs,
      };
      sendLmpFrame(
        sock,
        isWs,
        encodeLmpFrame(
          LmpFrameType.Response,
          requestId,
          encodeRpcResponseBinary(true, stats)
        )
      );
      return;
    }

    cacheMisses += 1;
    activeWorkers += 1;
    try {
      const output = await app.invoke(decoded.operationId, decoded.input, {
        files: decoded.files,
      });
      completedTasks += 1;
      const lcbResp = encodeRpcResponseBinary(true, output);

      if (cacheEnabled && opMeta) {
        const clampedTtl = Math.min(opMeta.cache.ttlSeconds * 1000, maxCacheTtlMs);
        wireCache.set(cacheKey, {
          expiresAtMs: Date.now() + clampedTtl,
          encodedResponse: lcbResp,
        });
      }

      sendLmpFrame(
        sock,
        isWs,
        encodeLmpFrame(LmpFrameType.Response, requestId, lcbResp)
      );
    } catch (err) {
      completedTasks += 1;
      const lampErr = normalizeLampError(err);
      sendLmpFrame(
        sock,
        isWs,
        encodeLmpFrame(
          LmpFrameType.Error,
          requestId,
          encodeRpcResponseBinary(
            false,
            null,
            lampErr.code,
            lampErr.identifier,
            lampErr.message,
            lampErr.retryable
          )
        )
      );
    } finally {
      activeWorkers -= 1;
    }
  }

  const server = net.createServer((sock) => {
    sock.setNoDelay(true);
    activeSockets.add(sock);
    sock.on("close", () => activeSockets.delete(sock));
    sock.on("error", () => activeSockets.delete(sock));

    let mode: "detecting" | "raw" | "ws" = "detecting";
    let rawBuf: Buffer<ArrayBufferLike> = Buffer.alloc(0);
    const lmpDecoder = new LmpFrameDecoder();

    function drainLmp(isWs: boolean): void {
      while (true) {
        const frame = lmpDecoder.tryNextFrame();
        if (!frame) break;
        void dispatchLmpFrame(
          sock,
          isWs,
          frame.header.type,
          frame.header.requestId,
          frame.payload
        );
      }
    }

    function drainWebSocket(): void {
      let pos = 0;
      while (rawBuf.byteLength - pos >= 2) {
        const b0 = rawBuf[pos];
        const b1 = rawBuf[pos + 1];
        const opcode = b0 & 0x0f;
        const masked = (b1 & 0x80) !== 0;
        let payloadLen = b1 & 0x7f;
        let hdrLen = 2;

        if (payloadLen === 126) {
          if (rawBuf.byteLength - pos < 4) break;
          payloadLen = rawBuf.readUInt16BE(pos + 2);
          hdrLen = 4;
        } else if (payloadLen === 127) {
          if (rawBuf.byteLength - pos < 10) break;
          payloadLen = Number(rawBuf.readBigUInt64BE(pos + 2));
          hdrLen = 10;
        }

        const maskLen = masked ? 4 : 0;
        if (rawBuf.byteLength - pos < hdrLen + maskLen + payloadLen) break;

        const dataStart = pos + hdrLen + maskLen;
        const body = Uint8Array.from(rawBuf.subarray(dataStart, dataStart + payloadLen));
        if (masked) {
          const maskStart = pos + hdrLen;
          for (let i = 0; i < body.byteLength; i += 1) {
            body[i] ^= rawBuf[maskStart + (i & 3)];
          }
        }
        pos = dataStart + payloadLen;

        if (opcode === 0x08) {
          sock.end();
          return;
        }
        if (opcode === 0x09 && body.byteLength <= 125) {
          const pong = Buffer.alloc(2 + body.byteLength);
          pong[0] = 0x8a;
          pong[1] = body.byteLength;
          Buffer.from(body).copy(pong, 2);
          sock.write(pong);
          continue;
        }
        if (opcode === 0x02 || opcode === 0x00) {
          lmpDecoder.append(body);
          drainLmp(true);
        }
      }
      if (pos > 0) {
        rawBuf = rawBuf.subarray(pos);
      }
    }

    sock.on("data", (chunk: Buffer) => {
      rawBuf = rawBuf.byteLength === 0 ? chunk : Buffer.concat([rawBuf, chunk]);

      if (mode === "detecting") {
        if (rawBuf.byteLength < 4) return;
        const startsWithLmp =
          rawBuf[0] === 0x4c &&
          rawBuf[1] === 0x4d &&
          rawBuf[2] === 0x50 &&
          rawBuf[3] === 0x31;

        if (startsWithLmp) {
          mode = "raw";
          lmpDecoder.append(rawBuf);
          rawBuf = Buffer.alloc(0);
          drainLmp(false);
          return;
        }

        const hdrEnd = rawBuf.indexOf("\r\n\r\n");
        if (hdrEnd === -1) return;

        const headerText = rawBuf.subarray(0, hdrEnd).toString("latin1");
        const upgradeWs = /\r\nUpgrade:\s*websocket/i.test(headerText);
        const keyMatch = headerText.match(/\r\nSec-WebSocket-Key:\s*([^\r\n]+)/i);

        if (!upgradeWs || !keyMatch) {
          sock.end(
            "HTTP/1.1 404 Not Found\r\nConnection: close\r\nContent-Length: 36\r\n\r\nLamp does not expose HTTP REST endpoints"
          );
          return;
        }

        const accept = createHash("sha1")
          .update(`${keyMatch[1].trim()}${WS_GUID}`)
          .digest("base64");
        sock.write(
          `HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ${accept}\r\n\r\n`
        );
        mode = "ws";
        rawBuf = rawBuf.subarray(hdrEnd + 4);
        drainWebSocket();
        return;
      }

      if (mode === "raw") {
        lmpDecoder.append(chunk);
        drainLmp(false);
      } else {
        drainWebSocket();
      }
    });
  });

  await new Promise<void>((resolve, reject) => {
    server.once("error", reject);
    server.listen(requestedPort, host, () => {
      server.off("error", reject);
      resolve();
    });
  });

  const addr = server.address() as net.AddressInfo;
  const boundHost = addr.address.includes(":") ? "127.0.0.1" : addr.address;
  const boundPort = addr.port;

  return {
    port: boundPort,
    url: `lmp://${boundHost}:${boundPort}`,
    wsUrl: `ws://${boundHost}:${boundPort}`,
    async close() {
      for (const s of activeSockets) {
        s.destroy();
      }
      await new Promise<void>((resolve) => server.close(() => resolve()));
      await app.stop();
    },
  };
}
