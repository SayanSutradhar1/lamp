#pragma once

// ============================================================
// Lamp Framework
// Phase 8 - Operation Cache Bridge Contract
//
// File: runtime/bridge/operation_cache_interface.hpp
//
// Purpose:
//   Transport-independent cache interface consumed optionally
//   by ExecutionDispatcher and implemented by runtime/cache.
// ============================================================

#include "bridge/operation_descriptor.hpp"
#include "bridge/request_envelope.hpp"
#include "bridge/result_envelope.hpp"

#include <optional>

namespace lamp::bridge {

struct OperationCacheLookup final {
    CacheResult status = CacheResult::Miss;
    std::optional<ResultEnvelope> result;
    bool needs_revalidation = false;
};

class IOperationCache {
public:
    virtual ~IOperationCache() = default;

    [[nodiscard]]
    virtual OperationCacheLookup Lookup(
        const OperationDescriptor& descriptor,
        const RequestEnvelope& request
    ) = 0;

    virtual void Store(
        const OperationDescriptor& descriptor,
        const RequestEnvelope& request,
        const ResultEnvelope& result
    ) = 0;

    virtual void InvalidateOperation(
        const OperationId& operation_id
    ) = 0;
};

} // namespace lamp::bridge
