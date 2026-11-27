# runtime/transport

Pure-binary non-REST wire transport (`LMP1` framing + `LCB1` compact binary codec) over multiplexed TCP (`lmp://`) and binary WebSocket (`ws://`).

## Key Components

- `lamp_binary_protocol.hpp` — `LMP1` 14-byte frame encoder/decoder and `LCB1` string-interned binary codec
- `lamp_binary_server.hpp` — Non-blocking multiplexed TCP + binary WebSocket server (rejects HTTP REST)
- `async_binary_converter.hpp` — Non-blocking async conversion for binary payloads and file attachments
- `wire_cache.hpp` — `FullCycleWireCache` serving pre-serialized `LCB1` responses directly from C++ with server-clamped TTLs
