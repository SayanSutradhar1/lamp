#pragma once

// ============================================================
// Lamp Framework
// Phase 12 - Observability & Production Readiness
//
// File: runtime/telemetry/metrics_registry.hpp
//
// Purpose:
//   Thread-safe counters, gauges, latency histograms with
//   percentile estimates, cardinality-bounded labeled series,
//   and Prometheus / JSON exposition renderers.
// ============================================================

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lamp::telemetry {

using MetricLabels = std::vector<std::pair<std::string, std::string>>;

namespace detail {

[[nodiscard]]
inline std::string EscapePrometheusLabelValue(std::string_view v) {
    std::string out;
    out.reserve(v.size());
    for (unsigned char c : v) {
        if (c == '\\' || c == '"') {
            out.push_back('\\');
            out.push_back(static_cast<char>(c));
        } else if (c == '\n') {
            out.append("\\n");
        } else if (c >= 0x20) {
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

[[nodiscard]]
inline std::string EscapeJsonString(std::string_view v) {
    std::string out;
    out.reserve(v.size());
    for (unsigned char c : v) {
        if (c == '\\' || c == '"') {
            out.push_back('\\');
            out.push_back(static_cast<char>(c));
        } else if (c == '\n') {
            out.append("\\n");
        } else if (c == '\r') {
            out.append("\\r");
        } else if (c == '\t') {
            out.append("\\t");
        } else if (c >= 0x20) {
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

[[nodiscard]]
inline std::string FormatLabelKey(const MetricLabels& labels) {
    if (labels.empty()) {
        return {};
    }
    MetricLabels sorted = labels;
    std::sort(
        sorted.begin(),
        sorted.end(),
        [](const auto& a, const auto& b) { return a.first < b.first; }
    );
    std::string key;
    for (std::size_t i = 0; i < sorted.size(); ++i) {
        if (i > 0) {
            key.push_back(',');
        }
        key.append(sorted[i].first)
           .append("=\"")
           .append(EscapePrometheusLabelValue(sorted[i].second))
           .push_back('"');
    }
    return key;
}

} // namespace detail

// ============================================================
// Atomic Counter & Gauge
// ============================================================

class Counter final {
public:
    void Increment(std::uint64_t delta = 1) noexcept {
        value_.fetch_add(delta, std::memory_order_relaxed);
    }

    [[nodiscard]]
    std::uint64_t Value() const noexcept {
        return value_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<std::uint64_t> value_{0};
};

class Gauge final {
public:
    void Set(std::int64_t v) noexcept {
        value_.store(v, std::memory_order_relaxed);
    }

    void Increment(std::int64_t delta = 1) noexcept {
        value_.fetch_add(delta, std::memory_order_relaxed);
    }

    void Decrement(std::int64_t delta = 1) noexcept {
        value_.fetch_sub(delta, std::memory_order_relaxed);
    }

    [[nodiscard]]
    std::int64_t Value() const noexcept {
        return value_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<std::int64_t> value_{0};
};

// ============================================================
// Latency / Distribution Histogram
// ============================================================

struct HistogramSnapshot final {
    std::vector<double> upper_bounds;
    std::vector<std::uint64_t> cumulative_counts;
    std::uint64_t total_count = 0;
    double sum = 0.0;

    [[nodiscard]]
    double QuantileEstimate(double q) const noexcept {
        if (total_count == 0 || upper_bounds.empty()) {
            return 0.0;
        }
        if (q <= 0.0) {
            return 0.0;
        }
        if (q >= 1.0) {
            return upper_bounds.back();
        }

        const double target = q * static_cast<double>(total_count);
        for (std::size_t i = 0; i < upper_bounds.size(); ++i) {
            if (static_cast<double>(cumulative_counts[i]) >= target) {
                return upper_bounds[i];
            }
        }
        return upper_bounds.back();
    }
};

class Histogram final {
public:
    static inline const std::vector<double> kDefaultDurationMsBuckets = {
        0.5, 1.0, 2.5, 5.0, 10.0, 25.0, 50.0, 100.0, 250.0, 500.0, 1000.0, 5000.0
    };

    explicit Histogram(std::vector<double> bounds = kDefaultDurationMsBuckets)
        : bounds_(std::move(bounds)) {
        std::sort(bounds_.begin(), bounds_.end());
        bounds_.erase(std::unique(bounds_.begin(), bounds_.end()), bounds_.end());
        bucket_counts_.assign(bounds_.size() + 1U, 0);
    }

    void Observe(double value) {
        if (!std::isfinite(value)) {
            return;
        }
        std::lock_guard lock(mutex_);
        ++total_count_;
        sum_ += value;
        const auto it = std::lower_bound(bounds_.begin(), bounds_.end(), value);
        const std::size_t idx = static_cast<std::size_t>(it - bounds_.begin());
        ++bucket_counts_[idx];
    }

    [[nodiscard]]
    HistogramSnapshot Snapshot() const {
        std::lock_guard lock(mutex_);
        HistogramSnapshot snap;
        snap.upper_bounds = bounds_;
        snap.total_count = total_count_;
        snap.sum = sum_;
        snap.cumulative_counts.reserve(bucket_counts_.size());

        std::uint64_t cumulative = 0;
        for (const auto count : bucket_counts_) {
            cumulative += count;
            snap.cumulative_counts.push_back(cumulative);
        }
        return snap;
    }

private:
    mutable std::mutex mutex_;
    std::vector<double> bounds_;
    std::vector<std::uint64_t> bucket_counts_;
    std::uint64_t total_count_ = 0;
    double sum_ = 0.0;
};

// ============================================================
// Metrics Registry (Cardinality-Bounded)
// ============================================================

struct MetricsConfig final {
    std::size_t max_series = 4096;
};

class MetricsRegistry final {
public:
    explicit MetricsRegistry(MetricsConfig config = {}) noexcept
        : config_(config) {
        if (config_.max_series == 0) {
            config_.max_series = 1;
        }
    }

    [[nodiscard]]
    std::shared_ptr<Counter> GetOrCreateCounter(
        std::string_view name,
        const MetricLabels& labels = {}
    ) {
        const std::string label_key = detail::FormatLabelKey(labels);
        const std::string full_key = MakeSeriesKey("counter", name, label_key);

        {
            std::shared_lock lock(mutex_);
            if (const auto it = counters_.find(full_key); it != counters_.end()) {
                return it->second.metric;
            }
        }

        std::unique_lock lock(mutex_);
        if (const auto it = counters_.find(full_key); it != counters_.end()) {
            return it->second.metric;
        }
        if (TotalSeriesLocked() >= config_.max_series) {
            overflow_drops_.fetch_add(1, std::memory_order_relaxed);
            return fallback_counter_;
        }

        auto counter = std::make_shared<Counter>();
        counters_.emplace(
            full_key,
            CounterEntry{std::string(name), label_key, counter}
        );
        return counter;
    }

    [[nodiscard]]
    std::shared_ptr<Gauge> GetOrCreateGauge(
        std::string_view name,
        const MetricLabels& labels = {}
    ) {
        const std::string label_key = detail::FormatLabelKey(labels);
        const std::string full_key = MakeSeriesKey("gauge", name, label_key);

        {
            std::shared_lock lock(mutex_);
            if (const auto it = gauges_.find(full_key); it != gauges_.end()) {
                return it->second.metric;
            }
        }

        std::unique_lock lock(mutex_);
        if (const auto it = gauges_.find(full_key); it != gauges_.end()) {
            return it->second.metric;
        }
        if (TotalSeriesLocked() >= config_.max_series) {
            overflow_drops_.fetch_add(1, std::memory_order_relaxed);
            return fallback_gauge_;
        }

        auto gauge = std::make_shared<Gauge>();
        gauges_.emplace(
            full_key,
            GaugeEntry{std::string(name), label_key, gauge}
        );
        return gauge;
    }

    [[nodiscard]]
    std::shared_ptr<Histogram> GetOrCreateHistogram(
        std::string_view name,
        const MetricLabels& labels = {},
        std::vector<double> bounds = Histogram::kDefaultDurationMsBuckets
    ) {
        const std::string label_key = detail::FormatLabelKey(labels);
        const std::string full_key = MakeSeriesKey("histogram", name, label_key);

        {
            std::shared_lock lock(mutex_);
            if (const auto it = histograms_.find(full_key); it != histograms_.end()) {
                return it->second.metric;
            }
        }

        std::unique_lock lock(mutex_);
        if (const auto it = histograms_.find(full_key); it != histograms_.end()) {
            return it->second.metric;
        }
        if (TotalSeriesLocked() >= config_.max_series) {
            overflow_drops_.fetch_add(1, std::memory_order_relaxed);
            return fallback_histogram_;
        }

        auto hist = std::make_shared<Histogram>(std::move(bounds));
        histograms_.emplace(
            full_key,
            HistogramEntry{std::string(name), label_key, hist}
        );
        return hist;
    }

    [[nodiscard]]
    std::size_t SeriesCount() const {
        std::shared_lock lock(mutex_);
        return TotalSeriesLocked();
    }

    [[nodiscard]]
    std::uint64_t SeriesOverflowCount() const noexcept {
        return overflow_drops_.load(std::memory_order_relaxed);
    }

    [[nodiscard]]
    std::string RenderPrometheusText() const {
        std::shared_lock lock(mutex_);
        std::ostringstream out;

        for (const auto& [_, entry] : counters_) {
            out << entry.name;
            if (!entry.label_key.empty()) {
                out << "{" << entry.label_key << "}";
            }
            out << " " << entry.metric->Value() << "\n";
        }

        for (const auto& [_, entry] : gauges_) {
            out << entry.name;
            if (!entry.label_key.empty()) {
                out << "{" << entry.label_key << "}";
            }
            out << " " << entry.metric->Value() << "\n";
        }

        for (const auto& [_, entry] : histograms_) {
            const auto snap = entry.metric->Snapshot();
            for (std::size_t i = 0; i < snap.upper_bounds.size(); ++i) {
                out << entry.name << "_bucket{";
                if (!entry.label_key.empty()) {
                    out << entry.label_key << ",";
                }
                out << "le=\"" << snap.upper_bounds[i] << "\"} "
                    << snap.cumulative_counts[i] << "\n";
            }
            out << entry.name << "_bucket{";
            if (!entry.label_key.empty()) {
                out << entry.label_key << ",";
            }
            out << "le=\"+Inf\"} " << snap.total_count << "\n";

            out << entry.name << "_sum";
            if (!entry.label_key.empty()) {
                out << "{" << entry.label_key << "}";
            }
            out << " " << snap.sum << "\n";

            out << entry.name << "_count";
            if (!entry.label_key.empty()) {
                out << "{" << entry.label_key << "}";
            }
            out << " " << snap.total_count << "\n";
        }

        return out.str();
    }

    [[nodiscard]]
    std::string RenderJsonSnapshot() const {
        std::shared_lock lock(mutex_);
        std::ostringstream out;
        out << "{\"series_count\":" << TotalSeriesLocked()
            << ",\"overflow_drops\":" << overflow_drops_.load(std::memory_order_relaxed)
            << ",\"counters\":[";

        bool first = true;
        for (const auto& [_, entry] : counters_) {
            if (!first) out << ",";
            first = false;
            out << "{\"name\":\"" << detail::EscapeJsonString(entry.name)
                << "\",\"labels\":\"" << detail::EscapeJsonString(entry.label_key)
                << "\",\"value\":" << entry.metric->Value() << "}";
        }
        out << "],\"gauges\":[";
        first = true;
        for (const auto& [_, entry] : gauges_) {
            if (!first) out << ",";
            first = false;
            out << "{\"name\":\"" << detail::EscapeJsonString(entry.name)
                << "\",\"labels\":\"" << detail::EscapeJsonString(entry.label_key)
                << "\",\"value\":" << entry.metric->Value() << "}";
        }
        out << "],\"histograms\":[";
        first = true;
        for (const auto& [_, entry] : histograms_) {
            if (!first) out << ",";
            first = false;
            const auto snap = entry.metric->Snapshot();
            out << "{\"name\":\"" << detail::EscapeJsonString(entry.name)
                << "\",\"labels\":\"" << detail::EscapeJsonString(entry.label_key)
                << "\",\"count\":" << snap.total_count
                << ",\"sum\":" << snap.sum
                << ",\"p95_ms\":" << snap.QuantileEstimate(0.95) << "}";
        }
        out << "]}";
        return out.str();
    }

private:
    struct CounterEntry final {
        std::string name;
        std::string label_key;
        std::shared_ptr<Counter> metric;
    };
    struct GaugeEntry final {
        std::string name;
        std::string label_key;
        std::shared_ptr<Gauge> metric;
    };
    struct HistogramEntry final {
        std::string name;
        std::string label_key;
        std::shared_ptr<Histogram> metric;
    };

    [[nodiscard]]
    std::size_t TotalSeriesLocked() const noexcept {
        return counters_.size() + gauges_.size() + histograms_.size();
    }

    [[nodiscard]]
    static std::string MakeSeriesKey(
        std::string_view kind,
        std::string_view name,
        std::string_view label_key
    ) {
        std::string k;
        k.reserve(kind.size() + name.size() + label_key.size() + 2U);
        k.append(kind).push_back('|');
        k.append(name).push_back('|');
        k.append(label_key);
        return k;
    }

    MetricsConfig config_;
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, CounterEntry> counters_;
    std::unordered_map<std::string, GaugeEntry> gauges_;
    std::unordered_map<std::string, HistogramEntry> histograms_;
    std::atomic<std::uint64_t> overflow_drops_{0};
    std::shared_ptr<Counter> fallback_counter_ = std::make_shared<Counter>();
    std::shared_ptr<Gauge> fallback_gauge_ = std::make_shared<Gauge>();
    std::shared_ptr<Histogram> fallback_histogram_ = std::make_shared<Histogram>();
};

} // namespace lamp::telemetry
