#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lamp::core {

// ============================================================
// Origin Allowance Configuration & Policy
// ============================================================
//
// Controls which external origins (e.g. "https://app.example.com")
// are permitted to communicate with a Lamp application and
// exchange data across HTTP and WebSocket/LMP1 boundaries.
//
// Modes:
//   - DenyAll   (default, allowOrigin: false): rejects any
//               cross-origin request carrying an Origin header
//               with a CORS error.
//   - AllowAll  (allowOrigin: true): permits any origin ("*").
//   - AllowList (allowOrigin: (expose, callback) => true):
//               permits only origins registered via expose([...]).
// ============================================================

enum class OriginAllowanceMode : std::uint8_t {
    DenyAll = 0,
    AllowAll = 1,
    AllowList = 2
};

struct OriginAllowanceConfig final {
    OriginAllowanceMode mode = OriginAllowanceMode::DenyAll;
    std::vector<std::string> allowedOrigins{};
    bool allowWithoutOriginHeader = true;
};

struct OriginEvaluationResult final {
    bool allowed = false;
    std::string normalizedOrigin{};
    std::string accessControlAllowOrigin{};
    std::string errorCode{};
    std::string errorMessage{};
};

class OriginAllowancePolicy final {
public:
    OriginAllowancePolicy() = default;

    explicit OriginAllowancePolicy(OriginAllowanceConfig config) {
        configure(std::move(config));
    }

    [[nodiscard]]
    static std::string normalizeOrigin(std::string_view raw) {
        std::string_view trimmed = TrimAscii(raw);
        if (trimmed.empty()) {
            return {};
        }
        if (trimmed == "*") {
            return "*";
        }

        // Strip trailing slashes so "https://example.com/" == "https://example.com".
        while (trimmed.size() > 1 && trimmed.back() == '/') {
            trimmed.remove_suffix(1);
        }

        std::string out;
        out.reserve(trimmed.size());
        for (const char ch : trimmed) {
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        }
        return out;
    }

    [[nodiscard]]
    static bool isValidOriginEntry(std::string_view raw) noexcept {
        const std::string_view trimmed = TrimAscii(raw);
        if (trimmed.empty()) {
            return false;
        }
        for (const char ch : trimmed) {
            const unsigned char uch = static_cast<unsigned char>(ch);
            if (uch < 0x20 || uch == 0x7F) {
                return false;
            }
        }
        return true;
    }

    void configure(OriginAllowanceConfig config) {
        std::vector<std::string> normalized;
        normalized.reserve(config.allowedOrigins.size());
        for (const auto& entry : config.allowedOrigins) {
            auto norm = normalizeOrigin(entry);
            if (!norm.empty()) {
                normalized.push_back(std::move(norm));
            }
        }
        config.allowedOrigins = std::move(normalized);

        std::unique_lock lock(mutex_);
        config_ = std::move(config);
    }

    void setAllowAll(bool allowAll) {
        std::unique_lock lock(mutex_);
        config_.mode = allowAll
            ? OriginAllowanceMode::AllowAll
            : OriginAllowanceMode::DenyAll;
        if (!allowAll) {
            config_.allowedOrigins.clear();
        }
    }

    void setAllowedOrigins(std::vector<std::string> origins) {
        OriginAllowanceConfig next;
        {
            std::shared_lock lock(mutex_);
            next = config_;
        }
        next.mode = OriginAllowanceMode::AllowList;
        next.allowedOrigins = std::move(origins);
        configure(std::move(next));
    }

    [[nodiscard]]
    OriginAllowanceConfig config() const {
        std::shared_lock lock(mutex_);
        return config_;
    }

    [[nodiscard]]
    bool isOriginAllowed(std::string_view origin) const {
        return evaluate(origin).allowed;
    }

    [[nodiscard]]
    OriginEvaluationResult evaluate(std::optional<std::string_view> origin) const {
        std::shared_lock lock(mutex_);

        if (!origin.has_value() || TrimAscii(*origin).empty()) {
            if (config_.allowWithoutOriginHeader) {
                OriginEvaluationResult ok;
                ok.allowed = true;
                return ok;
            }
            OriginEvaluationResult denied;
            denied.allowed = false;
            denied.errorCode = "CORS_ORIGIN_REQUIRED";
            denied.errorMessage = "Origin header is required by Lamp CORS policy";
            return denied;
        }

        const std::string_view rawTrimmed = TrimAscii(*origin);
        const std::string normalized = normalizeOrigin(rawTrimmed);

        if (config_.mode == OriginAllowanceMode::DenyAll) {
            OriginEvaluationResult denied;
            denied.allowed = false;
            denied.normalizedOrigin = normalized;
            denied.errorCode = "CORS_ORIGIN_DENIED";
            denied.errorMessage =
                "CORS error: origin '" + std::string(rawTrimmed) + "' is not allowed";
            return denied;
        }

        if (config_.mode == OriginAllowanceMode::AllowAll) {
            OriginEvaluationResult ok;
            ok.allowed = true;
            ok.normalizedOrigin = normalized;
            ok.accessControlAllowOrigin = "*";
            return ok;
        }

        // OriginAllowanceMode::AllowList
        for (const auto& allowedEntry : config_.allowedOrigins) {
            if (allowedEntry == "*") {
                OriginEvaluationResult ok;
                ok.allowed = true;
                ok.normalizedOrigin = normalized;
                ok.accessControlAllowOrigin = std::string(rawTrimmed);
                return ok;
            }
            if (normalized != "null" && allowedEntry == normalized) {
                OriginEvaluationResult ok;
                ok.allowed = true;
                ok.normalizedOrigin = normalized;
                ok.accessControlAllowOrigin = std::string(rawTrimmed);
                return ok;
            }
        }

        OriginEvaluationResult denied;
        denied.allowed = false;
        denied.normalizedOrigin = normalized;
        denied.errorCode = "CORS_ORIGIN_DENIED";
        denied.errorMessage =
            "CORS error: origin '" + std::string(rawTrimmed) + "' is not in allowed origins";
        return denied;
    }

private:
    [[nodiscard]]
    static constexpr std::string_view TrimAscii(std::string_view value) noexcept {
        while (!value.empty() &&
               (value.front() == ' ' || value.front() == '\t' ||
                value.front() == '\r' || value.front() == '\n')) {
            value.remove_prefix(1);
        }
        while (!value.empty() &&
               (value.back() == ' ' || value.back() == '\t' ||
                value.back() == '\r' || value.back() == '\n')) {
            value.remove_suffix(1);
        }
        return value;
    }

    mutable std::shared_mutex mutex_;
    OriginAllowanceConfig config_{};
};

} // namespace lamp::core
