#pragma once

#include <v8.h>
#include <libplatform/libplatform.h>

#include <cstddef>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace lamp::v8 {

// ============================================================
// V8 Platform Errors
// ============================================================

class V8PlatformError final : public std::runtime_error {
public:
    explicit V8PlatformError(const std::string& message)
        : std::runtime_error(message) {}
};

// ============================================================
// V8 Platform Configuration
// ============================================================

struct V8PlatformConfig final {

    // Number of worker threads used by the V8 platform.
    //
    // A value of zero lets V8 select its default.
    int threadPoolSize = 0;

    // Idle tasks are useful for V8 background maintenance.
    bool enableIdleTasks = true;
};

// ============================================================
// V8 Platform
// ============================================================
//
// Responsibilities:
//
// - Initialize the process-wide V8 platform.
// - Initialize the V8 engine.
// - Expose the underlying v8::Platform.
// - Prevent duplicate initialization.
// - Shut down V8 in the correct order.
//
// Important:
//
// V8Platform owns process-wide resources.
//
// The application must destroy all isolates, contexts,
// and V8-dependent objects before shutting down V8Platform.
//
// V8 should not be repeatedly initialized and disposed
// within the same process.
//
// ============================================================

class V8Platform final {
public:

    // ========================================================
    // Singleton Access
    // ========================================================

    [[nodiscard]]
    static V8Platform& instance() {
        static V8Platform platform;
        return platform;
    }

    V8Platform(const V8Platform&) = delete;
    V8Platform& operator=(const V8Platform&) = delete;

    V8Platform(V8Platform&&) = delete;
    V8Platform& operator=(V8Platform&&) = delete;

    // ========================================================
    // Initialize
    // ========================================================

    void initialize(
        const V8PlatformConfig& config = {}
    ) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (initialized_) {
            return;
        }

        if (disposed_) {
            throw V8PlatformError(
                "V8 cannot be reinitialized after disposal"
            );
        }

        if (config.threadPoolSize < 0) {
            throw V8PlatformError(
                "V8 thread pool size cannot be negative"
            );
        }

        // Create the default V8 platform.
        //
        // The platform must outlive every V8 isolate.

        platform_ = ::v8::platform::NewDefaultPlatform(
            config.threadPoolSize,
            config.enableIdleTasks
                ? ::v8::platform::IdleTaskSupport::kEnabled
                : ::v8::platform::IdleTaskSupport::kDisabled
        );

        if (!platform_) {
            throw V8PlatformError(
                "Failed to create V8 platform"
            );
        }

        // Register the platform with V8.

        ::v8::V8::InitializePlatform(
            platform_.get()
        );

        // Initialize the V8 engine.

        if (!::v8::V8::Initialize()) {
            ::v8::V8::DisposePlatform();
            platform_.reset();

            throw V8PlatformError(
                "Failed to initialize V8 engine"
            );
        }

        config_ = config;
        initialized_ = true;
    }

    // ========================================================
    // Shutdown
    // ========================================================

    void shutdown() {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!initialized_) {
            return;
        }

        // All isolates must already be destroyed.
        //
        // Do not call this while JavaScript execution
        // or V8 background work is still active.

        ::v8::V8::Dispose();

        ::v8::V8::DisposePlatform();

        platform_.reset();

        initialized_ = false;
        disposed_ = true;
    }

    // ========================================================
    // Platform Access
    // ========================================================

    [[nodiscard]]
    ::v8::Platform& platform() {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!initialized_ || !platform_) {
            throw V8PlatformError(
                "V8 platform has not been initialized"
            );
        }

        return *platform_;
    }

    // ========================================================
    // State Inspection
    // ========================================================

    [[nodiscard]]
    bool isInitialized() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return initialized_;
    }

    [[nodiscard]]
    bool isDisposed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return disposed_;
    }

    [[nodiscard]]
    V8PlatformConfig config() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return config_;
    }

private:

    // ========================================================
    // Construction
    // ========================================================

    V8Platform() = default;

    ~V8Platform() = default;

    // ========================================================
    // Internal State
    // ========================================================

    mutable std::mutex mutex_;

    std::unique_ptr<::v8::Platform> platform_;

    V8PlatformConfig config_{};

    bool initialized_ = false;
    bool disposed_ = false;
};

} // namespace lamp::v8