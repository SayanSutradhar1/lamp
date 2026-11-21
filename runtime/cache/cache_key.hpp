#pragma once

// ============================================================
// Lamp Framework
// Phase 8 - Caching and Performance
//
// File: runtime/cache/cache_key.hpp
//
// Purpose:
//   Deterministic, binary-safe cache key generation from
//   OperationDescriptor and RequestEnvelope.
// ============================================================

#include "bridge/operation_descriptor.hpp"
#include "bridge/request_envelope.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lamp::cache {

struct CacheKey final {
    bridge::OperationId operation_id;
    std::string bytes;
    std::uint64_t hash = 0;

    [[nodiscard]]
    friend bool operator==(
        const CacheKey& a,
        const CacheKey& b
    ) noexcept {
        return a.hash == b.hash &&
               a.operation_id == b.operation_id &&
               a.bytes == b.bytes;
    }
};

struct CacheKeyHash final {
    [[nodiscard]]
    std::size_t operator()(const CacheKey& key) const noexcept {
        return static_cast<std::size_t>(key.hash);
    }
};

class CacheKeyBuilder final {
public:
    [[nodiscard]]
    static CacheKey Build(
        const bridge::OperationDescriptor& descriptor,
        const bridge::RequestEnvelope& request,
        const std::vector<std::string>& metadata_keys = {}
    ) {
        std::string buf;
        buf.reserve(
            descriptor.id.size() +
            request.payload.size() +
            64
        );

        AppendField(buf, descriptor.id);
        buf.push_back(static_cast<char>(request.format));
        buf.push_back(static_cast<char>(descriptor.output_format));

        AppendField(buf, descriptor.input_schema.name);
        AppendU32(buf, descriptor.input_schema.version);
        AppendField(buf, descriptor.output_schema.name);
        AppendU32(buf, descriptor.output_schema.version);

        AppendU64(buf, static_cast<std::uint64_t>(request.payload.size()));
        if (!request.payload.empty()) {
            buf.append(
                reinterpret_cast<const char*>(request.payload.data()),
                request.payload.size()
            );
        }

        if (!metadata_keys.empty()) {
            std::vector<std::string> sortedKeys = metadata_keys;
            std::sort(sortedKeys.begin(), sortedKeys.end());
            for (const auto& k : sortedKeys) {
                AppendField(buf, k);
                if (const auto val = request.FindMetadata(k)) {
                    buf.push_back(1);
                    AppendField(buf, *val);
                } else {
                    buf.push_back(0);
                }
            }
        }

        CacheKey key;
        key.operation_id = descriptor.id;
        key.hash = Fnv1a64(buf);
        key.bytes = std::move(buf);
        return key;
    }

private:
    static void AppendU32(std::string& out, std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            out.push_back(
                static_cast<char>((v >> (i * 8)) & 0xFFU)
            );
        }
    }

    static void AppendU64(std::string& out, std::uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            out.push_back(
                static_cast<char>((v >> (i * 8)) & 0xFFULL)
            );
        }
    }

    static void AppendField(std::string& out, std::string_view sv) {
        AppendU64(out, static_cast<std::uint64_t>(sv.size()));
        out.append(sv.data(), sv.size());
    }

    [[nodiscard]]
    static std::uint64_t Fnv1a64(std::string_view data) noexcept {
        std::uint64_t hash = 14695981039346656037ULL;
        for (const unsigned char ch : data) {
            hash ^= ch;
            hash *= 1099511628211ULL;
        }
        return hash;
    }
};

} // namespace lamp::cache
