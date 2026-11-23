/**
 * Lamp Client Contract SDK (`lamp-client`).
 *
 * Implements high-speed binary `LMP1` + `LCB1` communication directly against the
 * C++ `lamp_server` runtime over persistent TCP (Node/CLI) or binary WebSockets (`lamp-client/browser`),
 * supporting async streaming conversion of JSON and binary file payloads with zero REST endpoints.
 *
 * @packageDocumentation
 */

export { createLampClient } from "./node_client.ts";
export { LampClientError, isLampClientError } from "./errors.ts";
export {
  LMP_HEADER_BYTES,
  MAX_LMP_PAYLOAD_BYTES,
  LmpCodecKind,
  LmpFrameDecoder,
  LmpFrameFlags,
  LmpFrameType,
  decodeRpcResponseBinary,
  encodeLmpFrame,
  encodeRpcRequestBinary,
  encodeRpcRequestStreamAsync,
  normalizeFileInputsAsync,
} from "./binary_protocol.ts";

export type {
  LampClientErrorCode,
  LampClientErrorOptions,
} from "./errors.ts";
export type {
  DecodedBinaryRpcResult,
  LmpFrame,
  LmpFrameHeader,
  LmpFrameTypeValue,
  NormalizedRpcInput,
} from "./binary_protocol.ts";
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
