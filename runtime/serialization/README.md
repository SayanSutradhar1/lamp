# runtime/serialization

Dynamic value representation, schema validation, and binary/JSON codecs.

## Key Components

- `serialization_value.hpp` — Typed dynamic `Value` (null, bool, int64, double, string, bytes, array, object)
- `schema_registry.hpp` — Operation input/output schema storage and validation
- `protobuf_codec.hpp` & `protobuf_wire.hpp` — Protobuf wire encoder/decoder
- `json_codec.hpp` — JSON codec with binary `Bytes` support
- `codec_negotiator.hpp` & `operation_codec_adapter.hpp` — Media-type/`q=` negotiation and operation adapter
