
#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace lamp::core {

// ============================================================
// Common Types
// ============================================================

using Milliseconds = std::chrono::milliseconds;
using Bytes = std::size_t;

inline constexpr Bytes KiB = 1024;
inline constexpr Bytes MiB = 1024 * KiB;
inline constexpr Bytes GiB = 1024 * MiB;

// ============================================================
// Transport Configuration
// ============================================================

enum class TransportProtocol : std::uint8_t {
    Http1,
    Http2
};

enum class SerializationFormat : std::uint8_t {
    Protobuf,
    Json
};

struct TransportConfig {
    std::string host = "127.0.0.1";
    std::uint16_t port = 8080;

    TransportProtocol protocol =
        TransportProtocol::Http2;

    SerializationFormat preferredFormat =
        SerializationFormat::Protobuf;

    bool enableJsonFallback = true;
    bool enableCompression = true;

    // Production deployments should configure TLS.
    bool enableTls = false;

    std::string tlsCertificatePath;
    std::string tlsPrivateKeyPath;

    std::uint32_t maxConnections = 10'000;

    Bytes maxRequestBodySize = 8 * MiB;

    Milliseconds requestTimeout{30'000};
    Milliseconds idleTimeout{60'000};
};

// ============================================================
// V8 Execution Configuration
// ============================================================

struct V8Config {
    bool enabled = true;

    // Zero means automatically determine worker count.
    std::uint32_t isolateCount = 0;

    // Maximum V8 heap size per isolate.
    Bytes maxHeapSizePerIsolate = 256 * MiB;

    // Maximum number of queued requests per isolate.
    std::uint32_t maxQueuedTasksPerIsolate = 1'024;

    // Maximum concurrent asynchronous requests per isolate.
    std::uint32_t maxInFlightRequestsPerIsolate = 256;

    bool enableJit = true;

    Milliseconds executionTimeout{30'000};

    // Replace isolates after an explicit runtime decision.
    bool enableIsolateRecycling = true;
};

// ============================================================
// Native CPU Worker Configuration
// ============================================================

struct NativeWorkerConfig {
    bool enabled = true;

    // Zero means automatically determine worker count.
    std::uint32_t workerCount = 0;

    std::uint32_t maxQueuedTasks = 4'096;

    bool enableSimd = true;

    // Select the appropriate implementation at runtime.
    bool enableCpuFeatureDetection = true;

    Milliseconds taskTimeout{30'000};
};

// ============================================================
// Cache Configuration
// ============================================================

enum class CacheEvictionPolicy : std::uint8_t {
    Lru,
    Fifo
};

struct L1CacheConfig {
    bool enabled = true;

    Bytes maxMemory = 256 * MiB;

    // Zero means automatically determine shard count.
    std::uint32_t shardCount = 0;

    CacheEvictionPolicy evictionPolicy =
        CacheEvictionPolicy::Lru;

    Milliseconds defaultTtl{30'000};

    bool enableRequestCoalescing = true;
};

struct L2CacheConfig {
    bool enabled = false;

    std::string endpoint = "127.0.0.1:6379";

    Milliseconds connectionTimeout{2'000};
    Milliseconds operationTimeout{1'000};
};

struct CacheConfig {
    L1CacheConfig l1;
    L2CacheConfig l2;
};

// ============================================================
// Streaming Configuration
// ============================================================

struct StreamingConfig {
    bool enabled = true;

    Bytes preferredChunkSize = 64 * KiB;

    Bytes maxBufferedBytesPerStream = 4 * MiB;

    std::uint32_t maxConcurrentStreams = 1'024;

    bool enableBackpressure = true;
};

// ============================================================
// Resource Limits
// ============================================================

struct ResourceConfig {
    Bytes maxRuntimeMemory = 2 * GiB;

    std::uint32_t maxPendingRequests = 10'000;

    Milliseconds gracefulShutdownTimeout{15'000};
};

// ============================================================
// Logging Configuration
// ============================================================

enum class LogLevel : std::uint8_t {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Critical,
    Off
};

struct LoggingConfig {
    LogLevel level = LogLevel::Info;

    bool enableStructuredLogging = true;
    bool enableRequestLogging = true;
};

// ============================================================
// Main Lamp Runtime Configuration
// ============================================================

struct LampConfig {
    std::string applicationName = "lamp-app";

    TransportConfig transport;
    V8Config v8;
    NativeWorkerConfig nativeWorkers;
    CacheConfig cache;
    StreamingConfig streaming;
    ResourceConfig resources;
    LoggingConfig logging;

    // --------------------------------------------------------
    // Hardware Detection
    // --------------------------------------------------------

    [[nodiscard]]
    static std::uint32_t hardwareThreads() noexcept {
        const auto count =
            std::thread::hardware_concurrency();

        return count == 0 ? 1u : count;
    }

    // --------------------------------------------------------
    // Resolve Automatic Worker Counts
    // --------------------------------------------------------

    [[nodiscard]]
    std::uint32_t resolvedIsolateCount() const noexcept {
        if (!v8.enabled) {
            return 0;
        }

        if (v8.isolateCount != 0) {
            return v8.isolateCount;
        }

        return std::max(
            1u,
            hardwareThreads() / 2
        );
    }

    [[nodiscard]]
    std::uint32_t resolvedNativeWorkerCount() const noexcept {
        if (!nativeWorkers.enabled) {
            return 0;
        }

        if (nativeWorkers.workerCount != 0) {
            return nativeWorkers.workerCount;
        }

        return std::max(
            1u,
            hardwareThreads() / 2
        );
    }

    // --------------------------------------------------------
    // Configuration Validation
    // --------------------------------------------------------

    [[nodiscard]]
    std::vector<std::string> validate() const {
        std::vector<std::string> errors;

        if (applicationName.empty()) {
            errors.emplace_back(
                "Application name cannot be empty."
            );
        }

        if (transport.host.empty()) {
            errors.emplace_back(
                "Transport host cannot be empty."
            );
        }

        if (transport.port == 0) {
            errors.emplace_back(
                "Transport port must be greater than zero."
            );
        }

        if (transport.maxConnections == 0) {
            errors.emplace_back(
                "Maximum connections must be greater than zero."
            );
        }

        if (transport.maxRequestBodySize == 0) {
            errors.emplace_back(
                "Maximum request body size must be positive."
            );
        }

        if (transport.requestTimeout.count() <= 0) {
            errors.emplace_back(
                "Request timeout must be positive."
            );
        }

        if (transport.enableTls) {
            if (transport.tlsCertificatePath.empty() ||
                transport.tlsPrivateKeyPath.empty()) {
                errors.emplace_back(
                    "TLS requires a certificate and private key."
                );
            }
        }

        if (v8.enabled) {
            if (v8.maxHeapSizePerIsolate == 0) {
                errors.emplace_back(
                    "V8 isolate heap limit must be positive."
                );
            }

            if (v8.maxInFlightRequestsPerIsolate == 0) {
                errors.emplace_back(
                    "V8 in-flight request limit must be positive."
                );
            }

            if (v8.executionTimeout.count() <= 0) {
                errors.emplace_back(
                    "V8 execution timeout must be positive."
                );
            }
        }

        if (nativeWorkers.enabled) {
            if (nativeWorkers.maxQueuedTasks == 0) {
                errors.emplace_back(
                    "Native task queue limit must be positive."
                );
            }

            if (nativeWorkers.taskTimeout.count() <= 0) {
                errors.emplace_back(
                    "Native task timeout must be positive."
                );
            }
        }

        if (cache.l1.enabled) {
            if (cache.l1.maxMemory == 0) {
                errors.emplace_back(
                    "L1 cache memory limit must be positive."
                );
            }

            if (cache.l1.defaultTtl.count() <= 0) {
                errors.emplace_back(
                    "L1 cache TTL must be positive."
                );
            }
        }

        if (cache.l2.enabled) {
            if (cache.l2.endpoint.empty()) {
                errors.emplace_back(
                    "L2 cache endpoint cannot be empty."
                );
            }
        }

        if (streaming.enabled) {
            if (streaming.preferredChunkSize == 0) {
                errors.emplace_back(
                    "Streaming chunk size must be positive."
                );
            }

            if (streaming.maxBufferedBytesPerStream <
                streaming.preferredChunkSize) {
                errors.emplace_back(
                    "Streaming buffer limit must be at least "
                    "the preferred chunk size."
                );
            }

            if (streaming.maxConcurrentStreams == 0) {
                errors.emplace_back(
                    "Maximum concurrent streams must be positive."
                );
            }

            if (!streaming.enableBackpressure) {
                errors.emplace_back(
                    "Streaming backpressure is required."
                );
            }
        }

        if (resources.maxRuntimeMemory == 0) {
            errors.emplace_back(
                "Runtime memory limit must be positive."
            );
        }

        if (resources.maxPendingRequests == 0) {
            errors.emplace_back(
                "Pending request limit must be positive."
            );
        }

        if (resources.gracefulShutdownTimeout.count() <= 0) {
            errors.emplace_back(
                "Graceful shutdown timeout must be positive."
            );
        }

        return errors;
    }

    [[nodiscard]]
    bool isValid() const {
        return validate().empty();
    }
};

} // namespace lamp::core
