
#pragma once

#include "core/application.hpp"
#include "core/runtime_context.hpp"
#include "core/service_registry.hpp"

#include <condition_variable>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace lamp::core {

// ============================================================
// Runtime Manager State
// ============================================================

enum class RuntimeManagerState : std::uint8_t {
    Created,
    Starting,
    Running,
    Stopping,
    Stopped,
    Failed
};

// ============================================================
// Runtime Manager Error
// ============================================================

class RuntimeManagerError final
    : public std::runtime_error {

public:
    explicit RuntimeManagerError(
        const std::string& message
    )
        : std::runtime_error(message) {}
};

// ============================================================
// Runtime Manager Configuration
// ============================================================

struct RuntimeManagerConfig {
    RuntimeContextConfig runtime{};
};

// ============================================================
// Runtime Manager
//
// Owns:
//   - LampApplication
//   - RuntimeContext
//   - ServiceRegistry
//
// Lifecycle:
//   Created -> Starting -> Running
//   Running -> Stopping -> Stopped
//   Starting -> Failed
//   Stopping -> Failed
//   Failed -> Stopping -> Stopped
//
// Startup order:
//   1. RuntimeContext
//   2. LampApplication initialization
//   3. LampApplication startup
//
// Shutdown order:
//   1. LampApplication
//   2. ServiceRegistry closure
//   3. RuntimeContext
//   4. ServiceRegistry cleanup
//
// Thread safety:
//   - Lifecycle transitions are serialized.
//   - Configuration operations are serialized.
//   - Concurrent shutdown callers wait for completion.
//   - Service resolution uses ServiceRegistry locking.
//
// Restrictions:
//   - No restart after shutdown.
//   - No restart after failed startup.
//   - No synchronous lifecycle reentrancy.
//   - No shutdown from a RuntimeContext worker thread.
//   - External users must not destroy the manager while
//     another thread is accessing it.
//
// Registered services may outlive registry cleanup through
// retained shared_ptr handles.
//
// Built-in services are non-owning aliases and must not
// outlive the manager.
// ============================================================

class RuntimeManager final {
public:
    // ========================================================
    // Construction
    // ========================================================

    explicit RuntimeManager(
        RuntimeManagerConfig config = {}
    )
        : config_(std::move(config)),
          application_(
              std::make_unique<LampApplication>()
          ),
          context_(
              std::make_unique<RuntimeContext>(
                  config_.runtime
              )
          ),
          services_(
              std::make_unique<ServiceRegistry>()
          ) {
        registerBuiltInServices();
    }

    // ========================================================
    // Destruction
    // ========================================================

    ~RuntimeManager() noexcept {
        try {
            shutdown();
        } catch (...) {
            std::terminate();
        }
    }

    RuntimeManager(const RuntimeManager&) = delete;
    RuntimeManager& operator=(const RuntimeManager&) = delete;

    RuntimeManager(RuntimeManager&&) = delete;
    RuntimeManager& operator=(RuntimeManager&&) = delete;

    // ========================================================
    // Startup
    // ========================================================

    void start() {
        {
            std::unique_lock<std::mutex> lock(mutex_);

            if (state_ != RuntimeManagerState::Created) {
                throw RuntimeManagerError(
                    "RuntimeManager can only start "
                    "from the Created state"
                );
            }

            // Prevent service configuration from racing
            // with the startup transition.
            state_ = RuntimeManagerState::Starting;
        }

        try {
            context_->start();

            application_->initialize();
            application_->start();

            {
                std::lock_guard<std::mutex> lock(mutex_);
                state_ = RuntimeManagerState::Running;
            }

            condition_.notify_all();

        } catch (...) {
            const auto originalError =
                std::current_exception();

            cleanupAfterFailure();

            {
                std::lock_guard<std::mutex> lock(mutex_);
                state_ = RuntimeManagerState::Failed;
            }

            condition_.notify_all();

            std::rethrow_exception(originalError);
        }
    }

    // ========================================================
    // Shutdown
    // ========================================================

    void shutdown(bool drain = true) {
        std::unique_lock<std::mutex> lock(mutex_);

        // Wait for another lifecycle operation to finish.
        condition_.wait(lock, [this]() {
            return state_ != RuntimeManagerState::Starting &&
                   state_ != RuntimeManagerState::Stopping;
        });

        if (state_ == RuntimeManagerState::Stopped) {
            return;
        }

        // A failed startup may have already attempted
        // cleanup. Shutdown is allowed to retry.
        state_ = RuntimeManagerState::Stopping;

        lock.unlock();

        std::exception_ptr firstError;

        // ----------------------------------------------------
        // Stop Application
        // ----------------------------------------------------

        try {
            application_->shutdown();
        } catch (...) {
            firstError = std::current_exception();
        }

        // ----------------------------------------------------
        // Close Service Registry
        // ----------------------------------------------------

        services_->close();

        // ----------------------------------------------------
        // Stop Native Runtime
        // ----------------------------------------------------

        try {
            context_->shutdown(drain);
        } catch (...) {
            if (!firstError) {
                firstError = std::current_exception();
            }
        }

        // ----------------------------------------------------
        // Release Registered Services
        // ----------------------------------------------------

        try {
            services_->clear();
        } catch (...) {
            if (!firstError) {
                firstError = std::current_exception();
            }
        }

        // ----------------------------------------------------
        // Finalize Lifecycle
        // ----------------------------------------------------

        lock.lock();

        state_ = firstError
            ? RuntimeManagerState::Failed
            : RuntimeManagerState::Stopped;

        lock.unlock();
        condition_.notify_all();

        if (firstError) {
            std::rethrow_exception(firstError);
        }
    }

    // ========================================================
    // Application Access
    // ========================================================

    [[nodiscard]]
    LampApplication& application() noexcept {
        return *application_;
    }

    [[nodiscard]]
    const LampApplication& application() const noexcept {
        return *application_;
    }

    // ========================================================
    // Runtime Context Access
    // ========================================================

    [[nodiscard]]
    RuntimeContext& context() noexcept {
        return *context_;
    }

    [[nodiscard]]
    const RuntimeContext& context() const noexcept {
        return *context_;
    }

    // ========================================================
    // Service Registry Access
    // ========================================================

    [[nodiscard]]
    ServiceRegistry& services() noexcept {
        return *services_;
    }

    [[nodiscard]]
    const ServiceRegistry& services() const noexcept {
        return *services_;
    }

    // ========================================================
    // Register Existing Service
    // ========================================================

    template <typename Service>
    void registerService(
        std::shared_ptr<Service> service
    ) {
        std::lock_guard<std::mutex> lock(mutex_);

        ensureConfigurableLocked();

        services_->registerService<Service>(
            std::move(service)
        );
    }

    // ========================================================
    // Register Implementation As Interface
    // ========================================================

    template <
        typename Interface,
        typename Implementation
    >
    void registerAs(
        std::shared_ptr<Implementation> implementation
    ) {
        std::lock_guard<std::mutex> lock(mutex_);

        ensureConfigurableLocked();

        services_->registerAs<Interface>(
            std::move(implementation)
        );
    }

    // ========================================================
    // Construct And Register Service
    // ========================================================

    template <typename Service, typename... Args>
    [[nodiscard]]
    std::shared_ptr<Service> emplaceService(
        Args&&... args
    ) {
        std::lock_guard<std::mutex> lock(mutex_);

        ensureConfigurableLocked();

        return services_->emplace<Service>(
            std::forward<Args>(args)...
        );
    }

    // ========================================================
    // Resolve Service
    // ========================================================

    template <typename Service>
    [[nodiscard]]
    std::shared_ptr<Service> resolve() const {
        return services_->resolve<Service>();
    }

    // ========================================================
    // Optional Service Resolution
    // ========================================================

    template <typename Service>
    [[nodiscard]]
    std::shared_ptr<Service> tryResolve() const {
        return services_->tryResolve<Service>();
    }

    // ========================================================
    // Check Service Registration
    // ========================================================

    template <typename Service>
    [[nodiscard]]
    bool hasService() const {
        return services_->contains<Service>();
    }

    // ========================================================
    // Replace Existing Service
    // ========================================================

    template <typename Service>
    void replaceService(
        std::shared_ptr<Service> service
    ) {
        std::lock_guard<std::mutex> lock(mutex_);

        ensureConfigurableLocked();

        services_->replace<Service>(
            std::move(service)
        );
    }

    // ========================================================
    // Remove Service
    // ========================================================

    template <typename Service>
    [[nodiscard]]
    bool removeService() {
        std::lock_guard<std::mutex> lock(mutex_);

        ensureConfigurableLocked();

        return services_->remove<Service>();
    }

    // ========================================================
    // State Inspection
    // ========================================================

    [[nodiscard]]
    RuntimeManagerState state() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return state_;
    }

    [[nodiscard]]
    bool isRunning() const noexcept {
        return state() == RuntimeManagerState::Running;
    }

    [[nodiscard]]
    bool isStopping() const noexcept {
        return state() == RuntimeManagerState::Stopping;
    }

    [[nodiscard]]
    bool isStopped() const noexcept {
        return state() == RuntimeManagerState::Stopped;
    }

    [[nodiscard]]
    bool hasFailed() const noexcept {
        return state() == RuntimeManagerState::Failed;
    }

    [[nodiscard]]
    bool isAcceptingRequests() const noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);

            if (state_ != RuntimeManagerState::Running) {
                return false;
            }
        }

        return application_->isAcceptingRequests() &&
               context_->isAcceptingTasks();
    }

    [[nodiscard]]
    RuntimeManagerConfig config() const {
        return config_;
    }

private:
    // ========================================================
    // Built-In Service Registration
    //
    // These aliases do not own the underlying objects.
    // Their lifetime is bounded by RuntimeManager.
    // ========================================================

    void registerBuiltInServices() {
        services_->registerService<LampApplication>(
            std::shared_ptr<LampApplication>(
                application_.get(),
                [](LampApplication*) noexcept {}
            )
        );

        services_->registerService<RuntimeContext>(
            std::shared_ptr<RuntimeContext>(
                context_.get(),
                [](RuntimeContext*) noexcept {}
            )
        );
    }

    // ========================================================
    // Service Configuration Validation
    //
    // Caller must hold mutex_.
    // ========================================================

    void ensureConfigurableLocked() const {
        if (state_ != RuntimeManagerState::Created) {
            throw RuntimeManagerError(
                "Services can only be configured "
                "before RuntimeManager startup"
            );
        }
    }

    // ========================================================
    // Failed Startup Cleanup
    // ========================================================

    void cleanupAfterFailure() noexcept {
        try {
            application_->shutdown();
        } catch (...) {
            // Preserve the original startup exception.
        }

        services_->close();

        try {
            context_->shutdown(false);
        } catch (...) {
            // Preserve the original startup exception.
        }

        try {
            services_->clear();
        } catch (...) {
            // Preserve the original startup exception.
        }
    }

    // ========================================================
    // Configuration
    // ========================================================

    RuntimeManagerConfig config_;

    // ========================================================
    // Owned Runtime Objects
    // ========================================================

    std::unique_ptr<LampApplication> application_;
    std::unique_ptr<RuntimeContext> context_;
    std::unique_ptr<ServiceRegistry> services_;

    // ========================================================
    // Lifecycle Synchronization
    // ========================================================

    mutable std::mutex mutex_;
    std::condition_variable condition_;

    RuntimeManagerState state_ =
        RuntimeManagerState::Created;
};

} // namespace lamp::core
