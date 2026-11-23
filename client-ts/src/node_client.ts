import net from "node:net";
import {
  LmpFrameDecoder,
  LmpFrameFlags,
  LmpFrameType,
  decodeRpcResponseBinary,
  encodeLmpFrame,
  encodeRpcRequestStreamAsync,
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

function parseEndpoint(rawUrl: string): { host: string; port: number } {
  const normalized = /^[a-z]+:\/\//i.test(rawUrl) ? rawUrl : `lmp://${rawUrl}`;
  const parsed = new URL(normalized);
  const host = parsed.hostname || "127.0.0.1";
  const port = parsed.port ? Number(parsed.port) : 3000;
  if (!Number.isInteger(port) || port <= 0 || port > 65535) {
    throw new LampClientError(`Invalid Lamp endpoint port in "${rawUrl}"`, {
      code: "INVALID_ARGUMENT",
      identifier: "INVALID_ENDPOINT",
    });
  }
  return { host, port };
}

/**
 * Creates a persistent multiplexed Node.js TCP binary client (`LMP1` + `LCB1`)
 * communicating directly with the C++ `lamp_server` runtime with zero HTTP/REST overhead.
 *
 * @param options - Endpoint URL and connection/streaming timeouts.
 * @returns Multiplexed {@link LampClient} backed by a `node:net` socket (`TCP_NODELAY`).
 */
export function createLampClient(options: LampClientOptions): LampClient {
  const { host, port } = parseEndpoint(options.url);
  const defaultTimeoutMs = options.timeoutMs ?? 30_000;
  const connectTimeoutMs = options.connectTimeoutMs ?? 5_000;
  const defaultChunkBytes = options.streamChunkBytes ?? 65_536;

  let socket: net.Socket | null = null;
  let connectPromise: Promise<net.Socket> | null = null;
  let closed = false;
  let nextRequestId = 1;
  let writeChain: Promise<void> = Promise.resolve();

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

  function handleIncomingData(chunk: Buffer): void {
    try {
      decoder.append(chunk);
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
          : new LampClientError("Corrupted binary frame received from server", {
              code: "SERIALIZATION_FAILED",
              identifier: "FRAME_DECODE_ERROR",
              cause: err,
            })
      );
      socket?.destroy();
    }
  }

  async function ensureConnected(): Promise<net.Socket> {
    if (closed) {
      throw new LampClientError("Lamp client has been closed", {
        code: "FAILED_PRECONDITION",
        identifier: "CLIENT_CLOSED",
      });
    }
    if (socket && !socket.destroyed && socket.writable) {
      return socket;
    }
    if (connectPromise) {
      return connectPromise;
    }

    connectPromise = new Promise<net.Socket>((resolve, reject) => {
      const sock = net.createConnection({ host, port });
      sock.setNoDelay(true);

      const timer = setTimeout(() => {
        sock.destroy();
        connectPromise = null;
        reject(
          new LampClientError(`Timed out connecting to lmp://${host}:${port}`, {
            code: "UNAVAILABLE",
            identifier: "CONNECT_TIMEOUT",
            retryable: true,
          })
        );
      }, connectTimeoutMs);

      sock.once("connect", () => {
        clearTimeout(timer);
        socket = sock;
        connectPromise = null;

        sock.on("data", handleIncomingData);
        sock.on("error", (err) => {
          failAllPending(
            new LampClientError(`Binary socket error: ${err.message}`, {
              code: "UNAVAILABLE",
              identifier: "SOCKET_ERROR",
              retryable: true,
              cause: err,
            })
          );
        });
        sock.on("close", () => {
          if (socket === sock) {
            socket = null;
          }
          failAllPending(
            new LampClientError("Connection to Lamp binary server closed", {
              code: "UNAVAILABLE",
              identifier: "CONNECTION_CLOSED",
              retryable: true,
            })
          );
        });

        resolve(sock);
      });

      sock.once("error", (err) => {
        clearTimeout(timer);
        connectPromise = null;
        reject(
          new LampClientError(`Failed to connect to lmp://${host}:${port}: ${err.message}`, {
            code: "UNAVAILABLE",
            identifier: "CONNECT_FAILED",
            retryable: true,
            cause: err,
          })
        );
      });
    });

    return connectPromise;
  }

  async function writeChunksSerialized(chunks: readonly Uint8Array[]): Promise<void> {
    const task = writeChain.then(async () => {
      const sock = await ensureConnected();
      for (const chunk of chunks) {
        if (!sock.write(chunk)) {
          await new Promise<void>((resolve) => sock.once("drain", resolve));
        }
      }
    });
    writeChain = task.catch(() => {});
    return task;
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

      const chunkSize = callOptions.streamChunkBytes ?? defaultChunkBytes;
      const chunks: Uint8Array[] = [];
      let totalBytes = 0;

      const gen = encodeRpcRequestStreamAsync(
        operationId,
        requestId,
        input,
        callOptions.files ?? [],
        chunkSize
      );
      while (true) {
        const next = await gen.next();
        if (next.done) {
          totalBytes = next.value.totalBytes;
          break;
        }
        chunks.push(next.value);
      }

      const timeoutMs = callOptions.timeoutMs ?? defaultTimeoutMs;
      await ensureConnected();

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
            requestBytes: totalBytes,
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

      await writeChunksSerialized(chunks).catch((err) => {
        const entry = pendingCalls.get(requestId);
        if (entry) {
          clearTimeout(entry.timeoutHandle);
          entry.cleanupAbort?.();
          pendingCalls.delete(requestId);
        }
        throw err;
      });

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
      await ensureConnected();
      const pingFrame = encodeLmpFrame(
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

      await writeChunksSerialized([pingFrame]);
      return waitPong;
    },

    async close(): Promise<void> {
      closed = true;
      failAllPending(
        new LampClientError("Lamp client closed", {
          code: "CANCELLED",
          identifier: "CLIENT_CLOSED",
        })
      );
      if (socket) {
        const s = socket;
        socket = null;
        await new Promise<void>((resolve) => {
          s.end(() => resolve());
          setTimeout(() => {
            s.destroy();
            resolve();
          }, 150);
        });
      }
    },
  };

  return client;
}
