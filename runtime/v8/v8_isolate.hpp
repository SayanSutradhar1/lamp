#pragma once

#include "v8_platform.hpp"

#include <v8.h>

#include <cstddef>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace lamp::v8 {

// ============================================================
// V8 Isolate Errors
// ============================================================

class V8IsolateError final : public std::runtime_error {
public:
    explicit V8IsolateError(const std::string& message)
        : std::runtime_error(message) {}
};

// ============================================================
// V8 Isolate Configuration
// ============================================================

struct V8IsolateConfig final {

    // Initial heap size in megabytes.
    //
    // Zero means use the V8 default.
    std::size_t initialHeapSizeMB = 0;

    // Maximum heap size in megabytes.
    //
    // Zero means use the V8 default.
    std::size_t maxHeapSizeMB = 0;
};

// ============================================================
// V8 Isolate
// ============================================================
//
// Responsibilities:
//
// - Own one V8 isolate.
// - Own the isolate's ArrayBuffer allocator.
// - Configure heap constraints.
// - Provide safe isolate access.
// - Dispose of the isolate before its allocator.
// - Prevent accidental copying or moving.
//
// Threading:
//
// A V8 isolate must not be entered concurrently by
// multiple threads without appropriate synchronization.
//
// The intended Lamp architecture assigns an isolate
// to one execution worker at a time.
//
// This class manages lifetime, not execution locking.
//
// ============================================================

class V8Isolate final {
public:

    // ========================================================
    // Construction
    // ========================================================

    explicit V8Isolate(
        const V8IsolateConfig& config = {}
    )
        : config_(config) {

        if (!V8Platform::instance().isInitialized()) {
            throw V8IsolateError(
                "V8 platform must be initialized "
                "before creating an isolate"
            );
        }

        if (
            config_.maxHeapSizeMB != 0 &&
            config_.initialHeapSizeMB >
                config_.maxHeapSizeMB
        ) {
            throw V8IsolateError(
                "Initial heap size cannot exceed "
                "maximum heap size"
            );
        }

        // ----------------------------------------------------
        // Create ArrayBuffer Allocator
        // ----------------------------------------------------

        allocator_.reset(
            ::v8::ArrayBuffer::Allocator::NewDefaultAllocator()
        );

        if (!allocator_) {
            throw V8IsolateError(
                "Failed to create V8 ArrayBuffer allocator"
            );
        }

        // ----------------------------------------------------
        // Configure Isolate Creation
        // ----------------------------------------------------

        ::v8::Isolate::CreateParams params;

        params.array_buffer_allocator = allocator_.get();

        // ----------------------------------------------------
        // Configure Heap Constraints
        // ----------------------------------------------------

        if (
            config_.initialHeapSizeMB != 0 ||
            config_.maxHeapSizeMB != 0
        ) {
            ::v8::ResourceConstraints constraints;

            if (config_.initialHeapSizeMB != 0) {
                constraints.set_initial_old_generation_size_in_bytes(
                    config_.initialHeapSizeMB * 1024 * 1024
                );
            }

            if (config_.maxHeapSizeMB != 0) {
                constraints.set_max_old_generation_size_in_bytes(
                    config_.maxHeapSizeMB * 1024 * 1024
                );
            }

            params.constraints = constraints;
        }

        // ----------------------------------------------------
        // Create V8 Isolate
        // ----------------------------------------------------

        isolate_ = ::v8::Isolate::New(params);

        if (!isolate_) {
            throw V8IsolateError(
                "Failed to create V8 isolate"
            );
        }
    }

    // ========================================================
    // Destruction
    // ========================================================

    ~V8Isolate() noexcept {
        dispose();
    }

    // ========================================================
    // Non-Copyable / Non-Movable
    // ========================================================

    V8Isolate(const V8Isolate&) = delete;
    V8Isolate& operator=(const V8Isolate&) = delete;

    V8Isolate(V8Isolate&&) = delete;
    V8Isolate& operator=(V8Isolate&&) = delete;

    // ========================================================
    // Isolate Access
    // ========================================================

    [[nodiscard]]
    ::v8::Isolate* get() const noexcept {
        return isolate_;
    }

    [[nodiscard]]
    ::v8::Isolate& isolate() const {
        if (!isolate_) {
            throw V8IsolateError(
                "V8 isolate has already been disposed"
            );
        }

        return *isolate_;
    }

    // ========================================================
    // State Inspection
    // ========================================================

    [[nodiscard]]
    bool isValid() const noexcept {
        return isolate_ != nullptr;
    }

    [[nodiscard]]
    const V8IsolateConfig& config() const noexcept {
        return config_;
    }

    // ========================================================
    // Explicit Disposal
    // ========================================================
    //
    // IMPORTANT:
    //
    // Dispose only when:
    //
    // - No JavaScript is executing.
    // - No thread has entered this isolate.
    // - No V8 handles associated with it remain active.
    // - All contexts and persistent handles are released.
    //
    // This operation must be externally synchronized
    // against isolate access and execution.
    //
    // ========================================================

    void dispose() noexcept {
        if (!isolate_) {
            return;
        }

        isolate_->Dispose();
        isolate_ = nullptr;

        // The allocator must outlive the isolate.
        allocator_.reset();
    }

private:

    // ========================================================
    // Internal State
    // ========================================================

    V8IsolateConfig config_{};

    std::unique_ptr<::v8::ArrayBuffer::Allocator> allocator_;

    ::v8::Isolate* isolate_ = nullptr;
};

} // namespace lamp::v8