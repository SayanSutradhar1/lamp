#pragma once

// ============================================================
// Lamp Framework
// Phase 12 - Observability & Production Readiness
//
// File: runtime/telemetry/health_check.hpp
//
// Purpose:
//   Liveness & readiness health registry and HTTP endpoint
//   adapter (/healthz, /readyz, /metrics).
// ============================================================

#include "http/http_response.hpp"
#include "http/http_router.hpp"
#include "telemetry/metrics_registry.hpp"

#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace lamp::telemetry {

enum class HealthStatus : std::uint8_t {
    Healthy = 0,
    Degraded = 1,
    Unhealthy = 2
};

[[nodiscard]]
constexpr std::string_view ToString(HealthStatus s) noexcept {
    switch (s) {
        case HealthStatus::Healthy: return "healthy";
        case HealthStatus::Degraded: return "degraded";
        case HealthStatus::Unhealthy: return "unhealthy";
    }
    return "unhealthy";
}

struct ComponentHealth final {
    std::string name;
    HealthStatus status = HealthStatus::Healthy;
    std::string message;
};

using HealthCheckFn = std::function<ComponentHealth()>;

struct HealthReport final {
    HealthStatus overall = HealthStatus::Healthy;
    std::vector<ComponentHealth> components;

    [[nodiscard]]
    bool IsServing() const noexcept {
        return overall != HealthStatus::Unhealthy;
    }

    [[nodiscard]]
    std::string ToJson() const {
        std::ostringstream out;
        out << "{\"status\":\"" << ToString(overall) << "\",\"components\":[";
        for (std::size_t i = 0; i < components.size(); ++i) {
            if (i > 0) out << ",";
            out << "{\"name\":\"" << detail::EscapeJsonString(components[i].name)
                << "\",\"status\":\"" << ToString(components[i].status)
                << "\",\"message\":\"" << detail::EscapeJsonString(components[i].message)
                << "\"}";
        }
        out << "]}";
        return out.str();
    }
};

class HealthRegistry final {
public:
    void RegisterLiveness(std::string name, HealthCheckFn fn) {
        std::unique_lock lock(mutex_);
        liveness_.emplace_back(std::move(name), std::move(fn));
    }

    void RegisterReadiness(std::string name, HealthCheckFn fn) {
        std::unique_lock lock(mutex_);
        readiness_.emplace_back(std::move(name), std::move(fn));
    }

    [[nodiscard]]
    HealthReport EvaluateLiveness() const {
        return EvaluateList(liveness_);
    }

    [[nodiscard]]
    HealthReport EvaluateReadiness() const {
        return EvaluateList(readiness_);
    }

private:
    [[nodiscard]]
    HealthReport EvaluateList(
        const std::vector<std::pair<std::string, HealthCheckFn>>& checks
    ) const {
        std::vector<std::pair<std::string, HealthCheckFn>> snapshot;
        {
            std::shared_lock lock(mutex_);
            snapshot = checks;
        }

        HealthReport report;
        for (const auto& [name, fn] : snapshot) {
            ComponentHealth entry;
            entry.name = name;
            try {
                entry = fn();
                if (entry.name.empty()) {
                    entry.name = name;
                }
            } catch (...) {
                entry.status = HealthStatus::Unhealthy;
                entry.message = "health check threw exception";
            }

            if (entry.status == HealthStatus::Unhealthy) {
                report.overall = HealthStatus::Unhealthy;
            } else if (entry.status == HealthStatus::Degraded &&
                       report.overall == HealthStatus::Healthy) {
                report.overall = HealthStatus::Degraded;
            }
            report.components.push_back(std::move(entry));
        }
        return report;
    }

    mutable std::shared_mutex mutex_;
    std::vector<std::pair<std::string, HealthCheckFn>> liveness_;
    std::vector<std::pair<std::string, HealthCheckFn>> readiness_;
};

[[nodiscard]]
inline http::HttpRouteHandler MakeLivenessHttpHandler(
    std::shared_ptr<HealthRegistry> health
) {
    return [health = std::move(health)](
               const http::HttpRequest& req,
               const http::HttpRouteContext&
           ) -> http::HttpResponse {
        const auto report = health ? health->EvaluateLiveness() : HealthReport{};
        http::HttpResponse resp(
            report.IsServing() ? http::HttpStatus::Ok : http::HttpStatus::ServiceUnavailable,
            req.version()
        );
        resp.setHeader("Content-Type", "application/json");
        resp.setHeader("Cache-Control", "no-store");
        resp.setHeader("X-Content-Type-Options", "nosniff");
        resp.setBody(report.ToJson());
        return resp;
    };
}

[[nodiscard]]
inline http::HttpRouteHandler MakeReadinessHttpHandler(
    std::shared_ptr<HealthRegistry> health
) {
    return [health = std::move(health)](
               const http::HttpRequest& req,
               const http::HttpRouteContext&
           ) -> http::HttpResponse {
        const auto report = health ? health->EvaluateReadiness() : HealthReport{};
        http::HttpResponse resp(
            report.IsServing() ? http::HttpStatus::Ok : http::HttpStatus::ServiceUnavailable,
            req.version()
        );
        resp.setHeader("Content-Type", "application/json");
        resp.setHeader("Cache-Control", "no-store");
        resp.setHeader("X-Content-Type-Options", "nosniff");
        resp.setBody(report.ToJson());
        return resp;
    };
}

[[nodiscard]]
inline http::HttpRouteHandler MakePrometheusMetricsHttpHandler(
    std::shared_ptr<MetricsRegistry> metrics
) {
    return [metrics = std::move(metrics)](
               const http::HttpRequest& req,
               const http::HttpRouteContext&
           ) -> http::HttpResponse {
        http::HttpResponse resp(http::HttpStatus::Ok, req.version());
        resp.setHeader("Content-Type", "text/plain; version=0.0.4; charset=utf-8");
        resp.setHeader("Cache-Control", "no-store");
        resp.setHeader("X-Content-Type-Options", "nosniff");
        resp.setBody(metrics ? metrics->RenderPrometheusText() : std::string{});
        return resp;
    };
}

} // namespace lamp::telemetry
