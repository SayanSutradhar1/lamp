#pragma once

// ============================================================
// Lamp Framework
// Host Runtime - Pure Binary Non-REST Server (`LampAppServer`)
//
// File: host/lamp_app_server.hpp
//
// Purpose:
//   Hosts the complete Lamp backend application solely in the
//   C++ native runtime:
//     - transport::LampBinaryServer (LMP1 + LCB1 over TCP and
//       binary WebSocket; rejects HTTP REST)
//     - core::DynamicWorkerPool (dynamic thread scaling + split
//       subtask parallelism)
//     - transport::FullCycleWireCache (caches entire binary
//       responses with configurable TTL + strict upper limit)
//     - LampV8RuntimeScheduler (embedded SDK contracts + V8)
// ============================================================

#include "bridge/operation_registry.hpp"
#include "cache/local_operation_cache.hpp"
#include "core/dynamic_worker_pool.hpp"
#include "host/lamp_v8_scheduler.hpp"
#include "serialization/serialization_value.hpp"
#include "telemetry/health_check.hpp"
#include "telemetry/metrics_registry.hpp"
#include "telemetry/tracing.hpp"
#include "transport/lamp_binary_protocol.hpp"
#include "transport/lamp_binary_server.hpp"
#include "transport/wire_cache.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace lamp::host {

struct LampAppServerConfig final {
    std::string host = "127.0.0.1";
    std::uint16_t port = 3000;
    std::string entry_name = "app.js";
    std::string entry_source;

    std::size_t max_connections = 2048;
    std::size_t max_outstanding_ops = 4096;

    core::DynamicWorkerPoolConfig worker_pool{};
    transport::FullCycleWireCacheConfig wire_cache{};
};

class LampAppServer final {
public:
    explicit LampAppServer(LampAppServerConfig config)
        : config_(std::move(config)),
          registry_(std::make_shared<bridge::OperationRegistry>()),
          cache_(std::make_shared<cache::LocalOperationCache>()),
          metrics_(std::make_shared<telemetry::MetricsRegistry>()),
          tracer_(std::make_shared<telemetry::Tracer>()),
          health_(std::make_shared<telemetry::HealthRegistry>()),
          worker_pool_(std::make_shared<core::DynamicWorkerPool>(config_.worker_pool)),
          wire_cache_(std::make_shared<transport::FullCycleWireCache>(config_.wire_cache)) {}

    ~LampAppServer() noexcept {
        Shutdown();
    }

    LampAppServer(const LampAppServer&) = delete;
    LampAppServer& operator=(const LampAppServer&) = delete;

    void Start() {
        bool expected = false;
        if (!started_.compare_exchange_strong(expected, true)) {
            return;
        }

        LampV8SchedulerConfig v8_cfg;
        v8_cfg.script_source = config_.entry_source;
        v8_cfg.script_name = config_.entry_name;
        v8_cfg.max_pending_tasks = config_.max_outstanding_ops;
        v8_cfg.max_parallel_isolates = config_.worker_pool.max_threads;
        v8_cfg.worker_pool = worker_pool_;

        scheduler_ = std::make_shared<LampV8RuntimeScheduler>(
            registry_,
            cache_,
            metrics_,
            tracer_,
            std::move(v8_cfg)
        );
        scheduler_->Start();

        health_->RegisterLiveness("v8_scheduler", [this] {
            if (scheduler_ && scheduler_->IsRunning()) {
                return telemetry::ComponentHealth{
                    "v8_scheduler",
                    telemetry::HealthStatus::Healthy,
                    "running"
                };
            }
            return telemetry::ComponentHealth{
                "v8_scheduler",
                telemetry::HealthStatus::Unhealthy,
                "stopped"
            };
        });

        transport::LampBinaryServerConfig bin_cfg;
        bin_cfg.host = config_.host;
        bin_cfg.port = config_.port;
        bin_cfg.max_connections = config_.max_connections;
        bin_cfg.worker_pool = config_.worker_pool;
        bin_cfg.wire_cache = config_.wire_cache;

        binary_server_ = std::make_unique<transport::LampBinaryServer>(
            [this](const transport::DecodedRpcInvocation& inv, std::uint64_t request_id) {
                return DispatchBinary(inv, request_id);
            },
            bin_cfg,
            worker_pool_,
            wire_cache_
        );
        binary_server_->Start();
    }

    void Shutdown() noexcept {
        bool expected = false;
        if (!stopped_.compare_exchange_strong(expected, true)) {
            return;
        }

        if (binary_server_) {
            try {
                binary_server_->Shutdown();
            } catch (...) {
            }
        }
        if (scheduler_) {
            try {
                scheduler_->Shutdown();
            } catch (...) {
            }
        }
    }

    [[nodiscard]]
    bool IsRunning() const noexcept {
        return binary_server_ && binary_server_->IsRunning();
    }

    [[nodiscard]]
    std::string Host() const {
        return binary_server_ ? binary_server_->Host() : config_.host;
    }

    [[nodiscard]]
    std::uint16_t Port() const noexcept {
        return binary_server_ ? binary_server_->Port() : config_.port;
    }

    [[nodiscard]]
    std::shared_ptr<bridge::OperationRegistry> Registry() const noexcept {
        return registry_;
    }

    [[nodiscard]]
    std::shared_ptr<core::DynamicWorkerPool> WorkerPool() const noexcept {
        return worker_pool_;
    }

    [[nodiscard]]
    std::shared_ptr<transport::FullCycleWireCache> WireCache() const noexcept {
        return wire_cache_;
    }

    [[nodiscard]]
    std::shared_ptr<telemetry::MetricsRegistry> Metrics() const noexcept {
        return metrics_;
    }

private:
    transport::BinaryRpcDispatchResult DispatchBinary(
        const transport::DecodedRpcInvocation& inv,
        std::uint64_t request_id
    ) {
        if (inv.operation_id == "$lamp.manifest") {
            return HandleManifest();
        }
        if (inv.operation_id == "$lamp.stats") {
            return HandleStats();
        }
        return scheduler_->ExecuteBinary(inv, request_id);
    }

    transport::BinaryRpcDispatchResult HandleManifest() const {
        const auto entries = registry_->Snapshot();
        serialization::Value::Array ops;
        ops.reserve(entries.size());
        for (const auto& entry : entries) {
            const auto& d = entry->descriptor;
            serialization::Value::Object item;
            item.emplace_back("id", serialization::Value(d.id));
            item.emplace_back("cacheEnabled", serialization::Value(d.cache.Enabled()));
            item.emplace_back(
                "cacheTtlMs",
                serialization::Value(
                    std::chrono::duration_cast<std::chrono::milliseconds>(d.cache.ttl).count()
                )
            );
            item.emplace_back(
                "timeoutMs",
                serialization::Value(static_cast<std::int64_t>(d.limits.timeout.count()))
            );
            ops.push_back(serialization::Value::MakeObject(std::move(item)));
        }

        serialization::Value::Object root;
        root.emplace_back("protocol", serialization::Value("LMP1/LCB1"));
        root.emplace_back("restPattern", serialization::Value(false));
        root.emplace_back("operations", serialization::Value::MakeArray(std::move(ops)));

        transport::DecodedRpcResult res;
        res.ok = true;
        res.output = serialization::Value::MakeObject(std::move(root));
        return transport::BinaryRpcDispatchResult{
            true,
            transport::LcbCodec::EncodeRpcResponse(res),
            std::chrono::milliseconds{0}
        };
    }

    transport::BinaryRpcDispatchResult HandleStats() const {
        const std::size_t peak_threads = (std::max)(
            worker_pool_->PeakThreadCount(),
            scheduler_ ? scheduler_->PeakSubtaskThreads() : std::size_t{0}
        );
        serialization::Value::Object root;
        root.emplace_back("activeWorkers", serialization::Value(static_cast<std::int64_t>(worker_pool_->ActiveTaskCount())));
        root.emplace_back("currentThreads", serialization::Value(static_cast<std::int64_t>(worker_pool_->CurrentThreadCount())));
        root.emplace_back("peakThreads", serialization::Value(static_cast<std::int64_t>(peak_threads)));
        root.emplace_back("completedTasks", serialization::Value(static_cast<std::int64_t>(worker_pool_->TotalCompletedTasks())));
        root.emplace_back("cacheHits", serialization::Value(static_cast<std::int64_t>(wire_cache_->Hits())));
        root.emplace_back("cacheMisses", serialization::Value(static_cast<std::int64_t>(wire_cache_->Misses())));
        root.emplace_back("cacheMaxTtlMs", serialization::Value(static_cast<std::int64_t>(wire_cache_->MaxTtlLimit().count())));

        transport::DecodedRpcResult res;
        res.ok = true;
        res.output = serialization::Value::MakeObject(std::move(root));
        return transport::BinaryRpcDispatchResult{
            true,
            transport::LcbCodec::EncodeRpcResponse(res),
            std::chrono::milliseconds{0}
        };
    }

    LampAppServerConfig config_;
    std::shared_ptr<bridge::OperationRegistry> registry_;
    std::shared_ptr<cache::LocalOperationCache> cache_;
    std::shared_ptr<telemetry::MetricsRegistry> metrics_;
    std::shared_ptr<telemetry::Tracer> tracer_;
    std::shared_ptr<telemetry::HealthRegistry> health_;
    std::shared_ptr<core::DynamicWorkerPool> worker_pool_;
    std::shared_ptr<transport::FullCycleWireCache> wire_cache_;

    std::shared_ptr<LampV8RuntimeScheduler> scheduler_;
    std::unique_ptr<transport::LampBinaryServer> binary_server_;

    std::atomic<bool> started_{false};
    std::atomic<bool> stopped_{false};
};

} // namespace lamp::host
