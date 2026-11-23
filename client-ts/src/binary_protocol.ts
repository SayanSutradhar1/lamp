import { LampClientError } from "./errors.ts";
import type { LampBinaryFile, LampInputFile } from "./types.ts";

/** Byte size of an `LMP1` binary frame header on the wire (`14` bytes). */
export const LMP_HEADER_BYTES = 14;

/** Maximum allowed single `LMP1` frame payload size (`64 MiB`). */
export const MAX_LMP_PAYLOAD_BYTES = 64 * 1024 * 1024;

/** Wire frame kinds carried in the low nibble of `LMP1` header byte 4. */
export const LmpFrameType = {
  /** RPC operation invocation frame carrying an `LCB1` payload. */
  Request: 1,
  /** Successful RPC response frame carrying an `LCB1` result payload. */
  Response: 2,
  /** Structured RPC error response frame carrying an `LCB1` error payload. */
  Error: 3,
  /** Streaming chunk frame. */
  StreamChunk: 4,
  /** Wire-level keepalive/latency probe request. */
  Ping: 5,
  /** Wire-level keepalive/latency probe reply. */
  Pong: 6,
} as const;

/** Numeric `LMP1` frame type discriminator. */
export type LmpFrameTypeValue = (typeof LmpFrameType)[keyof typeof LmpFrameType];

/** Bitmask flags carried in `LMP1` header byte 5. */
export const LmpFrameFlags = {
  /** Default empty flags. */
  None: 0x00,
  /** Marks final frame of an operation exchange. */
  EndOfStream: 0x01,
  /** Response served directly from C++ `FullCycleWireCache` before reaching V8. */
  CacheHit: 0x02,
  /** Invocation carries one or more zero-base64 binary file payloads. */
  HasFiles: 0x04,
} as const;

/** Wire codec identifiers carried in the high nibble of `LMP1` header byte 4. */
export const LmpCodecKind = {
  /** Lamp Compact Binary (`LCB1`) with string-interning dictionary. */
  LampCompactBinary: 1,
  /** Raw opaque binary frame. */
  RawBinary: 2,
  /** JSON fallback codec. */
  JsonFallback: 3,
} as const;

const LCB_TAG = {
  Null: 0,
  False: 1,
  True: 2,
  Int64: 3,
  UInt64: 4,
  Float64: 5,
  String: 6,
  Bytes: 7,
  Array: 8,
  Object: 9,
} as const;

/** Decoded `LMP1` 14-byte frame header metadata. */
export interface LmpFrameHeader {
  /** Frame type (`Request`, `Response`, `Error`, `Ping`, `Pong`, etc.). */
  readonly type: number;
  /** Bitmask combining {@link LmpFrameFlags}. */
  readonly flags: number;
  /** Codec identifier (defaults to `LmpCodecKind.LampCompactBinary`). */
  readonly codec: number;
  /** Unsigned 32-bit multiplexed request correlation ID. */
  readonly requestId: number;
  /** Payload length in bytes following the 14-byte header. */
  readonly payloadSize: number;
}

/** Complete parsed `LMP1` binary wire frame. */
export interface LmpFrame {
  /** Decoded header metadata. */
  readonly header: LmpFrameHeader;
  /** Raw wire payload bytes (`LCB1` encoded container or ping payload). */
  readonly payload: Uint8Array;
}

/** Decoded `LCB1` RPC response envelope returned from C++ runtime handlers. */
export interface DecodedBinaryRpcResult<TOutput = unknown> {
  /** `true` on success, `false` when error metadata is populated. */
  readonly ok: boolean;
  /** Decoded output value when `ok` is `true`. */
  readonly output: TOutput;
  /** Decoded attached response binary files. */
  readonly files: readonly Required<LampBinaryFile>[];
  /** Canonical error code when `ok` is `false`. */
  readonly errorCode: string;
  /** Machine-readable error identifier when `ok` is `false`. */
  readonly errorIdentifier: string;
  /** Human-readable error message when `ok` is `false`. */
  readonly errorMessage: string;
  /** Whether the operation failure is safe to retry. */
  readonly retryable: boolean;
}

/** Normalized request input alongside extracted raw binary file payloads. */
export interface NormalizedRpcInput {
  /** JSON-compatible object tree with file positions replaced by `{ __lampFileRef: index }`. */
  readonly value: unknown;
  /** Ordered binary file payloads transferred verbatim inside `LCB1`. */
  readonly files: readonly Required<LampBinaryFile>[];
}

const textEncoder = new TextEncoder();
const textDecoder = new TextDecoder();

class ByteBuilder {
  private buf: Uint8Array;
  private len = 0;

  constructor(initialCapacity = 256) {
    this.buf = new Uint8Array(initialCapacity);
  }

  private ensure(extra: number): void {
    const needed = this.len + extra;
    if (needed <= this.buf.length) return;
    let next = this.buf.length * 2;
    while (next < needed) next *= 2;
    const grown = new Uint8Array(next);
    grown.set(this.buf.subarray(0, this.len), 0);
    this.buf = grown;
  }

  pushByte(b: number): void {
    this.ensure(1);
    this.buf[this.len++] = b & 0xff;
  }

  pushBytes(bytes: Uint8Array): void {
    if (bytes.byteLength === 0) return;
    this.ensure(bytes.byteLength);
    this.buf.set(bytes, this.len);
    this.len += bytes.byteLength;
  }

  pushVarintBigInt(value: bigint): void {
    let v = value;
    while (v >= 0x80n) {
      this.pushByte(Number(v & 0x7fn) | 0x80);
      v >>= 7n;
    }
    this.pushByte(Number(v));
  }

  pushVarintNumber(value: number): void {
    if (value <= 0xffffffff && value >= 0) {
      let v = value >>> 0;
      while (v >= 0x80) {
        this.pushByte((v & 0x7f) | 0x80);
        v >>>= 7;
      }
      this.pushByte(v);
      return;
    }
    this.pushVarintBigInt(BigInt(Math.trunc(value)));
  }

  pushFloat64BE(value: number): void {
    this.ensure(8);
    const view = new DataView(this.buf.buffer, this.buf.byteOffset + this.len, 8);
    view.setFloat64(0, value, false);
    this.len += 8;
  }

  finish(): Uint8Array {
    return this.buf.slice(0, this.len);
  }
}

class StringTable {
  private readonly indexByStr = new Map<string, number>();
  readonly entries: Uint8Array[] = [];

  intern(str: string): number {
    const existing = this.indexByStr.get(str);
    if (existing !== undefined) {
      return existing;
    }
    const idx = this.entries.length;
    this.indexByStr.set(str, idx);
    this.entries.push(textEncoder.encode(str));
    return idx;
  }
}

function readVarint(
  bytes: Uint8Array,
  state: { pos: number }
): bigint {
  let result = 0n;
  let shift = 0n;
  while (state.pos < bytes.byteLength) {
    const b = bytes[state.pos++];
    if (shift >= 64n) {
      throw new LampClientError("LCB1 varint overflow", {
        code: "SERIALIZATION_FAILED",
        identifier: "LCB1_VARINT_OVERFLOW",
      });
    }
    result |= BigInt(b & 0x7f) << shift;
    if ((b & 0x80) === 0) {
      return result;
    }
    shift += 7n;
  }
  throw new LampClientError("Truncated LCB1 varint", {
    code: "SERIALIZATION_FAILED",
    identifier: "LCB1_TRUNCATED",
  });
}

function zigZagEncode(n: bigint): bigint {
  return n >= 0n ? n << 1n : ((-n) << 1n) - 1n;
}

function zigZagDecode(n: bigint): bigint {
  return (n & 1n) === 0n ? n >> 1n : -((n >> 1n) + 1n);
}

function bigIntToSafeNumber(n: bigint): number | bigint {
  if (n >= BigInt(Number.MIN_SAFE_INTEGER) && n <= BigInt(Number.MAX_SAFE_INTEGER)) {
    return Number(n);
  }
  return n;
}

function isBlobLike(val: unknown): val is Blob {
  return (
    typeof val === "object" &&
    val !== null &&
    typeof (val as Blob).arrayBuffer === "function" &&
    typeof (val as Blob).size === "number"
  );
}

function isLampBinaryFileLike(val: unknown): val is LampBinaryFile {
  if (typeof val !== "object" || val === null || Array.isArray(val)) {
    return false;
  }
  const rec = val as Record<string, unknown>;
  return (
    typeof rec.name === "string" &&
    (rec.data instanceof Uint8Array || ArrayBuffer.isView(rec.data))
  );
}

/**
 * Asynchronously normalizes any JSON-like operation input paired with explicit files or nested
 * `Uint8Array` / {@link LampBinaryFile} / browser `File` & `Blob` values into zero-base64
 * binary attachments without blocking the JS event loop.
 *
 * @param input - Operation input payload resembling standard REST JSON + file inputs.
 * @param explicitFiles - Optional explicit file array exposed via server `ctx.files`.
 * @returns Normalized value tree and extracted raw {@link LampBinaryFile} array.
 */
export async function normalizeFileInputsAsync(
  input: unknown,
  explicitFiles: readonly LampInputFile[] = []
): Promise<NormalizedRpcInput> {
  const files: Required<LampBinaryFile>[] = [];

  async function toNormalizedFile(
    raw: LampInputFile,
    fallbackName: string
  ): Promise<Required<LampBinaryFile>> {
    if (isBlobLike(raw)) {
      const ab = await raw.arrayBuffer();
      const maybeName =
        "name" in raw && typeof raw.name === "string" && raw.name.length > 0
          ? raw.name
          : fallbackName;
      return {
        name: maybeName,
        mimeType: raw.type || "application/octet-stream",
        data: new Uint8Array(ab),
      };
    }
    const u8 =
      raw.data instanceof Uint8Array
        ? raw.data
        : new Uint8Array(
            (raw.data as ArrayBufferView).buffer,
            (raw.data as ArrayBufferView).byteOffset,
            (raw.data as ArrayBufferView).byteLength
          );
    return {
      name: raw.name,
      mimeType: raw.mimeType || "application/octet-stream",
      data: u8,
    };
  }

  for (let i = 0; i < explicitFiles.length; i += 1) {
    files.push(await toNormalizedFile(explicitFiles[i], `file_${i}.bin`));
  }

  async function walk(val: unknown, depth: number): Promise<unknown> {
    if (depth > 32 || val === null || val === undefined || typeof val !== "object") {
      return val;
    }
    if (val instanceof Uint8Array) {
      return val;
    }
    if (ArrayBuffer.isView(val)) {
      return new Uint8Array(val.buffer, val.byteOffset, val.byteLength);
    }
    if (isBlobLike(val) || isLampBinaryFileLike(val)) {
      const idx = files.length;
      files.push(await toNormalizedFile(val as LampInputFile, `upload_${idx}.bin`));
      return { __lampFileRef: idx };
    }
    if (Array.isArray(val)) {
      const out: unknown[] = new Array(val.length);
      for (let i = 0; i < val.length; i += 1) {
        out[i] = await walk(val[i], depth + 1);
      }
      return out;
    }
    const rec = val as Record<string, unknown>;
    const out: Record<string, unknown> = {};
    for (const [k, v] of Object.entries(rec)) {
      if (v === undefined || typeof v === "function") continue;
      out[k] = await walk(v, depth + 1);
    }
    return out;
  }

  const normalizedVal = await walk(input ?? null, 0);
  return { value: normalizedVal, files };
}

function writeLcbValue(
  val: unknown,
  dict: StringTable,
  body: ByteBuilder,
  depth = 0
): void {
  if (depth > 64) {
    throw new LampClientError("Maximum nesting depth exceeded encoding LCB1 payload", {
      code: "SERIALIZATION_FAILED",
      identifier: "LCB1_DEPTH_EXCEEDED",
    });
  }
  if (val === null || val === undefined) {
    body.pushByte(LCB_TAG.Null);
    return;
  }
  if (typeof val === "boolean") {
    body.pushByte(val ? LCB_TAG.True : LCB_TAG.False);
    return;
  }
  if (typeof val === "number") {
    if (!Number.isFinite(val)) {
      body.pushByte(LCB_TAG.Null);
      return;
    }
    if (Number.isInteger(val) && val >= Number.MIN_SAFE_INTEGER && val <= Number.MAX_SAFE_INTEGER) {
      if (val >= 0) {
        body.pushByte(LCB_TAG.UInt64);
        body.pushVarintNumber(val);
      } else {
        body.pushByte(LCB_TAG.Int64);
        body.pushVarintBigInt(zigZagEncode(BigInt(val)));
      }
      return;
    }
    body.pushByte(LCB_TAG.Float64);
    body.pushFloat64BE(val);
    return;
  }
  if (typeof val === "bigint") {
    body.pushByte(LCB_TAG.Int64);
    body.pushVarintBigInt(zigZagEncode(val));
    return;
  }
  if (typeof val === "string") {
    body.pushByte(LCB_TAG.String);
    body.pushVarintNumber(dict.intern(val));
    return;
  }
  if (val instanceof Uint8Array) {
    body.pushByte(LCB_TAG.Bytes);
    body.pushVarintNumber(val.byteLength);
    body.pushBytes(val);
    return;
  }
  if (Array.isArray(val)) {
    body.pushByte(LCB_TAG.Array);
    body.pushVarintNumber(val.length);
    for (let i = 0; i < val.length; i += 1) {
      writeLcbValue(val[i], dict, body, depth + 1);
    }
    return;
  }
  if (typeof val === "object") {
    const entries = Object.entries(val as Record<string, unknown>).filter(
      ([, v]) => v !== undefined && typeof v !== "function"
    );
    body.pushByte(LCB_TAG.Object);
    body.pushVarintNumber(entries.length);
    for (const [k, v] of entries) {
      body.pushVarintNumber(dict.intern(k));
      writeLcbValue(v, dict, body, depth + 1);
    }
    return;
  }
  body.pushByte(LCB_TAG.Null);
}

function readLcbValue(
  bytes: Uint8Array,
  state: { pos: number },
  dict: readonly string[],
  depth = 0
): unknown {
  if (depth > 64 || state.pos >= bytes.byteLength) {
    throw new LampClientError("Invalid LCB1 value encoding", {
      code: "SERIALIZATION_FAILED",
      identifier: "LCB1_DECODE_ERROR",
    });
  }
  const tag = bytes[state.pos++];
  switch (tag) {
    case LCB_TAG.Null:
      return null;
    case LCB_TAG.False:
      return false;
    case LCB_TAG.True:
      return true;
    case LCB_TAG.Int64:
      return bigIntToSafeNumber(zigZagDecode(readVarint(bytes, state)));
    case LCB_TAG.UInt64:
      return bigIntToSafeNumber(readVarint(bytes, state));
    case LCB_TAG.Float64: {
      if (bytes.byteLength - state.pos < 8) {
        throw new LampClientError("Truncated LCB1 Float64", {
          code: "SERIALIZATION_FAILED",
          identifier: "LCB1_TRUNCATED",
        });
      }
      const view = new DataView(bytes.buffer, bytes.byteOffset + state.pos, 8);
      state.pos += 8;
      return view.getFloat64(0, false);
    }
    case LCB_TAG.String: {
      const idx = Number(readVarint(bytes, state));
      if (idx < 0 || idx >= dict.length) {
        throw new LampClientError("Invalid LCB1 string index", {
          code: "SERIALIZATION_FAILED",
          identifier: "LCB1_BAD_STRING_REF",
        });
      }
      return dict[idx];
    }
    case LCB_TAG.Bytes: {
      const len = Number(readVarint(bytes, state));
      if (len < 0 || len > bytes.byteLength - state.pos) {
        throw new LampClientError("Truncated LCB1 byte slice", {
          code: "SERIALIZATION_FAILED",
          identifier: "LCB1_TRUNCATED_BYTES",
        });
      }
      const slice = bytes.slice(state.pos, state.pos + len);
      state.pos += len;
      return slice;
    }
    case LCB_TAG.Array: {
      const count = Number(readVarint(bytes, state));
      const out = new Array<unknown>(count);
      for (let i = 0; i < count; i += 1) {
        out[i] = readLcbValue(bytes, state, dict, depth + 1);
      }
      return out;
    }
    case LCB_TAG.Object: {
      const count = Number(readVarint(bytes, state));
      const out: Record<string, unknown> = {};
      for (let i = 0; i < count; i += 1) {
        const kIdx = Number(readVarint(bytes, state));
        if (kIdx < 0 || kIdx >= dict.length) {
          throw new LampClientError("Invalid LCB1 key index", {
            code: "SERIALIZATION_FAILED",
            identifier: "LCB1_BAD_KEY_REF",
          });
        }
        out[dict[kIdx]] = readLcbValue(bytes, state, dict, depth + 1);
      }
      return out;
    }
    default:
      throw new LampClientError(`Unknown LCB1 value tag ${tag}`, {
        code: "SERIALIZATION_FAILED",
        identifier: "LCB1_UNKNOWN_TAG",
      });
  }
}

/**
 * Encodes an operation call (`operationId`, JSON-compatible input, and native binary files)
 * into a compact `LCB1` binary request payload matching C++ `LcbCodec::EncodeRpcRequest`.
 *
 * @param operationId - Registered operation ID (for example `"users.create"`).
 * @param input - Normalized JSON-compatible payload.
 * @param files - Raw attached binary files transferred without base64 overhead.
 * @returns Encoded `LCB1` container bytes.
 */
export function encodeRpcRequestBinary(
  operationId: string,
  input: unknown = null,
  files: readonly Required<LampBinaryFile>[] = []
): Uint8Array {
  const dict = new StringTable();
  const body = new ByteBuilder(256);

  body.pushVarintNumber(dict.intern(operationId));
  writeLcbValue(input, dict, body);
  body.pushVarintNumber(files.length);

  for (let i = 0; i < files.length; i += 1) {
    const f = files[i];
    body.pushVarintNumber(dict.intern(f.name));
    body.pushVarintNumber(dict.intern(f.mimeType));
    body.pushVarintNumber(f.data.byteLength);
    body.pushBytes(f.data);
  }

  const bodyBytes = body.finish();
  const container = new ByteBuilder(16 + dict.entries.length * 16 + bodyBytes.byteLength);
  container.pushByte(0x4c); // 'L'
  container.pushByte(1);
  container.pushVarintNumber(dict.entries.length);
  for (let i = 0; i < dict.entries.length; i += 1) {
    const sBytes = dict.entries[i];
    container.pushVarintNumber(sBytes.byteLength);
    container.pushBytes(sBytes);
  }
  container.pushBytes(bodyBytes);
  return container.finish();
}

/**
 * Wraps an `LCB1` (or ping) payload inside a 14-byte `LMP1` multiplexed binary frame header.
 *
 * @param type - Frame type from {@link LmpFrameType}.
 * @param requestId - Multiplexed 32-bit request correlation ID.
 * @param payload - Binary payload bytes.
 * @param flags - Bitmask flags from {@link LmpFrameFlags}.
 * @param codec - Codec discriminator from {@link LmpCodecKind}.
 * @returns Complete ready-to-send `LMP1` wire frame.
 */
export function encodeLmpFrame(
  type: number,
  requestId: number,
  payload: Uint8Array,
  flags: number = LmpFrameFlags.EndOfStream,
  codec: number = LmpCodecKind.LampCompactBinary
): Uint8Array {
  const out = new Uint8Array(LMP_HEADER_BYTES + payload.byteLength);
  out[0] = 0x4c; // 'L'
  out[1] = 0x4d; // 'M'
  out[2] = 0x50; // 'P'
  out[3] = 0x31; // '1'
  out[4] = ((codec & 0x0f) << 4) | (type & 0x0f);
  out[5] = flags & 0xff;

  const dv = new DataView(out.buffer, out.byteOffset, LMP_HEADER_BYTES);
  dv.setUint32(6, requestId >>> 0, false);
  dv.setUint32(10, payload.byteLength >>> 0, false);

  if (payload.byteLength > 0) {
    out.set(payload, LMP_HEADER_BYTES);
  }
  return out;
}

/**
 * Converts JSON-like request inputs and binary files into a stream of `LMP1`/`LCB1` wire chunks
 * asynchronously, yielding to the event loop between chunks so large conversions never block
 * interactive UI or concurrent backend tasks.
 *
 * @param operationId - Registered operation ID.
 * @param requestId - Multiplexed wire correlation ID.
 * @param input - JSON-compatible payload possibly containing files (`LampBinaryFile`, `File`, `Blob`).
 * @param explicitFiles - Optional explicit file array.
 * @param chunkBytes - Target chunk yield threshold in bytes (defaults to `65536`).
 * @yields Binary `Uint8Array` frame chunks ready to flush directly onto a socket.
 */
export async function* encodeRpcRequestStreamAsync(
  operationId: string,
  requestId: number,
  input: unknown,
  explicitFiles: readonly LampInputFile[] = [],
  chunkBytes = 65536
): AsyncGenerator<Uint8Array, { totalBytes: number; hasFiles: boolean }, void> {
  const normalized = await normalizeFileInputsAsync(input, explicitFiles);
  const lcbPayload = encodeRpcRequestBinary(
    operationId,
    normalized.value,
    normalized.files
  );
  const flags =
    LmpFrameFlags.EndOfStream |
    (normalized.files.length > 0 ? LmpFrameFlags.HasFiles : LmpFrameFlags.None);

  const frame = encodeLmpFrame(
    LmpFrameType.Request,
    requestId,
    lcbPayload,
    flags,
    LmpCodecKind.LampCompactBinary
  );

  const step = Math.max(4096, chunkBytes);
  if (frame.byteLength <= step) {
    yield frame;
    return { totalBytes: frame.byteLength, hasFiles: normalized.files.length > 0 };
  }

  for (let offset = 0; offset < frame.byteLength; offset += step) {
    yield frame.subarray(offset, Math.min(frame.byteLength, offset + step));
    // Yield control between binary slices so UI/concurrent I/O remains unblocked.
    await new Promise<void>((resolve) => setTimeout(resolve, 0));
  }

  return { totalBytes: frame.byteLength, hasFiles: normalized.files.length > 0 };
}

/**
 * Decodes an `LCB1` response payload produced by C++ `LcbCodec::EncodeRpcResponse`.
 *
 * @typeParam TOutput - Expected decoded output type.
 * @param bytes - Raw `LMP1` frame payload bytes.
 * @returns Decoded response value, attached response files, or structured error fields.
 */
export function decodeRpcResponseBinary<TOutput = unknown>(
  bytes: Uint8Array
): DecodedBinaryRpcResult<TOutput> {
  if (bytes.byteLength < 3 || bytes[0] !== 0x4c || bytes[1] !== 1) {
    throw new LampClientError("Invalid LCB1 container header", {
      code: "SERIALIZATION_FAILED",
      identifier: "LCB1_INVALID_MAGIC",
    });
  }

  const state = { pos: 2 };
  const dictCount = Number(readVarint(bytes, state));
  const dict: string[] = new Array(dictCount);
  for (let i = 0; i < dictCount; i += 1) {
    const len = Number(readVarint(bytes, state));
    if (len < 0 || len > bytes.byteLength - state.pos) {
      throw new LampClientError("Truncated LCB1 dictionary entry", {
        code: "SERIALIZATION_FAILED",
        identifier: "LCB1_BAD_DICT",
      });
    }
    dict[i] = textDecoder.decode(bytes.subarray(state.pos, state.pos + len));
    state.pos += len;
  }

  if (state.pos >= bytes.byteLength) {
    throw new LampClientError("Truncated LCB1 response status byte", {
      code: "SERIALIZATION_FAILED",
      identifier: "LCB1_TRUNCATED",
    });
  }

  const ok = bytes[state.pos++] !== 0;
  if (ok) {
    const output = readLcbValue(bytes, state, dict) as TOutput;
    const fileCount = Number(readVarint(bytes, state));
    const files: Required<LampBinaryFile>[] = [];
    for (let i = 0; i < fileCount; i += 1) {
      const nameIdx = Number(readVarint(bytes, state));
      const mimeIdx = Number(readVarint(bytes, state));
      const dataLen = Number(readVarint(bytes, state));
      if (
        nameIdx >= dict.length ||
        mimeIdx >= dict.length ||
        dataLen < 0 ||
        dataLen > bytes.byteLength - state.pos
      ) {
        throw new LampClientError("Corrupted LCB1 attached file entry", {
          code: "SERIALIZATION_FAILED",
          identifier: "LCB1_CORRUPT_FILE",
        });
      }
      const data = bytes.slice(state.pos, state.pos + dataLen);
      state.pos += dataLen;
      files.push({
        name: dict[nameIdx],
        mimeType: dict[mimeIdx],
        data,
      });
    }

    return {
      ok: true,
      output,
      files,
      errorCode: "",
      errorIdentifier: "",
      errorMessage: "",
      retryable: false,
    };
  }

  const codeIdx = Number(readVarint(bytes, state));
  const idIdx = Number(readVarint(bytes, state));
  const msgIdx = Number(readVarint(bytes, state));
  const retryable = state.pos < bytes.byteLength ? bytes[state.pos++] !== 0 : false;

  return {
    ok: false,
    output: null as TOutput,
    files: [],
    errorCode: dict[codeIdx] ?? "INTERNAL",
    errorIdentifier: dict[idIdx] ?? "INTERNAL",
    errorMessage: dict[msgIdx] ?? "Operation failed",
    retryable,
  };
}

/**
 * Streaming binary frame accumulator reassembling TCP / WebSocket byte fragments
 * into complete {@link LmpFrame} instances.
 */
export class LmpFrameDecoder {
  private buffer = new Uint8Array(0);
  private offset = 0;

  /**
   * Appends newly arrived socket bytes to the internal ring buffer.
   *
   * @param chunk - Raw bytes read from TCP socket or binary WebSocket message.
   */
  append(chunk: Uint8Array): void {
    if (chunk.byteLength === 0) return;
    const remaining = this.buffer.byteLength - this.offset;
    if (remaining === 0) {
      this.buffer = chunk.slice();
      this.offset = 0;
      return;
    }
    const merged = new Uint8Array(remaining + chunk.byteLength);
    merged.set(this.buffer.subarray(this.offset), 0);
    merged.set(chunk, remaining);
    this.buffer = merged;
    this.offset = 0;
  }

  /**
   * Attempts to extract the next complete `LMP1` wire frame.
   *
   * @returns Next parsed {@link LmpFrame}, or `null` if more bytes are needed.
   */
  tryNextFrame(): LmpFrame | null {
    const avail = this.buffer.byteLength - this.offset;
    if (avail < LMP_HEADER_BYTES) {
      return null;
    }

    const base = this.offset;
    if (
      this.buffer[base] !== 0x4c ||
      this.buffer[base + 1] !== 0x4d ||
      this.buffer[base + 2] !== 0x50 ||
      this.buffer[base + 3] !== 0x31
    ) {
      throw new LampClientError("Invalid LMP1 frame magic header", {
        code: "SERIALIZATION_FAILED",
        identifier: "INVALID_LMP1_MAGIC",
      });
    }

    const dv = new DataView(this.buffer.buffer, this.buffer.byteOffset + base, LMP_HEADER_BYTES);
    const payloadSize = dv.getUint32(10, false);
    if (payloadSize > MAX_LMP_PAYLOAD_BYTES) {
      throw new LampClientError("LMP1 frame exceeds maximum allowed size", {
        code: "RESOURCE_EXHAUSTED",
        identifier: "FRAME_TOO_LARGE",
      });
    }

    const totalLen = LMP_HEADER_BYTES + payloadSize;
    if (avail < totalLen) {
      return null;
    }

    const typeCodec = this.buffer[base + 4];
    const header: LmpFrameHeader = {
      type: typeCodec & 0x0f,
      codec: (typeCodec >> 4) & 0x0f,
      flags: this.buffer[base + 5],
      requestId: dv.getUint32(6, false),
      payloadSize,
    };

    const payload = this.buffer.slice(base + LMP_HEADER_BYTES, base + totalLen);
    this.offset += totalLen;

    if (this.offset === this.buffer.byteLength) {
      this.buffer = new Uint8Array(0);
      this.offset = 0;
    } else if (this.offset > 65536) {
      this.buffer = this.buffer.slice(this.offset);
      this.offset = 0;
    }

    return { header, payload };
  }
}
