import {
  LmpFrameDecoder,
  LmpFrameFlags,
  LmpFrameType,
  decodeRpcResponseBinary,
  encodeLmpFrame,
  encodeRpcRequestBinary,
  normalizeFileInputsAsync,
} from "./binary_protocol.ts";
import { LampClientError } from "./errors.ts";
import type {
  LampCallOptions,
  LampCallResponse,
  LampClient,
  LampClientOptions,
  LampManifest,
  LampRuntimeStats,
} from "./types.ts";

export { LampClientError, isLampClientError } from "./errors.ts";
export type {
  LampClientErrorCode,
  LampClientErrorOptions,
} from "./errors.ts";
export type {
  LampBinaryFile,
  LampCallOptions,
  LampCallResponse,
  LampCallResultMeta,
  LampClient,
  LampClientOptions,
  LampInputFile,
  LampManifest,
  LampManifestOperation,
  LampRuntimeStats,
} from "./types.ts";

interface PendingCall {
  readonly resolve: (res: LampCallResponse<any>) => void;
  readonly reject: (err: unknown) => void;
  readonly timeoutHandle: ReturnType<typeof setTimeout>;
  readonly requestBytes: number;
  cleanupAbort?: () => void;
}

interface PendingPing {
  readonly resolve: (rttMs: number) => void;
  readonly reject: (err: unknown) => void;
  readonly startedAt: number;
  readonly timeoutHandle: ReturnType<typeof setTimeout>;
}

function normalizeWsUrl(rawUrl: string): string {
  if (/^wss?:\/\//i.test(rawUrl)) return rawUrl;
  if (/^https?:\/\//i.test(rawUrl)) return rawUrl.replace(/^http/i, "ws");
  if (/^lmp:\/\//i.test(rawUrl)) return rawUrl.replace(/^lmp:/i, "ws:");
  return `ws://${rawUrl}`;
}

/**
 * Creates a browser-compatible binary WebSocket client (`LMP1` + `LCB1` over binary frames)
 * communicating directly with the C++ `lamp_server` runtime without HTTP REST routing.
 *
 * Uses zero `node:*` built-in modules and converts standard Web `File` / `Blob` inputs asynchronously.
 *
 * @param options - WebSocket endpoint URL (for example `"ws://127.0.0.1:3000"`) and timeout settings.
 * @returns Multiplexed {@link LampClient} backed by a persistent WebSocket binary connection.
 */
export function createLampClient(options: LampClientOptions): LampClient {
  const wsUrl = normalizeWsUrl(options.url);
  const defaultTimeoutMs = options.timeoutMs ?? 30_000;
  const connectTimeoutMs = options.connectTimeoutMs ?? 5_000;

  let ws: WebSocket | null = null;
  let connectPromise: Promise<WebSocket> | null = null;
  let closed = false;
  let nextRequestId = 1;

  const decoder = new LmpFrameDecoder();
  const pendingCalls = new Map<number, PendingCall>();
  const pendingPings = new Map<number, PendingPing>();

  function failAllPending(reason: Error): void {
    for (const [id, entry] of pendingCalls) {
      clearTimeout(entry.timeoutHandle);
      entry.cleanupAbort?.();
      pendingCalls.delete(id);
      entry.reject(reason);
    }
    for (const [id, entry] of pendingPings) {
      clearTimeout(entry.timeoutHandle);
      pendingPings.delete(id);
      entry.reject(reason);
    }
  }

  function handleMessage(data: unknown): void {
    try {
      const bytes =
        data instanceof ArrayBuffer
          ? new Uint8Array(data)
          : data instanceof Uint8Array
            ? data
            : null;
      if (!bytes) return;

      decoder.append(bytes);
      while (true) {
        const frame = decoder.tryNextFrame();
        if (!frame) break;

        if (frame.header.type === LmpFrameType.Pong) {
          const ping = pendingPings.get(frame.header.requestId);
          if (ping) {
            clearTimeout(ping.timeoutHandle);
            pendingPings.delete(frame.header.requestId);
            ping.resolve(performance.now() - ping.startedAt);
          }
          continue;
        }

        const pending = pendingCalls.get(frame.header.requestId);
        if (!pending) continue;

        clearTimeout(pending.timeoutHandle);
        pending.cleanupAbort?.();
        pendingCalls.delete(frame.header.requestId);

        const decoded = decodeRpcResponseBinary(frame.payload);
        if (!decoded.ok) {
          pending.reject(
            new LampClientError(decoded.errorMessage, {
              code: decoded.errorCode,
              identifier: decoded.errorIdentifier,
              retryable: decoded.retryable,
            })
          );
          continue;
        }

        pending.resolve({
          data: decoded.output,
          meta: {
            requestId: frame.header.requestId,
            cacheHit: (frame.header.flags & LmpFrameFlags.CacheHit) !== 0,
            requestBytes: pending.requestBytes,
            responseBytes: frame.payload.byteLength,
            files: decoded.files,
          },
        });
      }
    } catch (err) {
      failAllPending(
        err instanceof LampClientError
          ? err
          : new LampClientError("Failed to decode binary WebSocket frame", {
              code: "SERIALIZATION_FAILED",
              identifier: "WS_DECODE_ERROR",
              cause: err,
            })
      );
      ws?.close();
    }
  }

  async function ensureConnected(): Promise<WebSocket> {
    if (closed) {
      throw new LampClientError("Lamp client has been closed", {
        code: "FAILED_PRECONDITION",
        identifier: "CLIENT_CLOSED",
      });
    }
    if (ws && ws.readyState === WebSocket.OPEN) {
      return ws;
    }
    if (connectPromise) {
      return connectPromise;
    }

    connectPromise = new Promise<WebSocket>((resolve, reject) => {
      const sock = new WebSocket(wsUrl);
      sock.binaryType = "arraybuffer";

      const timer = setTimeout(() => {
        connectPromise = null;
        try {
          sock.close();
        } catch {
          // Ignore close errors during timeout
        }
        reject(
          new LampClientError(`Timed out opening binary WebSocket ${wsUrl}`, {
            code: "UNAVAILABLE",
            identifier: "CONNECT_TIMEOUT",
            retryable: true,
          })
        );
      }, connectTimeoutMs);

      sock.onopen = () => {
        clearTimeout(timer);
        ws = sock;
        connectPromise = null;
        resolve(sock);
      };

      sock.onmessage = (evt) => {
        handleMessage(evt.data);
      };

      sock.onerror = () => {
        clearTimeout(timer);
        if (connectPromise) {
          connectPromise = null;
          reject(
            new LampClientError(`Failed to open binary WebSocket ${wsUrl}`, {
              code: "UNAVAILABLE",
              identifier: "CONNECT_FAILED",
              retryable: true,
            })
          );
        }
      };

      sock.onclose = () => {
        if (ws === sock) {
          ws = null;
        }
        failAllPending(
          new LampClientError("WebSocket connection to Lamp server closed", {
            code: "UNAVAILABLE",
            identifier: "CONNECTION_CLOSED",
            retryable: true,
          })
        );
      };
    });

    return connectPromise;
  }

  const client: LampClient = {
    async call<TOutput = unknown>(
      operationId: string,
      input?: unknown,
      callOptions?: LampCallOptions
    ): Promise<TOutput> {
      const res = await client.callWithMeta<TOutput>(operationId, input, callOptions);
      return res.data;
    },

    async callWithMeta<TOutput = unknown>(
      operationId: string,
      input?: unknown,
      callOptions: LampCallOptions = {}
    ): Promise<LampCallResponse<TOutput>> {
      const requestId = nextRequestId++;
      if (nextRequestId > 0x7fffffff) nextRequestId = 1;

      if (callOptions.signal?.aborted) {
        throw new LampClientError("Operation aborted prior to send", {
          code: "CANCELLED",
          identifier: "CLIENT_ABORTED",
        });
      }

      const normalized = await normalizeFileInputsAsync(
        input,
        callOptions.files ?? []
      );
      const lcbPayload = encodeRpcRequestBinary(
        operationId,
        normalized.value,
        normalized.files
      );
      const flags =
        LmpFrameFlags.EndOfStream |
        (normalized.files.length > 0 ? LmpFrameFlags.HasFiles : LmpFrameFlags.None);
      const frame = encodeLmpFrame(LmpFrameType.Request, requestId, lcbPayload, flags);

      const sock = await ensureConnected();
      const timeoutMs = callOptions.timeoutMs ?? defaultTimeoutMs;

      const responsePromise = new Promise<LampCallResponse<TOutput>>(
        (resolve, reject) => {
          const timeoutHandle = setTimeout(() => {
            const entry = pendingCalls.get(requestId);
            if (entry) {
              entry.cleanupAbort?.();
              pendingCalls.delete(requestId);
            }
            reject(
              new LampClientError(
                `Operation "${operationId}" exceeded timeout of ${timeoutMs}ms`,
                {
                  code: "DEADLINE_EXCEEDED",
                  identifier: "CLIENT_TIMEOUT",
                  retryable: true,
                }
              )
            );
          }, timeoutMs);

          const pending: PendingCall = {
            resolve,
            reject,
            timeoutHandle,
            requestBytes: frame.byteLength,
          };

          if (callOptions.signal) {
            const onAbort = () => {
              const entry = pendingCalls.get(requestId);
              if (entry) {
                clearTimeout(entry.timeoutHandle);
                pendingCalls.delete(requestId);
                reject(
                  new LampClientError("Operation cancelled by AbortSignal", {
                    code: "CANCELLED",
                    identifier: "CLIENT_ABORTED",
                  })
                );
              }
            };
            callOptions.signal.addEventListener("abort", onAbort, { once: true });
            pending.cleanupAbort = () => {
              callOptions.signal?.removeEventListener("abort", onAbort);
            };
          }

          pendingCalls.set(requestId, pending);
        }
      );

      sock.send(frame);
      return responsePromise;
    },

    manifest() {
      return client.call<LampManifest>("$lamp.manifest");
    },

    stats() {
      return client.call<LampRuntimeStats>("$lamp.stats");
    },

    async ping(): Promise<number> {
      const requestId = nextRequestId++;
      if (nextRequestId > 0x7fffffff) nextRequestId = 1;
      const sock = await ensureConnected();
      const frame = encodeLmpFrame(
        LmpFrameType.Ping,
        requestId,
        new Uint8Array(0)
      );
      const startedAt = performance.now();
      const waitPong = new Promise<number>((resolve, reject) => {
        const timeoutHandle = setTimeout(() => {
          pendingPings.delete(requestId);
          reject(
            new LampClientError("Ping timed out", {
              code: "DEADLINE_EXCEEDED",
              identifier: "PING_TIMEOUT",
            })
          );
        }, defaultTimeoutMs);
        pendingPings.set(requestId, {
          resolve,
          reject,
          startedAt,
          timeoutHandle,
        });
      });
      sock.send(frame);
      return waitPong;
    },

    async close(): Promise<void> {
      closed = true;
      failAllPending(
        new LampClientError("Lamp browser client closed", {
          code: "CANCELLED",
          identifier: "CLIENT_CLOSED",
        })
      );
      if (ws) {
        ws.close();
        ws = null;
      }
    },
  };

  return client;
}
