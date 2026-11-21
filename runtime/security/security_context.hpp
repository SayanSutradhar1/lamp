#pragma once

// ============================================================
// Lamp Framework
// Phase 10 - Security and Reliability
//
// File: runtime/security/security_context.hpp
//
// Purpose:
//   Authenticated principal representation, trusted metadata
//   binding, secret-free error sanitization, and safe default
//   HTTP security headers.
// ============================================================

#include "bridge/request_envelope.hpp"
#include "bridge/result_envelope.hpp"
#include "http/http_response.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lamp::security {

// ============================================================
// Trusted Principal & Security Context
// ============================================================

struct Principal final {
    bool authenticated = false;
    std::string subject_id;
    std::string tenant_id;
    std::string auth_scheme;
    std::vector<std::string> roles;
    std::vector<std::string> scopes;

    [[nodiscard]]
    bool HasRole(std::string_view role) const noexcept {
        for (const auto& entry : roles) {
            if (entry == role) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]]
    bool HasScope(std::string_view scope) const noexcept {
        for (const auto& entry : scopes) {
            if (entry == scope) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]]
    bool HasAnyRole(const std::vector<std::string>& required) const noexcept {
        if (required.empty()) {
            return true;
        }
        for (const auto& role : required) {
            if (HasRole(role)) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]]
    bool HasAllRoles(const std::vector<std::string>& required) const noexcept {
        for (const auto& role : required) {
            if (!HasRole(role)) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]]
    bool HasAllScopes(const std::vector<std::string>& required) const noexcept {
        for (const auto& scope : required) {
            if (!HasScope(scope)) {
                return false;
            }
        }
        return true;
    }
};

struct SecurityContext final {
    Principal principal;
    std::string client_ip;
    bridge::TransportKind source_transport = bridge::TransportKind::Unknown;
};

// ============================================================
// Reserved Metadata Keys
// ============================================================

inline constexpr std::string_view kReservedMetadataPrefix = "lamp.";
inline constexpr std::string_view kAuthMetadataPrefix = "auth.";
inline constexpr std::string_view kSecurityMetadataPrefix = "security.";

inline constexpr std::string_view kMetaSubject = "lamp.auth.subject";
inline constexpr std::string_view kMetaTenant = "lamp.auth.tenant";
inline constexpr std::string_view kMetaScheme = "lamp.auth.scheme";
inline constexpr std::string_view kMetaRoles = "lamp.auth.roles";
inline constexpr std::string_view kMetaScopes = "lamp.auth.scopes";
inline constexpr std::string_view kMetaAuthenticated = "lamp.auth.authenticated";
inline constexpr std::string_view kMetaClientIp = "lamp.client.ip";

namespace detail {

[[nodiscard]]
inline std::string AsciiToLower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

[[nodiscard]]
inline bool StartsWithIgnoreCase(std::string_view haystack, std::string_view prefix) noexcept {
    if (haystack.size() < prefix.size()) {
        return false;
    }
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        const auto a = std::tolower(static_cast<unsigned char>(haystack[i]));
        const auto b = std::tolower(static_cast<unsigned char>(prefix[i]));
        if (a != b) {
            return false;
        }
    }
    return true;
}

[[nodiscard]]
inline bool ContainsIgnoreCase(std::string_view haystack, std::string_view needle) noexcept {
    if (needle.empty() || haystack.size() < needle.size()) {
        return false;
    }
    for (std::size_t i = 0; i <= haystack.size() - needle.size(); ++i) {
        if (StartsWithIgnoreCase(haystack.substr(i), needle)) {
            return true;
        }
    }
    return false;
}

[[nodiscard]]
inline std::string JoinCsv(const std::vector<std::string>& items) {
    std::string out;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i > 0) {
            out.push_back(',');
        }
        out.append(items[i]);
    }
    return out;
}

[[nodiscard]]
inline std::vector<std::string> SplitCsv(std::string_view text) {
    std::vector<std::string> out;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const auto comma = text.find(',', pos);
        const auto part = comma == std::string_view::npos
            ? text.substr(pos)
            : text.substr(pos, comma - pos);
        if (!part.empty()) {
            out.emplace_back(part);
        }
        if (comma == std::string_view::npos) {
            break;
        }
        pos = comma + 1;
    }
    return out;
}

} // namespace detail

// Removes any client-supplied reserved security/auth metadata keys so
// callers cannot spoof authenticated identities or roles via metadata.
inline std::size_t StripUntrustedMetadata(bridge::RequestEnvelope& envelope) {
    const std::size_t before = envelope.metadata.size();
    std::erase_if(envelope.metadata, [](const bridge::RequestMetadataEntry& entry) {
        return detail::StartsWithIgnoreCase(entry.key, kReservedMetadataPrefix) ||
               detail::StartsWithIgnoreCase(entry.key, kAuthMetadataPrefix) ||
               detail::StartsWithIgnoreCase(entry.key, kSecurityMetadataPrefix) ||
               detail::StartsWithIgnoreCase(entry.key, "x-lamp-") ||
               detail::StartsWithIgnoreCase(entry.key, "authorization") ||
               detail::StartsWithIgnoreCase(entry.key, "cookie") ||
               detail::StartsWithIgnoreCase(entry.key, "x-api-key");
    });
    return before - envelope.metadata.size();
}

// Strips reserved metadata first, then writes verified security context fields.
inline void AttachSecurityContext(
    bridge::RequestEnvelope& envelope,
    const SecurityContext& ctx
) {
    StripUntrustedMetadata(envelope);

    if (!ctx.client_ip.empty()) {
        envelope.AddMetadata(std::string(kMetaClientIp), ctx.client_ip);
    }

    envelope.AddMetadata(
        std::string(kMetaAuthenticated),
        ctx.principal.authenticated ? "true" : "false"
    );

    if (!ctx.principal.authenticated) {
        return;
    }

    if (!ctx.principal.subject_id.empty()) {
        envelope.AddMetadata(std::string(kMetaSubject), ctx.principal.subject_id);
    }
    if (!ctx.principal.tenant_id.empty()) {
        envelope.AddMetadata(std::string(kMetaTenant), ctx.principal.tenant_id);
    }
    if (!ctx.principal.auth_scheme.empty()) {
        envelope.AddMetadata(std::string(kMetaScheme), ctx.principal.auth_scheme);
    }
    if (!ctx.principal.roles.empty()) {
        envelope.AddMetadata(std::string(kMetaRoles), detail::JoinCsv(ctx.principal.roles));
    }
    if (!ctx.principal.scopes.empty()) {
        envelope.AddMetadata(std::string(kMetaScopes), detail::JoinCsv(ctx.principal.scopes));
    }
}

[[nodiscard]]
inline SecurityContext ExtractSecurityContext(const bridge::RequestEnvelope& envelope) {
    SecurityContext ctx;
    ctx.source_transport = envelope.transport;

    for (const auto& entry : envelope.metadata) {
        if (entry.key == kMetaClientIp) {
            ctx.client_ip = entry.value;
        } else if (entry.key == kMetaAuthenticated) {
            ctx.principal.authenticated = (entry.value == "true");
        } else if (entry.key == kMetaSubject) {
            ctx.principal.subject_id = entry.value;
        } else if (entry.key == kMetaTenant) {
            ctx.principal.tenant_id = entry.value;
        } else if (entry.key == kMetaScheme) {
            ctx.principal.auth_scheme = entry.value;
        } else if (entry.key == kMetaRoles) {
            ctx.principal.roles = detail::SplitCsv(entry.value);
        } else if (entry.key == kMetaScopes) {
            ctx.principal.scopes = detail::SplitCsv(entry.value);
        }
    }

    if (!ctx.principal.authenticated) {
        ctx.principal.subject_id.clear();
        ctx.principal.tenant_id.clear();
        ctx.principal.auth_scheme.clear();
        ctx.principal.roles.clear();
        ctx.principal.scopes.clear();
    }

    return ctx;
}

// ============================================================
// Secret-Free Error Sanitization & Safe Response Defaults
// ============================================================

[[nodiscard]]
inline std::string_view DefaultSafeErrorMessage(
    bridge::OperationErrorCode code
) noexcept {
    using E = bridge::OperationErrorCode;
    switch (code) {
        case E::InvalidArgument:
            return "Request failed validation";
        case E::Unauthenticated:
            return "Authentication required";
        case E::PermissionDenied:
            return "Permission denied";
        case E::NotFound:
            return "Requested resource was not found";
        case E::AlreadyExists:
            return "Resource already exists";
        case E::Conflict:
            return "Request conflicts with current state";
        case E::ResourceExhausted:
            return "Rate limit or quota exceeded";
        case E::FailedPrecondition:
            return "Precondition not met";
        case E::Unavailable:
            return "Service temporarily unavailable";
        case E::Cancelled:
            return "Operation was cancelled";
        case E::DeadlineExceeded:
            return "Operation deadline exceeded";
        case E::NotImplemented:
            return "Operation is not implemented";
        case E::SerializationFailed:
        case E::ExecutionFailed:
        case E::Internal:
        case E::None:
        default:
            return "Internal processing error";
    }
}

[[nodiscard]]
inline bool ContainsSensitivePattern(std::string_view message) noexcept {
    // Reject stack traces, filesystem paths, credentials, SQL dumps, or control chars.
    static constexpr std::string_view kSensitiveMarkers[] = {
        "bearer ",
        "basic ",
        "password",
        "passwd",
        "secret",
        "api_key",
        "apikey",
        "token=",
        "authorization",
        "private_key",
        "-----begin",
        "c:\\",
        "d:\\",
        "/home/",
        "/users/",
        "/etc/",
        "/var/",
        "stack trace",
        "backtrace",
        "at 0x",
        "select ",
        "sqlstate"
    };

    for (unsigned char c : message) {
        if (c < 0x20 || c == 0x7F) {
            return true;
        }
    }

    for (const auto marker : kSensitiveMarkers) {
        if (detail::ContainsIgnoreCase(message, marker)) {
            return true;
        }
    }
    return false;
}

[[nodiscard]]
inline std::string SanitizeErrorMessage(
    std::string_view message,
    bridge::OperationErrorCode code
) {
    // Internal / execution / serialization failures never expose raw exception text.
    if (code == bridge::OperationErrorCode::Internal ||
        code == bridge::OperationErrorCode::ExecutionFailed ||
        code == bridge::OperationErrorCode::SerializationFailed ||
        message.empty() ||
        message.size() > 256 ||
        ContainsSensitivePattern(message)) {
        return std::string(DefaultSafeErrorMessage(code));
    }

    return std::string(message);
}

inline void SanitizeResultError(bridge::ResultEnvelope& result) {
    if (!result.error.HasError()) {
        return;
    }
    result.error.message = SanitizeErrorMessage(result.error.message, result.error.code);
}

struct SafeHeadersConfig final {
    bool set_nosniff = true;
    bool set_frame_options_deny = true;
    bool set_referrer_no_referrer = true;
    bool set_no_store_on_errors = true;
    bool enable_hsts = false;
    std::uint32_t hsts_max_age_seconds = 31536000;
};

inline void ApplySafeDefaultHeaders(
    http::HttpResponse& response,
    const SafeHeadersConfig& config = {}
) {
    if (config.set_nosniff && !response.header("X-Content-Type-Options")) {
        response.setHeader("X-Content-Type-Options", "nosniff");
    }
    if (config.set_frame_options_deny && !response.header("X-Frame-Options")) {
        response.setHeader("X-Frame-Options", "DENY");
    }
    if (config.set_referrer_no_referrer && !response.header("Referrer-Policy")) {
        response.setHeader("Referrer-Policy", "no-referrer");
    }
    if (config.set_no_store_on_errors &&
        (response.isClientError() || response.isServerError()) &&
        !response.header("Cache-Control")) {
        response.setHeader("Cache-Control", "no-store");
    }
    if (config.enable_hsts && !response.header("Strict-Transport-Security")) {
        response.setHeader(
            "Strict-Transport-Security",
            "max-age=" + std::to_string(config.hsts_max_age_seconds) + "; includeSubDomains"
        );
    }
}

[[nodiscard]]
inline http::HttpResponse BuildSafeErrorResponse(
    http::HttpStatus status,
    std::string_view error_identifier,
    http::HttpVersion version = {},
    std::optional<std::chrono::seconds> retry_after = std::nullopt,
    const SafeHeadersConfig& headers_config = {}
) {
    http::HttpResponse response(status, version);
    response.setHeader("Content-Type", "application/json");
    response.setHeader("Cache-Control", "no-store");

    if (status == http::HttpStatus::Unauthorized) {
        response.setHeader("WWW-Authenticate", "Bearer realm=\"lamp\"");
    }

    if (retry_after.has_value()) {
        const auto secs = retry_after->count() > 0 ? retry_after->count() : 1;
        response.setHeader("Retry-After", std::to_string(secs));
    }

    // Keep JSON identifier strictly safe alphanumeric/underscore/hyphen.
    std::string safe_id;
    safe_id.reserve(error_identifier.size());
    for (unsigned char c : error_identifier) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-') {
            safe_id.push_back(static_cast<char>(c));
        }
    }
    if (safe_id.empty()) {
        safe_id = "request_rejected";
    }

    response.setBody("{\"error\":\"" + safe_id + "\"}");
    ApplySafeDefaultHeaders(response, headers_config);
    return response;
}

} // namespace lamp::security
