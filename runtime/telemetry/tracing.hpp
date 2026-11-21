#pragma once

// ============================================================
// Lamp Framework
// Phase 12 - Observability & Production Readiness
//
// File: runtime/telemetry/tracing.hpp
//
// Purpose:
//   W3C Trace Context (traceparent) parsing/formatting,
//   lightweight RAII operation spans, and bounded in-memory
//   or custom span export.
// ============================================================

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lamp::telemetry {

enum class SpanStatus : std::uint8_t {
    Unset = 0,
    Ok = 1,
    Error = 2,
    Cancelled = 3
};

struct SpanContext final {
    std::string trace_id;       // 32 lowercase hex chars
    std::string span_id;        // 16 lowercase hex chars
    std::string parent_span_id; // empty for root span
    bool sampled = true;

    [[nodiscard]]
    bool IsValid() const noexcept {
        return trace_id.size() == 32 && span_id.size() == 16;
    }

    [[nodiscard]]
    std::string ToTraceparent() const {
        if (!IsValid()) {
            return {};
        }
        return "00-" + trace_id + "-" + span_id + "-" + (sampled ? "01" : "00");
    }
};

namespace detail {

[[nodiscard]]
inline bool IsLowerOrUpperHex(std::string_view s, bool& all_zero) noexcept {
    all_zero = true;
    for (unsigned char c : s) {
        const bool ok =
            (c >= '0' && c <= '9') ||
            (c >= 'a' && c <= 'f') ||
            (c >= 'A' && c <= 'F');
        if (!ok) {
            return false;
        }
        if (c != '0') {
            all_zero = false;
        }
    }
    return true;
}

[[nodiscard]]
inline std::string ToLowerHex(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (c >= 'A' && c <= 'F') {
            out.push_back(static_cast<char>(c - 'A' + 'a'));
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

[[nodiscard]]
inline std::string FormatHex64(std::uint64_t val) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = kHex[val & 0x0FU];
        val >>= 4U;
    }
    return out;
}

} // namespace detail

// Parses W3C `traceparent`: "00-<32hex>-<16hex>-<2hex>"
[[nodiscard]]
inline std::optional<SpanContext> ParseTraceparent(std::string_view header) {
    // Expected exact format length = 2 + 1 + 32 + 1 + 16 + 1 + 2 = 55
    if (header.size() != 55 || header[2] != '-' || header[35] != '-' || header[52] != '-') {
        return std::nullopt;
    }
    const auto version = header.substr(0, 2);
    const auto trace_id = header.substr(3, 32);
    const auto span_id = header.substr(36, 16);
    const auto flags = header.substr(53, 2);

    if (version != "00") {
        return std::nullopt;
    }

    bool trace_zero = false;
    bool span_zero = false;
    bool flags_zero = false;
    if (!detail::IsLowerOrUpperHex(trace_id, trace_zero) || trace_zero ||
        !detail::IsLowerOrUpperHex(span_id, span_zero) || span_zero ||
        !detail::IsLowerOrUpperHex(flags, flags_zero)) {
        return std::nullopt;
    }

    SpanContext ctx;
    ctx.trace_id = detail::ToLowerHex(trace_id);
    ctx.span_id = detail::ToLowerHex(span_id);
    ctx.sampled = ((flags[1] == '1') || ((flags[1] >= '0' && flags[1] <= '9') && ((flags[1] - '0') & 1) != 0));
    return ctx;
}

struct SpanRecord final {
    std::string name;
    SpanContext context;
    SpanStatus status = SpanStatus::Unset;
    std::string status_identifier;
    std::chrono::steady_clock::time_point started_at{};
    std::chrono::nanoseconds duration{0};
    std::vector<std::pair<std::string, std::string>> attributes;
};

class ISpanExporter {
public:
    virtual ~ISpanExporter() = default;
    virtual void Export(SpanRecord record) = 0;
};

class InMemorySpanExporter final : public ISpanExporter {
public:
    explicit InMemorySpanExporter(std::size_t max_retained = 1024) noexcept
        : max_retained_(max_retained == 0 ? 1 : max_retained) {}

    void Export(SpanRecord record) override {
        std::lock_guard lock(mutex_);
        if (spans_.size() >= max_retained_) {
            spans_.pop_front();
        }
        spans_.push_back(std::move(record));
    }

    [[nodiscard]]
    std::vector<SpanRecord> Snapshot() const {
        std::lock_guard lock(mutex_);
        return {spans_.begin(), spans_.end()};
    }

    void Clear() {
        std::lock_guard lock(mutex_);
        spans_.clear();
    }

private:
    std::size_t max_retained_;
    mutable std::mutex mutex_;
    std::deque<SpanRecord> spans_;
};

class Tracer final {
public:
    explicit Tracer(std::shared_ptr<ISpanExporter> exporter = {})
        : exporter_(std::move(exporter)) {}

    [[nodiscard]]
    SpanContext NextContext(
        std::optional<SpanContext> parent = std::nullopt
    ) {
        const std::uint64_t hi = seq_.fetch_add(1, std::memory_order_relaxed) ^ 0x9e3779b97f4a7c15ULL;
        const std::uint64_t lo = seq_.fetch_add(1, std::memory_order_relaxed) ^ 0xbf58476d1ce4e5b9ULL;
        const std::uint64_t sid = seq_.fetch_add(1, std::memory_order_relaxed) ^ 0x94d049bb133111ebULL;

        SpanContext ctx;
        ctx.span_id = detail::FormatHex64(sid == 0 ? 1ULL : sid);
        if (parent && parent->IsValid()) {
            ctx.trace_id = parent->trace_id;
            ctx.parent_span_id = parent->span_id;
            ctx.sampled = parent->sampled;
        } else {
            ctx.trace_id =
                detail::FormatHex64(hi == 0 ? 1ULL : hi) +
                detail::FormatHex64(lo == 0 ? 1ULL : lo);
            ctx.sampled = true;
        }
        return ctx;
    }

    void FinishSpan(SpanRecord record) {
        if (exporter_) {
            exporter_->Export(std::move(record));
        }
    }

private:
    std::shared_ptr<ISpanExporter> exporter_;
    std::atomic<std::uint64_t> seq_{1};
};

class SpanScope final {
public:
    SpanScope(
        Tracer& tracer,
        std::string name,
        std::optional<SpanContext> parent = std::nullopt
    )
        : tracer_(tracer) {
        record_.name = std::move(name);
        record_.context = tracer_.NextContext(std::move(parent));
        record_.started_at = std::chrono::steady_clock::now();
    }

    ~SpanScope() {
        Finish();
    }

    SpanScope(const SpanScope&) = delete;
    SpanScope& operator=(const SpanScope&) = delete;

    void SetAttribute(std::string key, std::string value) {
        if (!finished_) {
            record_.attributes.emplace_back(std::move(key), std::move(value));
        }
    }

    void SetStatus(SpanStatus status, std::string identifier = {}) {
        if (!finished_) {
            record_.status = status;
            record_.status_identifier = std::move(identifier);
        }
    }

    [[nodiscard]]
    const SpanContext& Context() const noexcept {
        return record_.context;
    }

    void Finish() {
        if (finished_) {
            return;
        }
        finished_ = true;
        record_.duration = std::chrono::steady_clock::now() - record_.started_at;
        if (record_.status == SpanStatus::Unset) {
            record_.status = SpanStatus::Ok;
        }
        tracer_.FinishSpan(std::move(record_));
    }

private:
    Tracer& tracer_;
    SpanRecord record_;
    bool finished_ = false;
};

} // namespace lamp::telemetry
