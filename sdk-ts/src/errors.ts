// ============================================================
// Lamp TypeScript SDK - Canonical Error Model
// Matches runtime/bridge/result_envelope.hpp OperationErrorCode
// ============================================================

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
  | "INTERNAL"
  | "NOT_IMPLEMENTED"
  | "CANCELLED"
  | "DEADLINE_EXCEEDED"
  | "SERIALIZATION_FAILED"
  | "EXECUTION_FAILED";

export interface LampErrorOptions {
  code?: LampErrorCode;
  identifier?: string;
  retryable?: boolean;
  details?: Record<string, unknown>;
  cause?: unknown;
}

export class LampError extends Error {
  readonly code: LampErrorCode;
  readonly identifier: string;
  readonly retryable: boolean;
  readonly details?: Record<string, unknown>;

  constructor(message: string, options: LampErrorOptions = {}) {
    super(message, options.cause !== undefined ? { cause: options.cause } : undefined);
    this.name = "LampError";
    this.code = options.code ?? "INTERNAL";
    this.identifier = options.identifier ?? this.code;
    this.retryable = options.retryable ?? (
      this.code === "UNAVAILABLE" || this.code === "RESOURCE_EXHAUSTED"
    );
    this.details = options.details;
  }

  toJSON(): {
    error: true;
    code: LampErrorCode;
    identifier: string;
    message: string;
    retryable: boolean;
  } {
    return {
      error: true,
      code: this.code,
      identifier: this.identifier,
      message: this.message,
      retryable: this.retryable,
    };
  }
}

export function isLampError(value: unknown): value is LampError {
  return value instanceof LampError;
}

export function normalizeLampError(err: unknown): LampError {
  if (err instanceof LampError) {
    return err;
  }
  if (
    typeof err === "object" &&
    err !== null &&
    "name" in err &&
    (err as { name?: unknown }).name === "AbortError"
  ) {
    return new LampError("Operation was cancelled", {
      code: "CANCELLED",
      identifier: "OPERATION_CANCELLED",
      cause: err,
    });
  }
  return new LampError("Operation execution failed", {
    code: "EXECUTION_FAILED",
    identifier: "INTERNAL_EXECUTION_ERROR",
    cause: err,
  });
}
