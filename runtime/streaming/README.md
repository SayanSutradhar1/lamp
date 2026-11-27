# runtime/streaming

Bounded backpressure-aware streaming channels, pub/sub hubs, and chunked/SSE codecs.

## Key Components

- `stream_channel.hpp` — Bounded `StreamChannel` with backpressure
- `stream_hub.hpp` — Multi-subscriber `StreamHub`
- `chunked_http_codec.hpp` — RFC 9112 `ChunkedHttpEncoder`/`ChunkedHttpDecoder` and `SseCodec`
- `streaming_http_bridge.hpp` — `HttpChunkedStreamPump` adapter
