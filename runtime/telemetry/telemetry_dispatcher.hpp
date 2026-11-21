#pragma once

// ============================================================
// Lamp Framework
// Phase 12 - Observability & Production Readiness
//
// File: runtime/telemetry/telemetry_dispatcher.hpp
//
// Purpose:
//   Observability decorator around bridge::ExecutionDispatcher
//   recording per-operation request counts, in-flight gauge,
//   latency histograms, cache hit counters, and W3C spans.
// ============================================================

#include "bridge/execution_dispatcher.hpp"
#include "bridge/request_envelope.hpp"
#include "bridge/result_envelope.hpp"
#include "telemetry/metrics_registry.hpp"
#include "telemetry/tracing.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace lamp::telemetry {

class TelemetryDispatcher final {
public:
    TelemetryDispatcher(
        std::shared_ptr<bridge::ExecutionDispatcher> dispatcher,
        std::shared_ptr<MetricsRegistry> metrics = std::make_shared<MetricsRegistry>(),
        std::shared_ptr<Tracer> tracer = std::make_shared<Tracer>()
    )
        : dispatcher_(std::move(dispatcher)),
          metrics_(std::move(metrics)),
          tracer_(std::move(tracer)) {
        if (metrics_) {
            in_flight_gauge_ = metrics_->GetOrCreateGauge("lamp_operations_in_flight");
        }
    }

    [[nodiscard]]
    bridge::ResultEnvelope Dispatch(
        bridge::RequestEnvelope request,
        std::optional<SpanContext> parent_span = std::nullopt
    ) {
        if (!dispatcher_) {
            return {};
        }

        if (in_flight_gauge_) {
            in_flight_gauge_->Increment();
        }

        std::unique_ptr<SpanScope> span;
        if (tracer_) {
            span = std::make_unique<SpanScope>(
                *tracer_,
                "op:" + request.operation_id,
                std::move(parent_span)
            );
            span->SetAttribute("operation.id", request.operation_id);
            if (request.trace_id.empty()) {
                request.trace_id = span->Context().trace_id;
            }
            if (request.span_id.empty()) {
                request.span_id = span->Context().span_id;
            }
        }

        const auto started = std::chrono::steady_clock::now();
        auto result = dispatcher_->Dispatch(request);
        const auto elapsed = std::chrono::steady_clock::now() - started;
        const double duration_ms =
            std::chrono::duration<double, std::milli>(elapsed).count();

        if (in_flight_gauge_) {
            in_flight_gauge_->Decrement();
        }

        if (span) {
            span->SetAttribute("backend", std::string(BackendName(result.backend)));
            if (result.IsSuccess()) {
                span->SetStatus(SpanStatus::Ok);
            } else if (result.outcome == bridge::ResultOutcome::Cancelled) {
                span->SetStatus(SpanStatus::Cancelled, result.error.identifier);
            } else {
                span->SetStatus(SpanStatus::Error, result.error.identifier);
            }
            span->Finish();
        }

        if (metrics_) {
            const MetricLabels labels = {
                {"operation", request.operation_id},
                {"outcome", std::string(OutcomeName(result.outcome))},
                {"backend", std::string(BackendName(result.backend))}
            };
            metrics_->GetOrCreateCounter("lamp_operations_total", labels)->Increment();
            metrics_->GetOrCreateHistogram(
                "lamp_operation_duration_ms",
                {{"operation", request.operation_id}}
            )->Observe(duration_ms);

            if (result.cache_result == bridge::CacheResult::HitLocal ||
                result.cache_result == bridge::CacheResult::HitShared ||
                result.cache_result == bridge::CacheResult::Stale) {
                metrics_->GetOrCreateCounter(
                    "lamp_operation_cache_hits_total",
                    {{"operation", request.operation_id}}
                )->Increment();
            }
        }

        return result;
    }

    [[nodiscard]]
    std::shared_ptr<MetricsRegistry> Metrics() const noexcept {
        return metrics_;
    }

    [[nodiscard]]
    std::shared_ptr<Tracer> Trace() const noexcept {
        return tracer_;
    }

private:
    [[nodiscard]]
    static std::string_view OutcomeName(bridge::ResultOutcome o) noexcept {
        switch (o) {
            case bridge::ResultOutcome::Success: return "success";
            case bridge::ResultOutcome::Failure: return "failure";
            case bridge::ResultOutcome::Cancelled: return "cancelled";
            case bridge::ResultOutcome::DeadlineExceeded: return "deadline_exceeded";
        }
        return "unknown";
    }

    [[nodiscard]]
    static std::string_view BackendName(bridge::ExecutionBackend b) noexcept {
        switch (b) {
            case bridge::ExecutionBackend::Native: return "native";
            case bridge::ExecutionBackend::JavaScript: return "javascript";
            case bridge::ExecutionBackend::Cache: return "cache";
            case bridge::ExecutionBackend::Unknown: return "unknown";
        }
        return "unknown";
    }

    std::shared_ptr<bridge::ExecutionDispatcher> dispatcher_;
    std::shared_ptr<MetricsRegistry> metrics_;
    std::shared_ptr<Tracer> tracer_;
    std::shared_ptr<Gauge> in_flight_gauge_;
};

} // namespace lamp::telemetry
