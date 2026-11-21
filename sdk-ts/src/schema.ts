// ============================================================
// Lamp TypeScript SDK - Schema & Validation Contract
// Zero external dependencies; interoperable with { parse() }.
// ============================================================

import { LampError } from "./errors.ts";

export type SchemaIssue = {
  path: string;
  message: string;
};

export type SchemaParseResult<T> =
  | { ok: true; value: T }
  | { ok: false; issues: SchemaIssue[] };

export interface LampSchema<T = unknown> {
  readonly _type?: T;
  readonly kind?: string;
  parse(value: unknown): T;
  safeParse?(value: unknown): SchemaParseResult<T>;
}

export interface ExternalParseSchema<T = unknown> {
  parse(value: unknown): T;
}

export type AnySchema<T = unknown> = LampSchema<T> | ExternalParseSchema<T>;

export type InferSchema<S> =
  S extends LampSchema<infer T>
    ? T
    : S extends ExternalParseSchema<infer T>
      ? T
      : unknown;

function makeSchema<T>(
  kind: string,
  safeParseFn: (value: unknown, path: string) => SchemaParseResult<T>
): LampSchema<T> {
  return {
    kind,
    safeParse(value: unknown): SchemaParseResult<T> {
      return safeParseFn(value, "$");
    },
    parse(value: unknown): T {
      const res = safeParseFn(value, "$");
      if (res.ok) {
        return res.value;
      }
      const first = res.issues[0];
      const msg = first
        ? `${first.path}: ${first.message}`
        : "Schema validation failed";
      throw new LampError(msg, {
        code: "INVALID_ARGUMENT",
        identifier: "SCHEMA_VALIDATION_FAILED",
        details: { issues: res.issues },
      });
    },
  };
}

export interface StringSchemaOptions {
  minLength?: number;
  maxLength?: number;
  pattern?: RegExp;
}

export interface NumberSchemaOptions {
  min?: number;
  max?: number;
  integer?: boolean;
}

export type ObjectShape = Record<string, LampSchema<unknown>>;

type InferObjectShape<Shape extends ObjectShape> = {
  [K in keyof Shape]: InferSchema<Shape[K]>;
};

function safeParseInternal<T>(
  schema: LampSchema<T>,
  value: unknown,
  path: string
): SchemaParseResult<T> {
  if (schema.safeParse) {
    const res = schema.safeParse(value);
    if (res.ok) {
      return res;
    }
    return {
      ok: false,
      issues: res.issues.map((issue) => ({
        path: issue.path === "$" ? path : `${path}.${issue.path.replace(/^\$\.?/, "")}`,
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

export const s = {
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
          issues: [{ path, message: "String does not match required pattern" }],
        };
      }
      return { ok: true, value };
    });
  },

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

  boolean(): LampSchema<boolean> {
    return makeSchema("boolean", (value, path) => {
      if (typeof value !== "boolean") {
        return { ok: false, issues: [{ path, message: "Expected boolean" }] };
      }
      return { ok: true, value };
    });
  },

  literal<const T extends string | number | boolean>(expected: T): LampSchema<T> {
    return makeSchema("literal", (value, path) => {
      if (value !== expected) {
        return {
          ok: false,
          issues: [{ path, message: `Expected literal ${String(expected)}` }],
        };
      }
      return { ok: true, value: value as T };
    });
  },

  enum<const Values extends readonly [string, ...string[]]>(
    values: Values
  ): LampSchema<Values[number]> {
    return makeSchema("enum", (value, path) => {
      if (typeof value !== "string" || !values.includes(value)) {
        return {
          ok: false,
          issues: [{ path, message: `Expected one of: ${values.join(", ")}` }],
        };
      }
      return { ok: true, value: value as Values[number] };
    });
  },

  bytes(): LampSchema<Uint8Array> {
    return makeSchema("bytes", (value, path) => {
      if (!(value instanceof Uint8Array)) {
        return { ok: false, issues: [{ path, message: "Expected Uint8Array" }] };
      }
      return { ok: true, value };
    });
  },

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

  object<Shape extends ObjectShape>(shape: Shape): LampSchema<InferObjectShape<Shape>> {
    return makeSchema("object", (value, path) => {
      if (typeof value !== "object" || value === null || Array.isArray(value)) {
        return { ok: false, issues: [{ path, message: "Expected plain object" }] };
      }
      const record = value as Record<string, unknown>;
      const out: Record<string, unknown> = {};
      for (const [key, fieldSchema] of Object.entries(shape)) {
        const res = safeParseInternal(fieldSchema, record[key], `${path}.${key}`);
        if (!res.ok) {
          return res;
        }
        out[key] = res.value;
      }
      return { ok: true, value: out as InferObjectShape<Shape> };
    });
  },

  optional<T>(inner: LampSchema<T>): LampSchema<T | undefined> {
    return makeSchema("optional", (value, path) => {
      if (value === undefined) {
        return { ok: true, value: undefined };
      }
      return safeParseInternal(inner, value, path);
    });
  },

  nullable<T>(inner: LampSchema<T>): LampSchema<T | null> {
    return makeSchema("nullable", (value, path) => {
      if (value === null) {
        return { ok: true, value: null };
      }
      return safeParseInternal(inner, value, path);
    });
  },

  unknown(): LampSchema<unknown> {
    return makeSchema("unknown", (value) => ({ ok: true, value }));
  },
};

export function validateSchema<T>(
  schema: AnySchema<T> | undefined,
  value: unknown,
  direction: "input" | "output"
): T {
  if (!schema) {
    return value as T;
  }
  if ("safeParse" in schema && typeof schema.safeParse === "function") {
    const res = schema.safeParse(value);
    if (res.ok) {
      return res.value;
    }
    const first = res.issues[0];
    const detail = first ? `${first.path}: ${first.message}` : "Validation failed";
    throw new LampError(
      direction === "input" ? `Invalid operation input (${detail})` : `Invalid operation output (${detail})`,
      {
        code: direction === "input" ? "INVALID_ARGUMENT" : "SERIALIZATION_FAILED",
        identifier:
          direction === "input" ? "INPUT_VALIDATION_FAILED" : "OUTPUT_VALIDATION_FAILED",
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
          direction === "input" ? "INPUT_VALIDATION_FAILED" : "OUTPUT_VALIDATION_FAILED",
        details: err.details,
        cause: err,
      });
    }
    throw new LampError(
      direction === "input" ? "Invalid operation input" : "Invalid operation output",
      {
        code: direction === "input" ? "INVALID_ARGUMENT" : "SERIALIZATION_FAILED",
        identifier:
          direction === "input" ? "INPUT_VALIDATION_FAILED" : "OUTPUT_VALIDATION_FAILED",
        cause: err,
      }
    );
  }
}
