
#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <utility>

namespace lamp::core {

// ============================================================
// Service Registry Error Codes
// ============================================================

enum class ServiceRegistryErrorCode {
    AlreadyRegistered,
    NotRegistered,
    NullService,
    RegistryClosed,
    ConstViolation
};

// ============================================================
// Service Registry Exception
// ============================================================

class ServiceRegistryError final
    : public std::runtime_error {

public:
    ServiceRegistryError(
        ServiceRegistryErrorCode code,
        std::string message
    )
        : std::runtime_error(std::move(message)),
          code_(code) {}

    [[nodiscard]]
    ServiceRegistryErrorCode code() const noexcept {
        return code_;
    }

private:
    ServiceRegistryErrorCode code_;
};

// ============================================================
// Service Registry
//
// A thread-safe, type-safe service container.
//
// Properties:
//
// 1. Services are indexed by their unqualified C++ type.
//
// 2. Each service type has at most one registration.
//
// 3. Registrations use shared ownership.
//
// 4. Mutable services can be resolved as mutable or const.
//
// 5. Const-only services cannot be resolved as mutable.
//
// 6. Service replacement and removal preserve outstanding
//    shared_ptr references.
//
// 7. Service destruction occurs outside registry locks.
//
// 8. close() permanently prevents new mutations except
//    clear(), which remains available for cleanup.
//
// 9. No automatic dependency construction or resolution.
//
// 10. No global singleton.
//
// Lifetime contract:
//
// The registry must outlive any external code concurrently
// calling its methods. A resolved shared_ptr may outlive
// the registry itself.
//
// Services must not create shared ownership cycles.
// ============================================================

class ServiceRegistry final {
private:
    // ========================================================
    // Internal Service Entry
    // ========================================================

    struct ServiceEntry {
        std::shared_ptr<const void> instance;

        // A mutable alias exists only when the original
        // registration permitted mutable access.
        std::shared_ptr<void> mutableInstance;

        [[nodiscard]]
        bool isMutable() const noexcept {
            return static_cast<bool>(mutableInstance);
        }
    };

    using ServiceMap = std::unordered_map<
        std::type_index,
        ServiceEntry
    >;

    // ========================================================
    // Type Helpers
    // ========================================================

    template <typename Service>
    using ServiceKey = std::remove_cv_t<
        std::remove_reference_t<Service>
    >;

    template <typename Service>
    static constexpr bool validServiceType =
        !std::is_void_v<ServiceKey<Service>> &&
        !std::is_reference_v<Service> &&
        !std::is_volatile_v<
            std::remove_reference_t<Service>
        >;

    template <typename Service>
    static void validateServiceType() {
        static_assert(
            validServiceType<Service>,
            "Service must be a non-void, "
            "non-reference, non-volatile type"
        );
    }

    template <typename Service>
    [[nodiscard]]
    static std::type_index typeKey() {
        validateServiceType<Service>();

        return std::type_index(
            typeid(ServiceKey<Service>)
        );
    }

    // ========================================================
    // Entry Construction
    // ========================================================

    template <typename Service>
    [[nodiscard]]
    static ServiceEntry makeEntry(
        std::shared_ptr<Service> service
    ) {
        validateServiceType<Service>();

        if (!service) {
            throw ServiceRegistryError(
                ServiceRegistryErrorCode::NullService,
                "Cannot register a null service"
            );
        }

        ServiceEntry entry;

        entry.instance =
            std::static_pointer_cast<const void>(
                service
            );

        if constexpr (!std::is_const_v<Service>) {
            entry.mutableInstance =
                std::static_pointer_cast<void>(
                    std::move(service)
                );
        }

        return entry;
    }

    // ========================================================
    // Typed Entry Resolution
    // ========================================================

    template <typename Service>
    [[nodiscard]]
    static std::shared_ptr<Service> castEntry(
        const ServiceEntry& entry
    ) {
        validateServiceType<Service>();

        if constexpr (std::is_const_v<Service>) {
            return std::static_pointer_cast<Service>(
                entry.instance
            );
        } else {
            if (!entry.isMutable()) {
                throw ServiceRegistryError(
                    ServiceRegistryErrorCode::ConstViolation,
                    "Cannot resolve a const-only service "
                    "as mutable"
                );
            }

            return std::static_pointer_cast<Service>(
                entry.mutableInstance
            );
        }
    }

public:
    // ========================================================
    // Construction
    // ========================================================

    ServiceRegistry() = default;

    ~ServiceRegistry() {
        // Explicitly clear to release registry ownership.
        // Service destructors must not reenter a registry
        // whose own destruction has already begun.
        clear();
    }

    ServiceRegistry(const ServiceRegistry&) = delete;
    ServiceRegistry& operator=(const ServiceRegistry&) = delete;

    ServiceRegistry(ServiceRegistry&&) = delete;
    ServiceRegistry& operator=(ServiceRegistry&&) = delete;

    // ========================================================
    // Register Existing Service
    // ========================================================

    template <typename Service>
    void registerService(
        std::shared_ptr<Service> service
    ) {
        const auto key = typeKey<Service>();

        // Construct the entry before acquiring the lock.
        // Failed construction leaves the registry unchanged.
        auto entry = makeEntry(std::move(service));

        std::unique_lock lock(mutex_);

        ensureOpenLocked();

        if (services_.contains(key)) {
            throw ServiceRegistryError(
                ServiceRegistryErrorCode::AlreadyRegistered,
                "Service is already registered"
            );
        }

        services_.emplace(
            key,
            std::move(entry)
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
        validateServiceType<Interface>();
        validateServiceType<Implementation>();

        static_assert(
            std::is_convertible_v<
                Implementation*,
                Interface*
            >,
            "Implementation must be convertible "
            "to Interface"
        );

        registerService<Interface>(
            std::static_pointer_cast<Interface>(
                std::move(implementation)
            )
        );
    }

    // ========================================================
    // Construct And Register Service
    // ========================================================

    template <typename Service, typename... Args>
    [[nodiscard]]
    std::shared_ptr<Service> emplace(
        Args&&... args
    ) {
        validateServiceType<Service>();

        auto service = std::make_shared<Service>(
            std::forward<Args>(args)...
        );

        registerService<Service>(service);

        return service;
    }

    // ========================================================
    // Resolve Service
    // ========================================================

    template <typename Service>
    [[nodiscard]]
    std::shared_ptr<Service> resolve() const {
        const auto key = typeKey<Service>();

        std::shared_lock lock(mutex_);

        const auto iterator = services_.find(key);

        if (iterator == services_.end()) {
            throw ServiceRegistryError(
                ServiceRegistryErrorCode::NotRegistered,
                "Requested service is not registered"
            );
        }

        return castEntry<Service>(
            iterator->second
        );
    }

    // ========================================================
    // Optional Resolution
    //
    // Returns nullptr when:
    // - Service is missing
    // - Mutable access is requested for a const-only service
    //
    // No allocation is performed during successful lookup.
    // ========================================================

    template <typename Service>
    [[nodiscard]]
    std::shared_ptr<Service> tryResolve() const {
        const auto key = typeKey<Service>();

        std::shared_lock lock(mutex_);

        const auto iterator = services_.find(key);

        if (iterator == services_.end()) {
            return {};
        }

        if constexpr (!std::is_const_v<Service>) {
            if (!iterator->second.isMutable()) {
                return {};
            }
        }

        return castEntry<Service>(
            iterator->second
        );
    }

    // ========================================================
    // Check Registration
    // ========================================================

    template <typename Service>
    [[nodiscard]]
    bool contains() const {
        const auto key = typeKey<Service>();

        std::shared_lock lock(mutex_);

        return services_.contains(key);
    }

    // ========================================================
    // Check Mutable Availability
    // ========================================================

    template <typename Service>
    [[nodiscard]]
    bool isMutable() const {
        const auto key = typeKey<Service>();

        std::shared_lock lock(mutex_);

        const auto iterator = services_.find(key);

        if (iterator == services_.end()) {
            return false;
        }

        return iterator->second.isMutable();
    }

    // ========================================================
    // Replace Existing Service
    //
    // Strong exception safety:
    // - New entry is prepared before locking.
    // - No mutation occurs if lookup or validation fails.
    // - The old entry is released outside the lock.
    // ========================================================

    template <typename Service>
    void replace(
        std::shared_ptr<Service> service
    ) {
        const auto key = typeKey<Service>();

        auto replacement =
            makeEntry(std::move(service));

        ServiceEntry previous;

        {
            std::unique_lock lock(mutex_);

            ensureOpenLocked();

            const auto iterator = services_.find(key);

            if (iterator == services_.end()) {
                throw ServiceRegistryError(
                    ServiceRegistryErrorCode::NotRegistered,
                    "Cannot replace an unregistered service"
                );
            }

            // shared_ptr moves are noexcept.
            // Swapping entries cannot allocate.
            using std::swap;

            swap(
                previous,
                iterator->second
            );

            swap(
                iterator->second,
                replacement
            );
        }

        // The old service is held by previous.
        // Both previous and replacement are destroyed
        // after the registry lock has been released.
    }

    // ========================================================
    // Remove Service
    // ========================================================

    template <typename Service>
    [[nodiscard]]
    bool remove() {
        const auto key = typeKey<Service>();

        ServiceEntry removed;

        {
            std::unique_lock lock(mutex_);

            ensureOpenLocked();

            const auto iterator = services_.find(key);

            if (iterator == services_.end()) {
                return false;
            }

            removed = std::move(
                iterator->second
            );

            services_.erase(iterator);
        }

        // removed releases the service outside the lock.
        return true;
    }

    // ========================================================
    // Close Registry
    //
    // Closing is permanent.
    // Existing registrations remain available for lookup.
    // ========================================================

    void close() noexcept {
        std::unique_lock lock(mutex_);
        closed_ = true;
    }

    // ========================================================
    // Clear Registry
    //
    // Allowed even after close().
    //
    // Swapping the map ensures service destructors run
    // outside the registry mutex.
    // ========================================================

    void clear() {
        ServiceMap removed;

        {
            std::unique_lock lock(mutex_);

            removed.swap(services_);
        }

        removed.clear();
    }

    // ========================================================
    // Registry Inspection
    // ========================================================

    [[nodiscard]]
    std::size_t size() const noexcept {
        std::shared_lock lock(mutex_);

        return services_.size();
    }

    [[nodiscard]]
    bool empty() const noexcept {
        std::shared_lock lock(mutex_);

        return services_.empty();
    }

    [[nodiscard]]
    bool isClosed() const noexcept {
        std::shared_lock lock(mutex_);

        return closed_;
    }

private:
    // ========================================================
    // Internal State Validation
    // ========================================================

    void ensureOpenLocked() const {
        if (closed_) {
            throw ServiceRegistryError(
                ServiceRegistryErrorCode::RegistryClosed,
                "ServiceRegistry is closed"
            );
        }
    }

    // ========================================================
    // Internal Storage
    // ========================================================

    mutable std::shared_mutex mutex_;

    ServiceMap services_;

    bool closed_ = false;
};

} // namespace lamp::core
