import { LampClientError } from "lamp-client";
import type { LampFile } from "lamp";

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

const textEncoder = new TextEncoder();
const textDecoder = new TextDecoder();

/**
 * Decoded incoming `LCB1` RPC invocation container sent by `lamp-client`.
 */
export interface DecodedNodeRpcRequest {
  /** Registered operation identifier (e.g. `"users.create"`). */
  readonly operationId: string;
  /** Decoded JSON-compatible request input value (with `$lampFileRef` indices). */
  readonly input: unknown;
  /** Attached native binary files decoded without base64 inflation. */
  readonly files: readonly LampFile[];
}

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
    new DataView(this.buf.buffer, this.buf.byteOffset + this.len, 8).setFloat64(
      0,
      value,
      false
    );
    this.len += 8;
  }

  finish(): Uint8Array {
    return this.buf.slice(0, this.len);
  }
}

const utf8Cache = new Map<string, Uint8Array>();

function encodeUtf8Cached(str: string): Uint8Array {
  let encoded = utf8Cache.get(str);
  if (encoded !== undefined) {
    return encoded;
  }
  encoded = Buffer.from(str, "utf8");
  if (str.length <= 64) {
    if (utf8Cache.size >= 2048) {
      utf8Cache.clear();
    }
    utf8Cache.set(str, encoded);
  }
  return encoded;
}

class StringTable {
  private readonly idxByStr = new Map<string, number>();
  readonly entries: Uint8Array[] = [];

  intern(str: string): number {
    const existing = this.idxByStr.get(str);
    if (existing !== undefined) return existing;
    const idx = this.entries.length;
    this.idxByStr.set(str, idx);
    this.entries.push(encodeUtf8Cached(str));
    return idx;
  }
}

function readVarintNumber(bytes: Uint8Array, state: { pos: number }): number {
  let result = 0;
  let shift = 0;
  while (state.pos < bytes.byteLength) {
    const b = bytes[state.pos++];
    if (shift >= 28) {
      // Fallback safely via BigInt if > 28 bits
      state.pos -= 1;
      let big = BigInt(result);
      let bShift = BigInt(shift);
      while (state.pos < bytes.byteLength) {
        const nb = bytes[state.pos++];
        if (bShift >= 64n) {
          throw new LampClientError("LCB1 varint overflow", {
            code: "INVALID_ARGUMENT",
            identifier: "LCB1_VARINT_OVERFLOW",
          });
        }
        big |= BigInt(nb & 0x7f) << bShift;
        if ((nb & 0x80) === 0) return Number(big);
        bShift += 7n;
      }
      throw new LampClientError("Truncated LCB1 varint", {
        code: "INVALID_ARGUMENT",
        identifier: "LCB1_TRUNCATED",
      });
    }
    result |= (b & 0x7f) << shift;
    if ((b & 0x80) === 0) return result >>> 0;
    shift += 7;
  }
  throw new LampClientError("Truncated LCB1 varint", {
    code: "INVALID_ARGUMENT",
    identifier: "LCB1_TRUNCATED",
  });
}

function readVarint(bytes: Uint8Array, state: { pos: number }): bigint {
  let result = 0n;
  let shift = 0n;
  while (state.pos < bytes.byteLength) {
    const b = bytes[state.pos++];
    if (shift >= 64n) {
      throw new LampClientError("LCB1 varint overflow", {
        code: "INVALID_ARGUMENT",
        identifier: "LCB1_VARINT_OVERFLOW",
      });
    }
    result |= BigInt(b & 0x7f) << shift;
    if ((b & 0x80) === 0) return result;
    shift += 7n;
  }
  throw new LampClientError("Truncated LCB1 varint", {
    code: "INVALID_ARGUMENT",
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

function readLcbValue(
  bytes: Uint8Array,
  state: { pos: number },
  dict: readonly string[],
  depth = 0
): unknown {
  if (depth > 64 || state.pos >= bytes.byteLength) {
    throw new LampClientError("Corrupted LCB1 request value", {
      code: "INVALID_ARGUMENT",
      identifier: "LCB1_BAD_VALUE",
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
          code: "INVALID_ARGUMENT",
          identifier: "LCB1_TRUNCATED",
        });
      }
      const dv = new DataView(bytes.buffer, bytes.byteOffset + state.pos, 8);
      state.pos += 8;
      return dv.getFloat64(0, false);
    }
    case LCB_TAG.String: {
      const idx = readVarintNumber(bytes, state);
      if (idx < 0 || idx >= dict.length) {
        throw new LampClientError("Bad LCB1 string ref", {
          code: "INVALID_ARGUMENT",
          identifier: "LCB1_BAD_STRING",
        });
      }
      return dict[idx];
    }
    case LCB_TAG.Bytes: {
      const len = readVarintNumber(bytes, state);
      if (len < 0 || len > bytes.byteLength - state.pos) {
        throw new LampClientError("Truncated LCB1 bytes", {
          code: "INVALID_ARGUMENT",
          identifier: "LCB1_TRUNCATED",
        });
      }
      const out = bytes.slice(state.pos, state.pos + len);
      state.pos += len;
      return out;
    }
    case LCB_TAG.Array: {
      const count = readVarintNumber(bytes, state);
      const arr = new Array<unknown>(count);
      for (let i = 0; i < count; i += 1) {
        arr[i] = readLcbValue(bytes, state, dict, depth + 1);
      }
      return arr;
    }
    case LCB_TAG.Object: {
      const count = readVarintNumber(bytes, state);
      const obj: Record<string, unknown> = {};
      for (let i = 0; i < count; i += 1) {
        const keyIdx = readVarintNumber(bytes, state);
        if (keyIdx < 0 || keyIdx >= dict.length) {
          throw new LampClientError("Bad LCB1 object key ref", {
            code: "INVALID_ARGUMENT",
            identifier: "LCB1_BAD_KEY",
          });
        }
        obj[dict[keyIdx]] = readLcbValue(bytes, state, dict, depth + 1);
      }
      return obj;
    }
    default:
      throw new LampClientError(`Unknown LCB1 tag ${tag}`, {
        code: "INVALID_ARGUMENT",
        identifier: "LCB1_UNKNOWN_TAG",
      });
  }
}

function writeLcbValue(
  val: unknown,
  dict: StringTable,
  body: ByteBuilder,
  depth = 0
): void {
  if (depth > 64 || val === null || val === undefined) {
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
    if (
      Number.isInteger(val) &&
      val >= Number.MIN_SAFE_INTEGER &&
      val <= Number.MAX_SAFE_INTEGER
    ) {
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
  if (val instanceof Date) {
    body.pushByte(LCB_TAG.String);
    body.pushVarintNumber(dict.intern(val.toISOString()));
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
    const rec = val as Record<string, unknown>;
    const keys = Object.keys(rec);
    let validCount = 0;
    for (let i = 0; i < keys.length; i += 1) {
      const v = rec[keys[i]];
      if (v !== undefined && typeof v !== "function") {
        validCount += 1;
      }
    }
    body.pushByte(LCB_TAG.Object);
    body.pushVarintNumber(validCount);
    for (let i = 0; i < keys.length; i += 1) {
      const k = keys[i];
      const v = rec[k];
      if (v === undefined || typeof v === "function") continue;
      body.pushVarintNumber(dict.intern(k));
      writeLcbValue(v, dict, body, depth + 1);
    }
    return;
  }
  body.pushByte(LCB_TAG.Null);
}

/**
 * Decodes an incoming `LCB1` RPC request payload sent by `lamp-client`.
 *
 * @param bytes - Raw `LMP1` frame payload bytes.
 * @returns Decoded operation identifier, input tree, and binary files.
 */
export function decodeRpcRequestBinary(bytes: Uint8Array): DecodedNodeRpcRequest {
  if (bytes.byteLength < 3 || bytes[0] !== 0x4c || bytes[1] !== 1) {
    throw new LampClientError("Malformed LCB1 request header", {
      code: "INVALID_ARGUMENT",
      identifier: "MALFORMED_LCB1_FRAME",
    });
  }

  const state = { pos: 2 };
  const dictCount = readVarintNumber(bytes, state);
  const dict: string[] = new Array(dictCount);
  const bufView = Buffer.from(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  for (let i = 0; i < dictCount; i += 1) {
    const len = readVarintNumber(bytes, state);
    if (len < 0 || len > bytes.byteLength - state.pos) {
      throw new LampClientError("Corrupt LCB1 dictionary", {
        code: "INVALID_ARGUMENT",
        identifier: "MALFORMED_LCB1_FRAME",
      });
    }
    dict[i] = bufView.toString("utf8", state.pos, state.pos + len);
    state.pos += len;
  }

  const opIdx = readVarintNumber(bytes, state);
  const operationId = dict[opIdx] ?? "";
  const input = readLcbValue(bytes, state, dict);
  const fileCount = readVarintNumber(bytes, state);

  const files: LampFile[] = [];
  for (let i = 0; i < fileCount; i += 1) {
    const nameIdx = readVarintNumber(bytes, state);
    const mimeIdx = readVarintNumber(bytes, state);
    const dataLen = readVarintNumber(bytes, state);
    if (
      nameIdx >= dict.length ||
      mimeIdx >= dict.length ||
      dataLen < 0 ||
      dataLen > bytes.byteLength - state.pos
    ) {
      throw new LampClientError("Corrupt LCB1 request file", {
        code: "INVALID_ARGUMENT",
        identifier: "MALFORMED_LCB1_FRAME",
      });
    }
    const data = bytes.slice(state.pos, state.pos + dataLen);
    state.pos += dataLen;
    files.push({
      __lampFile: true,
      name: dict[nameIdx],
      mimeType: dict[mimeIdx],
      size: data.byteLength,
      data,
    });
  }

  return { operationId, input, files };
}

/**
 * Encodes an `LCB1` response payload readable by `lamp-client` and `lamp-client/browser`.
 *
 * @param ok - Whether the invocation succeeded.
 * @param output - Handler output value when `ok` is `true`.
 * @param errorCode - Canonical error code when `ok` is `false`.
 * @param errorIdentifier - Application error identifier when `ok` is `false`.
 * @param errorMessage - Error message when `ok` is `false`.
 * @param retryable - Whether the caller may retry on failure.
 * @returns Compact `LCB1` binary response container bytes.
 */
export function encodeRpcResponseBinary(
  ok: boolean,
  output: unknown = null,
  errorCode = "",
  errorIdentifier = "",
  errorMessage = "",
  retryable = false
): Uint8Array {
  const dict = new StringTable();
  const body = new ByteBuilder(256);

  body.pushByte(ok ? 1 : 0);
  if (ok) {
    writeLcbValue(output, dict, body);
    body.pushVarintNumber(0);
  } else {
    body.pushVarintNumber(dict.intern(errorCode));
    body.pushVarintNumber(dict.intern(errorIdentifier));
    body.pushVarintNumber(dict.intern(errorMessage));
    body.pushByte(retryable ? 1 : 0);
  }

  const bodyBytes = body.finish();
  const container = new ByteBuilder(16 + dict.entries.length * 12 + bodyBytes.byteLength);
  container.pushByte(0x4c);
  container.pushByte(1);
  container.pushVarintNumber(dict.entries.length);
  for (const entry of dict.entries) {
    container.pushVarintNumber(entry.byteLength);
    container.pushBytes(entry);
  }
  container.pushBytes(bodyBytes);
  return container.finish();
}

/**
 * Encodes a binary RFC 6455 WebSocket server frame (`FIN=1`, `opcode=0x02`).
 *
 * @param payload - Raw `LMP1` binary frame bytes to wrap for browser WebSocket clients.
 * @returns WebSocket binary wire frame bytes.
 */
export function encodeWebSocketBinaryFrame(payload: Uint8Array): Uint8Array {
  const len = payload.byteLength;
  let headerLen = 2;
  if (len > 125 && len <= 65535) headerLen = 4;
  else if (len > 65535) headerLen = 10;

  const out = new Uint8Array(headerLen + len);
  out[0] = 0x82;
  if (len <= 125) {
    out[1] = len;
  } else if (len <= 65535) {
    out[1] = 126;
    new DataView(out.buffer, out.byteOffset, 4).setUint16(2, len, false);
  } else {
    out[1] = 127;
    new DataView(out.buffer, out.byteOffset, 10).setBigUint64(2, BigInt(len), false);
  }
  out.set(payload, headerLen);
  return out;
}
