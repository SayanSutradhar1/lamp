# runtime/http

Incremental HTTP/1.0–1.1 parser, async connection management, router, middleware chain, and `HttpAsyncServer` (used for optional HTTP adapters and telemetry/health probes).

## Key Components

- `http_async_server.hpp` / `http_async_server.cpp` & `http_async_connection.hpp` — Non-blocking event-driven HTTP server
- `http_server.hpp` / `http_server.cpp` & `http_connection.hpp` — Connection and server primitives
- `http_parser.hpp`, `http_request.hpp`, `http_response.hpp`, `http_headers.hpp`, `http_method.hpp` — Incremental HTTP message parsing and serialization
- `http_router.hpp`, `http_route_group.hpp`, `http_middleware.hpp`, `http_completion_queue.hpp` — Routing and middleware pipeline
