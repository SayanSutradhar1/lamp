
#pragma once

// ============================================================
// Lamp Framework
// Phase 4 - Native Execution Bridge
//
// File: runtime/bridge/operation_registry.hpp
//
// Purpose:
//   Thread-safe, transport-independent operation registry.
//
// Dependencies:
//   - C++20 standard library
//   - operation_descriptor.hpp
//   - request_envelope.hpp
//   - result_envelope.hpp
//
// Design:
//   - No V8 or HTTP dependencies.
//   - Immutable published registrations.
//   - Concurrent lookup using shared_mutex.
//   - Stable registration snapshots.
//   - Explicit backend availability.
//   - Native handler ownership through shared_ptr.
//   - JavaScript handlers represented by opaque IDs.
//   - No execution performed by the registry.
//
// Threading:
//   - Public registry operations are thread-safe.
//   - Published registrations are immutable.
//   - Handler implementations must satisfy their declared
//     concurrency and lifetime requirements.
// ============================================================

#include "operation_descriptor.hpp"
#include "request_envelope.hpp"
#include "result_envelope.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lamp::bridge {

// ============================================================
// Native operation handler
// ============================================================

// Native handlers are invoked by the execution dispatcher.
//
// They receive a validated request and produce a result.
//
// The handler does not know whether the request originated
// from HTTP, binary RPC, or an internal invocation.
//
// Exceptions must be caught by the dispatcher and converted
// into structured operation failures.

class INativeOperationHandler {
public:
    virtual ~INativeOperationHandler() = default;

    [[nodiscard]]
    virtual ResultEnvelope Execute(
        const RequestEnvelope& request
    ) = 0;
};

// ============================================================
// JavaScript handler reference
// ============================================================

// The registry does not own V8 handles.
//
// The V8 runtime will maintain a separate mapping between
// these logical handler identifiers and isolate-local
// JavaScript functions.
//
// A handler ID must not be interpreted as a native pointer
// or as a V8 handle.

struct JavaScriptHandlerReference final {
    std::uint64_t handler_id = 0;

    [[nodiscard]]
    bool IsValid() const noexcept {
        return handler_id != 0;
    }
};

// ============================================================
// Backend availability
// ============================================================

struct OperationBackends final {
    std::shared_ptr<INativeOperationHandler> native;

    JavaScriptHandlerReference javascript;

    [[nodiscard]]
    bool HasNative() const noexcept {
        return static_cast<bool>(native);
    }

    [[nodiscard]]
    bool HasJavaScript() const noexcept {
        return javascript.IsValid();
    }

    [[nodiscard]]
    bool HasAny() const noexcept {
        return HasNative() || HasJavaScript();
    }
};

// ============================================================
// Immutable registration
// ============================================================

struct OperationRegistration final {
    OperationDescriptor descriptor;

    OperationBackends backends;

    // Monotonically increasing registry revision assigned
    // when the registration is published.
    std::uint64_t revision = 0;
};

using OperationRegistrationPtr =
    std::shared_ptr<const OperationRegistration>;

// ============================================================
// Registry status
// ============================================================

enum class RegistryStatus : std::uint8_t {
    Ok = 0,
    InvalidDescriptor,
    MissingBackend,
    BackendMismatch,
    AlreadyExists,
    NotFound,
    RevisionExhausted
};

struct RegistryResult final {
    RegistryStatus status = RegistryStatus::Ok;

    std::string message;

    std::uint64_t revision = 0;

    [[nodiscard]]
    bool Ok() const noexcept {
        return status == RegistryStatus::Ok;
    }
};

// ============================================================
// Registry
// ============================================================

class OperationRegistry final {
public:
    OperationRegistry() = default;

    OperationRegistry(
        const OperationRegistry&
    ) = delete;

    OperationRegistry& operator=(
        const OperationRegistry&
    ) = delete;

    // --------------------------------------------------------
    // Register
    // --------------------------------------------------------

    [[nodiscard]]
    RegistryResult Register(
        OperationDescriptor descriptor,
        OperationBackends backends
    ) {
        if (const auto error = descriptor.Validate()) {
            return Failure(
                RegistryStatus::InvalidDescriptor,
                error->message
            );
        }

        if (!backends.HasAny()) {
            return Failure(
                RegistryStatus::MissingBackend,
                "Operation requires at least one backend"
            );
        }

        if (!Compatible(descriptor, backends)) {
            return Failure(
                RegistryStatus::BackendMismatch,
                "Operation execution mode is incompatible "
                "with its registered backends"
            );
        }

        // Prepare allocations outside the exclusive lock.
        auto registration =
            std::make_shared<OperationRegistration>();

        registration->descriptor = std::move(descriptor);
        registration->backends = std::move(backends);

        std::unique_lock lock(mutex_);

        const auto& id = registration->descriptor.id;

        if (operations_.contains(id)) {
            return Failure(
                RegistryStatus::AlreadyExists,
                "Operation is already registered"
            );
        }

        if (next_revision_ == 0) {
            return Failure(
                RegistryStatus::RevisionExhausted,
                "Registry revision counter exhausted"
            );
        }

        // Construct the entry before advancing the revision.
        //
        // If insertion throws, the registry remains unchanged.
        auto [it, inserted] =
            operations_.try_emplace(
                id,
                OperationRegistrationPtr{}
            );

        if (!inserted) {
            return Failure(
                RegistryStatus::AlreadyExists,
                "Operation is already registered"
            );
        }

        const auto revision = next_revision_++;

        registration->revision = revision;

        // Conversion to const shared_ptr does not allocate.
        it->second = std::move(registration);

        return Success(revision);
    }

    // --------------------------------------------------------
    // Replace
    // --------------------------------------------------------

    // Replace an existing operation with a new immutable
    // registration.
    //
    // Existing readers keep their previous snapshot alive.
    //
    // This method does not wait for in-flight invocations.
    //
    // JavaScript handler references must remain valid for
    // as long as any snapshot using them may execute.

    [[nodiscard]]
    RegistryResult Replace(
        OperationDescriptor descriptor,
        OperationBackends backends
    ) {
        if (const auto error = descriptor.Validate()) {
            return Failure(
                RegistryStatus::InvalidDescriptor,
                error->message
            );
        }

        if (!backends.HasAny()) {
            return Failure(
                RegistryStatus::MissingBackend,
                "Operation requires at least one backend"
            );
        }

        if (!Compatible(descriptor, backends)) {
            return Failure(
                RegistryStatus::BackendMismatch,
                "Operation execution mode is incompatible "
                "with its registered backends"
            );
        }

        auto registration =
            std::make_shared<OperationRegistration>();

        registration->descriptor = std::move(descriptor);
        registration->backends = std::move(backends);

        std::unique_lock lock(mutex_);

        auto it = operations_.find(
            registration->descriptor.id
        );

        if (it == operations_.end()) {
            return Failure(
                RegistryStatus::NotFound,
                "Operation is not registered"
            );
        }

        if (next_revision_ == 0) {
            return Failure(
                RegistryStatus::RevisionExhausted,
                "Registry revision counter exhausted"
            );
        }

        const auto revision = next_revision_++;

        registration->revision = revision;

        // Atomic from the perspective of registry readers:
        // shared_mutex excludes concurrent lookups while
        // the published shared_ptr is replaced.
        it->second = std::move(registration);

        return Success(revision);
    }

    // --------------------------------------------------------
    // Unregister
    // --------------------------------------------------------

    [[nodiscard]]
    RegistryResult Unregister(
        std::string_view operation_id
    ) {
        std::unique_lock lock(mutex_);

        // std::string_view lookup is supported through the
        // transparent hash and equality functors.
        auto it = operations_.find(operation_id);

        if (it == operations_.end()) {
            return Failure(
                RegistryStatus::NotFound,
                "Operation is not registered"
            );
        }

        const auto revision = it->second->revision;

        operations_.erase(it);

        return Success(revision);
    }

    // --------------------------------------------------------
    // Lookup
    // --------------------------------------------------------

    [[nodiscard]]
    OperationRegistrationPtr Find(
        std::string_view operation_id
    ) const {
        std::shared_lock lock(mutex_);

        auto it = operations_.find(operation_id);

        if (it == operations_.end()) {
            return {};
        }

        // Returning a shared_ptr keeps the immutable
        // registration alive after the lock is released.
        return it->second;
    }

    [[nodiscard]]
    bool Contains(
        std::string_view operation_id
    ) const {
        std::shared_lock lock(mutex_);

        return operations_.find(operation_id) !=
               operations_.end();
    }

    // --------------------------------------------------------
    // Introspection
    // --------------------------------------------------------

    [[nodiscard]]
    std::size_t Size() const {
        std::shared_lock lock(mutex_);
        return operations_.size();
    }

    [[nodiscard]]
    bool Empty() const {
        std::shared_lock lock(mutex_);
        return operations_.empty();
    }

    [[nodiscard]]
    std::vector<OperationRegistrationPtr>
    Snapshot() const {
        std::shared_lock lock(mutex_);

        std::vector<OperationRegistrationPtr> result;
        result.reserve(operations_.size());

        for (const auto& [id, registration] : operations_) {
            (void)id;
            result.push_back(registration);
        }

        return result;
    }

    // --------------------------------------------------------
    // Clear
    // --------------------------------------------------------

    // Removes all published registrations.
    //
    // Existing snapshots remain alive.
    //
    // This does not cancel in-flight invocations.

    void Clear() {
        std::unique_lock lock(mutex_);
        operations_.clear();
    }

private:
    // --------------------------------------------------------
    // Transparent string hashing
    // --------------------------------------------------------

    struct TransparentStringHash final {
        using is_transparent = void;

        [[nodiscard]]
        std::size_t operator()(
            std::string_view value
        ) const noexcept {
            return std::hash<std::string_view>{}(value);
        }
    };

    struct TransparentStringEqual final {
        using is_transparent = void;

        [[nodiscard]]
        bool operator()(
            std::string_view lhs,
            std::string_view rhs
        ) const noexcept {
            return lhs == rhs;
        }
    };

    using OperationMap = std::unordered_map<
        std::string,
        OperationRegistrationPtr,
        TransparentStringHash,
        TransparentStringEqual
    >;

    // --------------------------------------------------------
    // Backend compatibility
    // --------------------------------------------------------

    [[nodiscard]]
    static bool Compatible(
        const OperationDescriptor& descriptor,
        const OperationBackends& backends
    ) noexcept {
        switch (descriptor.execution.mode) {
            case ExecutionMode::Native:
                return backends.HasNative();

            case ExecutionMode::JavaScript:
                return backends.HasJavaScript();

            case ExecutionMode::Adaptive:
                return backends.HasAny();
        }

        return false;
    }

    // --------------------------------------------------------
    // Result factories
    // --------------------------------------------------------

    [[nodiscard]]
    static RegistryResult Success(
        std::uint64_t revision
    ) {
        return RegistryResult{
            RegistryStatus::Ok,
            {},
            revision
        };
    }

    [[nodiscard]]
    static RegistryResult Failure(
        RegistryStatus status,
        std::string message
    ) {
        return RegistryResult{
            status,
            std::move(message),
            0
        };
    }

    // --------------------------------------------------------
    // State
    // --------------------------------------------------------

    mutable std::shared_mutex mutex_;

    OperationMap operations_;

    // Zero is reserved to indicate exhaustion.
    //
    // After UINT64_MAX has been issued, incrementing
    // wraps this counter to zero, preventing reuse.
    std::uint64_t next_revision_ = 1;
};

} // namespace lamp::bridge
