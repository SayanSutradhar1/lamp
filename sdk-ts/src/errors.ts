/**
 * Canonical error codes returned by Lamp native runtime operations and schema validators.
 *
 * Designed around semantic failure categories rather than HTTP status numbers.
 */
export type LampErrorCode =
  | "INVALID_ARGUMENT"
  | "UNAUTHENTICATED"
  | "PERMISSION_DENIED"
  | "NOT_FOUND"
  | "ALREADY_EXISTS"
  | "CONFLICT"
  | "RESOURCE_EXHAUSTED"
  | "FAILED_PRECONDITION"
  | "UNAVAILABLE"
  | "CANCELLED"
  | "DEADLINE_EXCEEDED"
  | "EXECUTION_FAILED"
  | "SERIALIZATION_FAILED"
  | "NOT_IMPLEMENTED"
  | "INTERNAL";

/**
 * Construction options accepted by {@link LampError}.
 */
export interface LampErrorOptions {
  /** Machine-readable canonical error code. */
  readonly code?: LampErrorCode;
  /** Stable application-specific identifier such as `"USER_NOT_FOUND"`. */
  readonly identifier?: string;
  /** Whether the operation is safe for automated client retries. */
  readonly retryable?: boolean;
  /** Optional underlying error cause preserved across normalization. */
  readonly cause?: unknown;
  /** Structured diagnostic payload (for example schema validation issues). */
  readonly details?: Record<string, unknown>;
}

/**
 * Structured semantic error thrown by server operation handlers, validators, and the C++ runtime.
 *
 * Instances serialize cleanly into compact `LCB1` error envelopes over `LMP1` binary transport.
 */
export class LampError extends Error {
  /** Canonical Lamp error category. */
  readonly code: LampErrorCode;
  /** Application-defined diagnostic identifier. */
  readonly identifier: string;
  /** Indicates whether callers may safely retry this failure. */
  readonly retryable: boolean;
  /** Optional structured error details (such as field-level validation issues). */
  readonly details?: Record<string, unknown>;

  /**
   * Creates a structured {@link LampError}.
   *
   * @param message - Human-readable error message.
   * @param options - Optional error code, identifier, retryability, cause, and structured details.
   */
  constructor(message: string, options: LampErrorOptions = {}) {
    super(message, options.cause !== undefined ? { cause: options.cause } : undefined);
    this.name = "LampError";
    this.code = options.code ?? "INTERNAL";
    this.identifier = options.identifier ?? this.code;
    this.retryable =
      options.retryable ??
      (this.code === "UNAVAILABLE" || this.code === "RESOURCE_EXHAUSTED");
    this.details = options.details;
  }

  /**
   * Serializes the error into a wire-friendly JSON/LCB1 record.
   *
   * @returns Plain representation of the error code, identifier, message, and retry flag.
   */
  toJSON(): Record<string, unknown> {
    return {
      error: true,
      code: this.code,
      identifier: this.identifier,
      message: this.message,
      retryable: this.retryable,
    };
  }
}

/**
 * Type guard checking whether an unknown thrown value is a {@link LampError}.
 *
 * @param value - Unknown caught exception or value.
 * @returns `true` if `value` is a `LampError` instance.
 */
export function isLampError(value: unknown): value is LampError {
  return value instanceof LampError;
}

/**
 * Normalizes any thrown JS value into a structured {@link LampError}.
 *
 * @param err - Caught exception from an operation handler or hook.
 * @returns Either the original {@link LampError} or a wrapped `EXECUTION_FAILED` error.
 */
export function normalizeLampError(err: unknown): LampError {
  if (isLampError(err)) {
    return err;
  }

  if (err instanceof Error) {
    return new LampError(err.message, {
      code: "EXECUTION_FAILED",
      identifier: err.name || "JS_EXCEPTION",
      cause: err,
    });
  }

  return new LampError("Operation execution failed with non-Error value", {
    code: "EXECUTION_FAILED",
    identifier: "NON_ERROR_THROW",
    details: { thrownType: typeof err },
  });
}
