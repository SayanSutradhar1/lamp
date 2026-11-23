/**
 * Canonical error codes returned across the Lamp binary RPC wire protocol (`LCB1`).
 */
export type LampClientErrorCode =
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
 * Construction options accepted by {@link LampClientError}.
 */
export interface LampClientErrorOptions {
  /** Canonical semantic error category hydrated from server responses. */
  readonly code?: LampClientErrorCode | string;
  /** Stable machine-readable identifier such as `"USER_NOT_FOUND"`. */
  readonly identifier?: string;
  /** Whether automated retries are safe for this failure. */
  readonly retryable?: boolean;
  /** Underlying transport or cause value, if available. */
  readonly cause?: unknown;
}

/**
 * Structured RPC error hydrated by `lamp-client` from `LCB1` error envelopes or transport failures.
 */
export class LampClientError extends Error {
  /** Canonical semantic error code. */
  readonly code: LampClientErrorCode | string;
  /** Machine-readable diagnostic identifier. */
  readonly identifier: string;
  /** Indicates whether callers may safely retry this failed call. */
  readonly retryable: boolean;

  /**
   * Constructs a {@link LampClientError}.
   *
   * @param message - Human-readable error explanation.
   * @param options - Optional semantic error code, identifier, retryability flag, and cause.
   */
  constructor(message: string, options: LampClientErrorOptions = {}) {
    super(message, options.cause !== undefined ? { cause: options.cause } : undefined);
    this.name = "LampClientError";
    this.code = options.code ?? "INTERNAL";
    this.identifier = options.identifier ?? String(this.code);
    this.retryable =
      options.retryable ??
      (this.code === "UNAVAILABLE" || this.code === "RESOURCE_EXHAUSTED");
  }
}

/**
 * Type guard verifying whether an unknown error value is a {@link LampClientError}.
 *
 * @param err - Caught exception or rejected promise reason.
 * @returns `true` if `err` is a `LampClientError` instance.
 */
export function isLampClientError(err: unknown): err is LampClientError {
  return err instanceof LampClientError;
}
