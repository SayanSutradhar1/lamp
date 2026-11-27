# runtime/telemetry

Metrics, distributed tracing, and health/readiness probes.

## Key Components

- `metrics_registry.hpp` — Cardinality-bounded `MetricsRegistry` (Prometheus text & JSON export)
- `tracing.hpp` — W3C `traceparent` `Tracer` and `SpanScope`
- `health_check.hpp` — `/healthz` and `/readyz` probe registry
- `telemetry_dispatcher.hpp` — Telemetry endpoint dispatcher
