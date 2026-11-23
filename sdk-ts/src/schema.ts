import { LampError } from "./errors.ts";
import type { LampFile } from "./types.ts";

/**
 * Single field-level validation issue emitted by {@link LampSchema.safeParse}.
 */
export interface LampSchemaIssue {
  /** Dot/bracket path from root (`$`) to the failing field. */
  readonly path: string;
  /** Human-readable explanation of why validation failed. */
  readonly message: string;
}

/**
 * Discriminated validation result returned by {@link LampSchema.safeParse}.
 *
 * @typeParam T - Validated output type when `ok` is `true`.
 */
export type LampSchemaResult<T> =
  | { readonly ok: true; readonly value: T }
  | { readonly ok: false; readonly issues: readonly LampSchemaIssue[] };

/**
 * Runtime contract for schema validators accepted by `app.register({ input, output })`.
 *
 * Compatible with both built-in `s.*` schemas and external schema libraries exposing `.parse()`.
 *
 * @typeParam T - Inferred TypeScript value type produced by parsing.
 */
export interface LampSchema<T> {
  /** Optional structural schema kind tag (for example `"string"`, `"file"`, `"object"`). */
  readonly kind?: string;
  /** Parses `value` and returns the typed result or throws {@link LampError}. */
  parse(value: unknown): T;
  /** Validates `value` without throwing, returning issues on failure. */
  safeParse?(value: unknown): LampSchemaResult<T>;
}

/**
 * Extracts the TypeScript type validated by a {@link LampSchema}.
 *
 * @typeParam S - Schema type whose inner value type is inferred.
 */
export type InferSchema<S> = S extends LampSchema<infer T> ? T : never;

/**
 * Validation options for {@link s.string}.
 */
export interface StringSchemaOptions {
  /** Minimum inclusive UTF-16 character length. */
  readonly minLength?: number;
  /** Maximum inclusive UTF-16 character length. */
  readonly maxLength?: number;
  /** Optional regular expression constraint. */
  readonly pattern?: RegExp;
}

/**
 * Validation options for {@link s.number}.
 */
export interface NumberSchemaOptions {
  /** Minimum inclusive numeric value. */
  readonly min?: number;
  /** Maximum inclusive numeric value. */
  readonly max?: number;
  /** Require `Number.isInteger(value)`. */
  readonly integer?: boolean;
}

/**
 * Validation options for binary {@link s.file} uploads carried natively in `LCB1`.
 */
export interface FileSchemaOptions {
  /** Maximum allowed file payload size in bytes. */
  readonly maxSizeBytes?: number;
  /** Optional whitelist of permitted MIME types (for example `["image/png"]`). */
  readonly allowedMimeTypes?: readonly string[];
}

function makeSchema<T>(
  kind: string,
  safeParseFn: (value: unknown, path: string) => LampSchemaResult<T>
): LampSchema<T> {
  return {
    kind,
    safeParse(value: unknown): LampSchemaResult<T> {
      return safeParseFn(value, "$");
    },
    parse(value: unknown): T {
      const res = safeParseFn(value, "$");
      if (res.ok) {
        return res.value;
      }
      const first = res.issues[0];
      const msg = first ? `${first.path}: ${first.message}` : "Schema validation failed";
      throw new LampError(msg, {
        code: "INVALID_ARGUMENT",
        identifier: "SCHEMA_VALIDATION_FAILED",
        details: { issues: res.issues },
      });
    },
  };
}

function safeParseInternal<T>(
  schema: LampSchema<T>,
  value: unknown,
  path: string
): LampSchemaResult<T> {
  if (typeof schema.safeParse === "function") {
    const res = schema.safeParse(value);
    if (res.ok) {
      return res;
    }
    return {
      ok: false,
      issues: res.issues.map((issue) => ({
        path:
          issue.path === "$"
            ? path
            : `${path}.${issue.path.replace(/^\$\.?/, "")}`,
        message: issue.message,
      })),
    };
  }

  try {
    return { ok: true, value: schema.parse(value) };
  } catch (err) {
    return {
      ok: false,
      issues: [
        {
          path,
          message: err instanceof Error ? err.message : "Validation failed",
        },
      ],
    };
  }
}

type InferShape<Shape extends Record<string, LampSchema<unknown>>> = {
  [K in keyof Shape]: InferSchema<Shape[K]>;
};

/**
 * Built-in server contract schema builder namespace.
 *
 * Provides structural validators for JSON-compatible types as well as native
 * zero-base64 binary {@link LampFile} attachments (`s.file()`).
 */
export const s = {
  /**
   * Validates a string value with optional length and regex pattern checks.
   *
   * @param options - Optional string length and pattern constraints.
   * @returns Schema validating `string`.
   */
  string(options: StringSchemaOptions = {}): LampSchema<string> {
    return makeSchema("string", (value, path) => {
      if (typeof value !== "string") {
        return { ok: false, issues: [{ path, message: "Expected string" }] };
      }
      if (options.minLength !== undefined && value.length < options.minLength) {
        return {
          ok: false,
          issues: [{ path, message: `Length must be >= ${options.minLength}` }],
        };
      }
      if (options.maxLength !== undefined && value.length > options.maxLength) {
        return {
          ok: false,
          issues: [{ path, message: `Length must be <= ${options.maxLength}` }],
        };
      }
      if (options.pattern !== undefined && !options.pattern.test(value)) {
        return {
          ok: false,
          issues: [{ path, message: "String does not match pattern" }],
        };
      }
      return { ok: true, value };
    });
  },

  /**
   * Validates a finite number with optional range and integer checks.
   *
   * @param options - Optional numeric bounds and integer check.
   * @returns Schema validating `number`.
   */
  number(options: NumberSchemaOptions = {}): LampSchema<number> {
    return makeSchema("number", (value, path) => {
      if (typeof value !== "number" || !Number.isFinite(value)) {
        return { ok: false, issues: [{ path, message: "Expected finite number" }] };
      }
      if (options.integer && !Number.isInteger(value)) {
        return { ok: false, issues: [{ path, message: "Expected integer" }] };
      }
      if (options.min !== undefined && value < options.min) {
        return {
          ok: false,
          issues: [{ path, message: `Number must be >= ${options.min}` }],
        };
      }
      if (options.max !== undefined && value > options.max) {
        return {
          ok: false,
          issues: [{ path, message: `Number must be <= ${options.max}` }],
        };
      }
      return { ok: true, value };
    });
  },

  /**
   * Validates a strict boolean value.
   *
   * @returns Schema validating `boolean`.
   */
  boolean(): LampSchema<boolean> {
    return makeSchema("boolean", (value, path) =>
      typeof value === "boolean"
        ? { ok: true, value }
        : { ok: false, issues: [{ path, message: "Expected boolean" }] }
    );
  },

  /**
   * Validates a literal primitive constant.
   *
   * @typeParam T - Literal type.
   * @param expected - Exact expected value.
   * @returns Schema validating `T`.
   */
  literal<T extends string | number | boolean>(expected: T): LampSchema<T> {
    return makeSchema("literal", (value, path) =>
      value === expected
        ? { ok: true, value: expected }
        : {
            ok: false,
            issues: [{ path, message: `Expected literal ${String(expected)}` }],
          }
    );
  },

  /**
   * Validates that a string matches one of the allowed enum literals.
   *
   * @typeParam T - Tuple of allowed string values.
   * @param values - Non-empty array of allowed strings.
   * @returns Schema validating `T[number]`.
   */
  enum<const T extends readonly [string, ...string[]]>(
    values: T
  ): LampSchema<T[number]> {
    return makeSchema("enum", (value, path) =>
      typeof value === "string" && values.includes(value)
        ? { ok: true, value: value as T[number] }
        : {
            ok: false,
            issues: [{ path, message: `Expected one of: ${values.join(", ")}` }],
          }
    );
  },

  /**
   * Validates a native binary file attachment decoded from `LCB1` wire frames.
   *
   * Clients pass files alongside JSON input; the C++ runtime hydrates file references
   * directly into `{ name, mimeType, size, data: Uint8Array }` with zero base64 overhead.
   *
   * @param options - Optional maximum byte length and MIME type whitelist.
   * @returns Schema validating {@link LampFile}.
   */
  file(options: FileSchemaOptions = {}): LampSchema<LampFile> {
    return makeSchema("file", (value, path) => {
      if (typeof value !== "object" || value === null || Array.isArray(value)) {
        return { ok: false, issues: [{ path, message: "Expected file object" }] };
      }
      const record = value as Record<string, unknown>;
      if (typeof record.name !== "string" || record.name.length === 0) {
        return {
          ok: false,
          issues: [{ path: `${path}.name`, message: "Expected non-empty file name" }],
        };
      }
      const rawData = record.data;
      if (!(rawData instanceof Uint8Array) && !ArrayBuffer.isView(rawData)) {
        return {
          ok: false,
          issues: [{ path: `${path}.data`, message: "Expected Uint8Array binary file data" }],
        };
      }
      const bytes =
        rawData instanceof Uint8Array
          ? rawData
          : new Uint8Array(rawData.buffer, rawData.byteOffset, rawData.byteLength);
      const mimeType =
        typeof record.mimeType === "string" && record.mimeType.length > 0
          ? record.mimeType
          : "application/octet-stream";

      if (options.maxSizeBytes !== undefined && bytes.byteLength > options.maxSizeBytes) {
        return {
          ok: false,
          issues: [
            {
              path: `${path}.data`,
              message: `File size ${bytes.byteLength} exceeds max ${options.maxSizeBytes}`,
            },
          ],
        };
      }
      if (
        options.allowedMimeTypes !== undefined &&
        !options.allowedMimeTypes.includes(mimeType)
      ) {
        return {
          ok: false,
          issues: [
            {
              path: `${path}.mimeType`,
              message: `Unsupported MIME type: ${mimeType}`,
            },
          ],
        };
      }

      return {
        ok: true,
        value: {
          __lampFile: true,
          name: record.name,
          mimeType,
          size: bytes.byteLength,
          data: bytes,
        },
      };
    });
  },

  /**
   * Validates a typed array where every element satisfies `itemSchema`.
   *
   * @typeParam T - Element type.
   * @param itemSchema - Schema applied to each array element.
   * @returns Schema validating `T[]`.
   */
  array<T>(itemSchema: LampSchema<T>): LampSchema<T[]> {
    return makeSchema("array", (value, path) => {
      if (!Array.isArray(value)) {
        return { ok: false, issues: [{ path, message: "Expected array" }] };
      }
      const out: T[] = [];
      for (let i = 0; i < value.length; i += 1) {
        const res = safeParseInternal(itemSchema, value[i], `${path}[${i}]`);
        if (!res.ok) {
          return res;
        }
        out.push(res.value);
      }
      return { ok: true, value: out };
    });
  },

  /**
   * Validates a structured plain object matching the provided property schemas.
   *
   * @typeParam Shape - Mapping of field keys to schemas.
   * @param shape - Object schema definition.
   * @returns Schema validating `{ [K in keyof Shape]: InferSchema<Shape[K]> }`.
   */
  object<Shape extends Record<string, LampSchema<any>>>(
    shape: Shape
  ): LampSchema<InferShape<Shape>> {
    return makeSchema("object", (value, path) => {
      if (typeof value !== "object" || value === null || Array.isArray(value)) {
        return { ok: false, issues: [{ path, message: "Expected plain object" }] };
      }
      const src = value as Record<string, unknown>;
      const out: Record<string, unknown> = {};
      for (const [key, fieldSchema] of Object.entries(shape)) {
        const res = safeParseInternal(fieldSchema, src[key], `${path}.${key}`);
        if (!res.ok) {
          return res;
        }
        out[key] = res.value;
      }
      return { ok: true, value: out as InferShape<Shape> };
    });
  },

  /**
   * Marks a schema as optional (`T | undefined`).
   *
   * @typeParam T - Inner schema type.
   * @param inner - Underlying schema applied when defined.
   * @returns Schema validating `T | undefined`.
   */
  optional<T>(inner: LampSchema<T>): LampSchema<T | undefined> {
    return makeSchema<T | undefined>("optional", (value, path) => {
      if (value === undefined) {
        return { ok: true, value: undefined };
      }
      return safeParseInternal(inner, value, path);
    });
  },

  /**
   * Marks a schema as nullable (`T | null`).
   *
   * @typeParam T - Inner schema type.
   * @param inner - Underlying schema applied when non-null.
   * @returns Schema validating `T | null`.
   */
  nullable<T>(inner: LampSchema<T>): LampSchema<T | null> {
    return makeSchema<T | null>("nullable", (value, path) => {
      if (value === null) {
        return { ok: true, value: null };
      }
      return safeParseInternal(inner, value, path);
    });
  },

  /**
   * Accepts any value without runtime narrowing (`unknown`).
   *
   * @returns Pass-through schema yielding `unknown`.
   */
  unknown(): LampSchema<unknown> {
    return makeSchema("unknown", (value) => ({ ok: true, value }));
  },
};

/**
 * Validates input or output data against a {@link LampSchema} and raises a
 * semantically typed {@link LampError} on mismatch.
 *
 * @typeParam T - Expected validated value type.
 * @param schema - Optional schema to enforce.
 * @param value - Unvalidated runtime value.
 * @param direction - Whether validating `"input"` (`INVALID_ARGUMENT`) or `"output"` (`SERIALIZATION_FAILED`).
 * @returns Validated value typed as `T`.
 */
export function validateWithSchema<T>(
  schema: LampSchema<T> | undefined,
  value: unknown,
  direction: "input" | "output"
): T {
  if (!schema) {
    return value as T;
  }

  if (typeof schema.safeParse === "function") {
    const res = schema.safeParse(value);
    if (res.ok) {
      return res.value;
    }
    const first = res.issues[0];
    const detail = first ? `${first.path}: ${first.message}` : "Validation failed";
    throw new LampError(
      direction === "input"
        ? `Invalid operation input (${detail})`
        : `Invalid operation output (${detail})`,
      {
        code: direction === "input" ? "INVALID_ARGUMENT" : "SERIALIZATION_FAILED",
        identifier:
          direction === "input"
            ? "INPUT_VALIDATION_FAILED"
            : "OUTPUT_VALIDATION_FAILED",
        details: { issues: res.issues },
      }
    );
  }

  try {
    return schema.parse(value);
  } catch (err) {
    if (err instanceof LampError) {
      throw new LampError(err.message, {
        code: direction === "input" ? "INVALID_ARGUMENT" : "SERIALIZATION_FAILED",
        identifier:
          direction === "input"
            ? "INPUT_VALIDATION_FAILED"
            : "OUTPUT_VALIDATION_FAILED",
        cause: err,
      });
    }
    throw new LampError(
      direction === "input" ? "Invalid operation input" : "Invalid operation output",
      {
        code: direction === "input" ? "INVALID_ARGUMENT" : "SERIALIZATION_FAILED",
        identifier:
          direction === "input"
            ? "INPUT_VALIDATION_FAILED"
            : "OUTPUT_VALIDATION_FAILED",
        cause: err,
      }
    );
  }
}
