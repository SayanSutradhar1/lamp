#pragma once

// ============================================================
// Lamp Framework
// Phase 10 - Security and Reliability
//
// File: runtime/security/input_sanitizer.hpp
//
// Purpose:
//   Request validation, safe defaults, limits, path/header/
//   metadata injection prevention, and untrusted header
//   stripping for HTTP requests and bridge envelopes.
// ============================================================

#include "bridge/operation_descriptor.hpp"
#include "bridge/request_envelope.hpp"
#include "http/http_request.hpp"
#include "http/http_response.hpp"
#include "security/security_context.hpp"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lamp::security {

// ============================================================
// Safe-Default Security Limits
// ============================================================

struct SecurityLimits final {
    std::size_t max_path_bytes = 2048;
    std::size_t max_query_bytes = 4096;
    std::size_t max_header_count = 64;
    std::size_t max_header_name_bytes = 128;
    std::size_t max_header_value_bytes = 8192;
    std::size_t max_total_header_bytes = 32U * 1024U;
    std::size_t max_body_bytes = 8U * 1024U * 1024U;

    std::size_t max_metadata_entries = 64;
    std::size_t max_metadata_key_bytes = 128;
    std::size_t max_metadata_value_bytes = 4096;

    bool require_host_for_http11 = true;
    bool reject_path_traversal = true;
    bool reject_control_chars = true;
    bool reject_smuggling_headers = true;
    bool strip_untrusted_headers = true;
    bool strip_credential_headers_after_auth = true;
    bool trust_forwarded_headers = false;

    // Empty means any syntactically valid Content-Type is accepted.
    std::vector<std::string> allowed_content_types;
};

struct SecurityValidationResult final {
    bool valid = true;
    bridge::OperationErrorCode error_code = bridge::OperationErrorCode::None;
    http::HttpStatus http_status = http::HttpStatus::Ok;
    std::string identifier;
    std::string message;

    [[nodiscard]]
    static SecurityValidationResult Allow() {
        return SecurityValidationResult{};
    }

    [[nodiscard]]
    static SecurityValidationResult Reject(
        bridge::OperationErrorCode code,
        http::HttpStatus status,
        std::string id,
        std::string msg
    ) {
        SecurityValidationResult r;
        r.valid = false;
        r.error_code = code;
        r.http_status = status;
        r.identifier = std::move(id);
        r.message = std::move(msg);
        return r;
    }
};

namespace detail {

[[nodiscard]]
inline bool ContainsControlOrNull(std::string_view text, bool allow_tab = false) noexcept {
    for (unsigned char c : text) {
        if (allow_tab && c == '\t') {
            continue;
        }
        if (c < 0x20 || c == 0x7F) {
            return true;
        }
    }
    return false;
}

[[nodiscard]]
inline bool ContainsPathTraversal(std::string_view path) noexcept {
    // Detect raw "..", backslash, double-encoded "%252e" / "%25",
    // percent-encoded dots "%2e", or percent-encoded slashes/backslashes.
    if (path.find('\\') != std::string_view::npos) {
        return true;
    }

    if (ContainsIgnoreCase(path, "%2e") ||
        ContainsIgnoreCase(path, "%2f") ||
        ContainsIgnoreCase(path, "%5c") ||
        ContainsIgnoreCase(path, "%00") ||
        ContainsIgnoreCase(path, "%25")) {
        return true;
    }

    // Check segment-by-segment for "." / ".." traversal components.
    std::size_t pos = 0;
    while (pos < path.size()) {
        const auto slash = path.find('/', pos);
        const auto seg = slash == std::string_view::npos
            ? path.substr(pos)
            : path.substr(pos, slash - pos);
        if (seg == ".." || seg == ".") {
            return true;
        }
        if (slash == std::string_view::npos) {
            break;
        }
        pos = slash + 1;
    }

    return false;
}

[[nodiscard]]
inline bool IsValidHostHeaderValue(std::string_view host) noexcept {
    if (host.empty() || host.size() > 255) {
        return false;
    }
    for (unsigned char c : host) {
        // Reject userinfo '@' injection, spaces, slashes, quotes, control chars.
        if (c <= 0x20 || c >= 0x7F || c == '@' || c == '/' || c == '\\' ||
            c == '?' || c == '#' || c == '"') {
            return false;
        }
    }
    return true;
}

[[nodiscard]]
inline bool IsUntrustedForwardedOrSpoofingHeader(
    std::string_view name,
    bool trust_forwarded_headers
) noexcept {
    if (StartsWithIgnoreCase(name, "x-lamp-") ||
        http::httpHeaderNameEquals(name, "proxy") ||
        http::httpHeaderNameEquals(name, "proxy-authorization") ||
        http::httpHeaderNameEquals(name, "cookie") ||
        http::httpHeaderNameEquals(name, "set-cookie") ||
        http::httpHeaderNameEquals(name, "x-authenticated-user") ||
        http::httpHeaderNameEquals(name, "x-auth-roles") ||
        http::httpHeaderNameEquals(name, "x-user-id")) {
        return true;
    }

    if (!trust_forwarded_headers) {
        if (http::httpHeaderNameEquals(name, "forwarded") ||
            http::httpHeaderNameEquals(name, "x-forwarded-for") ||
            http::httpHeaderNameEquals(name, "x-forwarded-host") ||
            http::httpHeaderNameEquals(name, "x-forwarded-proto") ||
            http::httpHeaderNameEquals(name, "x-real-ip") ||
            http::httpHeaderNameEquals(name, "true-client-ip")) {
            return true;
        }
    }

    return false;
}

[[nodiscard]]
inline bool IsMetadataKeyAllowed(std::string_view key) noexcept {
    if (key.empty()) {
        return false;
    }
    for (unsigned char c : key) {
        const bool ok =
            (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '.' || c == '-' || c == '_';
        if (!ok) {
            return false;
        }
    }
    return true;
}

[[nodiscard]]
inline bool MatchesContentTypePrefix(
    std::string_view header_value,
    const std::vector<std::string>& allowlist
) noexcept {
    if (allowlist.empty()) {
        return true;
    }

    // Strip optional "; charset=..." parameters before comparison.
    const auto semi = header_value.find(';');
    std::string_view media = semi == std::string_view::npos
        ? header_value
        : header_value.substr(0, semi);

    while (!media.empty() && (media.front() == ' ' || media.front() == '\t')) {
        media.remove_prefix(1);
    }
    while (!media.empty() && (media.back() == ' ' || media.back() == '\t')) {
        media.remove_suffix(1);
    }

    for (const auto& allowed : allowlist) {
        if (media.size() == allowed.size() &&
            StartsWithIgnoreCase(media, allowed)) {
            return true;
        }
    }
    return false;
}

} // namespace detail

// ============================================================
// HTTP Request Security Validator
// ============================================================

class HttpRequestSecurityValidator final {
public:
    [[nodiscard]]
    static SecurityValidationResult Validate(
        const http::HttpRequest& request,
        const SecurityLimits& limits = {}
    ) {
        const std::string_view path = request.path();
        const std::string_view query = request.query();

        if (path.size() > limits.max_path_bytes) {
            return SecurityValidationResult::Reject(
                bridge::OperationErrorCode::InvalidArgument,
                http::HttpStatus::UriTooLong,
                "URI_TOO_LONG",
                "Request path exceeds configured limit"
            );
        }

        if (query.size() > limits.max_query_bytes) {
            return SecurityValidationResult::Reject(
                bridge::OperationErrorCode::InvalidArgument,
                http::HttpStatus::UriTooLong,
                "QUERY_TOO_LONG",
                "Request query exceeds configured limit"
            );
        }

        if (limits.reject_control_chars &&
            (detail::ContainsControlOrNull(path) ||
             detail::ContainsControlOrNull(query) ||
             detail::ContainsControlOrNull(request.methodName()))) {
            return SecurityValidationResult::Reject(
                bridge::OperationErrorCode::InvalidArgument,
                http::HttpStatus::BadRequest,
                "INVALID_REQUEST_TARGET",
                "Request target contains disallowed control characters"
            );
        }

        if (limits.reject_path_traversal &&
            (detail::ContainsPathTraversal(path) ||
             detail::ContainsIgnoreCase(query, "%00"))) {
            return SecurityValidationResult::Reject(
                bridge::OperationErrorCode::InvalidArgument,
                http::HttpStatus::BadRequest,
                "PATH_TRAVERSAL_REJECTED",
                "Request target contains disallowed traversal sequence"
            );
        }

        if (request.body().size() > limits.max_body_bytes) {
            return SecurityValidationResult::Reject(
                bridge::OperationErrorCode::InvalidArgument,
                http::HttpStatus::PayloadTooLarge,
                "PAYLOAD_TOO_LARGE",
                "Request body exceeds configured limit"
            );
        }

        const auto& headers = request.headers();
        if (headers.size() > limits.max_header_count) {
            return SecurityValidationResult::Reject(
                bridge::OperationErrorCode::InvalidArgument,
                http::HttpStatus::RequestHeaderFieldsTooLarge,
                "TOO_MANY_HEADERS",
                "Request header count exceeds configured limit"
            );
        }

        std::size_t total_header_bytes = 0;
        for (const auto& header : headers.entries()) {
            if (header.name.size() > limits.max_header_name_bytes ||
                header.value.size() > limits.max_header_value_bytes) {
                return SecurityValidationResult::Reject(
                    bridge::OperationErrorCode::InvalidArgument,
                    http::HttpStatus::RequestHeaderFieldsTooLarge,
                    "HEADER_TOO_LARGE",
                    "Request header exceeds configured size limit"
                );
            }

            if (limits.reject_control_chars &&
                (detail::ContainsControlOrNull(header.name) ||
                 detail::ContainsControlOrNull(header.value, true))) {
                return SecurityValidationResult::Reject(
                    bridge::OperationErrorCode::InvalidArgument,
                    http::HttpStatus::BadRequest,
                    "INVALID_HEADER",
                    "Request header contains invalid characters"
                );
            }

            total_header_bytes += header.name.size() + header.value.size();
            if (total_header_bytes > limits.max_total_header_bytes) {
                return SecurityValidationResult::Reject(
                    bridge::OperationErrorCode::InvalidArgument,
                    http::HttpStatus::RequestHeaderFieldsTooLarge,
                    "HEADERS_TOTAL_TOO_LARGE",
                    "Total request headers exceed configured limit"
                );
            }
        }

        if (limits.require_host_for_http11 && request.version().isHttp11()) {
            const auto host_count = headers.count("Host");
            const auto host_val = headers.get("Host");
            if (host_count != 1 || !host_val || !detail::IsValidHostHeaderValue(*host_val)) {
                return SecurityValidationResult::Reject(
                    bridge::OperationErrorCode::InvalidArgument,
                    http::HttpStatus::BadRequest,
                    "INVALID_HOST_HEADER",
                    "HTTP/1.1 request requires a single valid Host header"
                );
            }
        }

        if (limits.reject_smuggling_headers) {
            const bool has_cl = headers.contains("Content-Length");
            const bool has_te = headers.contains("Transfer-Encoding");
            if ((has_cl && has_te) || headers.count("Content-Length") > 1) {
                return SecurityValidationResult::Reject(
                    bridge::OperationErrorCode::InvalidArgument,
                    http::HttpStatus::BadRequest,
                    "AMBIGUOUS_FRAMING_HEADERS",
                    "Ambiguous Content-Length / Transfer-Encoding headers rejected"
                );
            }
        }

        if (!request.body().empty() && !limits.allowed_content_types.empty()) {
            const auto ct = headers.get("Content-Type");
            if (!ct || !detail::MatchesContentTypePrefix(*ct, limits.allowed_content_types)) {
                return SecurityValidationResult::Reject(
                    bridge::OperationErrorCode::InvalidArgument,
                    http::HttpStatus::UnsupportedMediaType,
                    "UNSUPPORTED_MEDIA_TYPE",
                    "Content-Type is not permitted"
                );
            }
        }

        return SecurityValidationResult::Allow();
    }

    static std::size_t StripUntrustedHeaders(
        http::HttpRequest& request,
        const SecurityLimits& limits = {},
        bool strip_credentials = true
    ) {
        if (!limits.strip_untrusted_headers && !strip_credentials) {
            return 0;
        }

        std::vector<std::string> to_remove;
        for (const auto& h : request.headers().entries()) {
            const bool is_credential =
                http::httpHeaderNameEquals(h.name, "authorization") ||
                http::httpHeaderNameEquals(h.name, "x-api-key");

            const bool remove_untrusted =
                limits.strip_untrusted_headers &&
                detail::IsUntrustedForwardedOrSpoofingHeader(
                    h.name,
                    limits.trust_forwarded_headers
                );

            if (remove_untrusted || (strip_credentials && is_credential)) {
                to_remove.push_back(h.name);
            }
        }

        std::size_t removed = 0;
        for (const auto& name : to_remove) {
            removed += request.headers().erase(name);
        }
        return removed;
    }

    [[nodiscard]]
    static std::string ExtractClientIp(
        const http::HttpRequest& request,
        std::string_view peer_remote_ip,
        const SecurityLimits& limits = {}
    ) {
        if (limits.trust_forwarded_headers) {
            if (const auto xff = request.header("X-Forwarded-For")) {
                const auto comma = xff->find(',');
                auto first = comma == std::string_view::npos ? *xff : xff->substr(0, comma);
                while (!first.empty() && first.front() == ' ') {
                    first.remove_prefix(1);
                }
                while (!first.empty() && first.back() == ' ') {
                    first.remove_suffix(1);
                }
                if (!first.empty() &&
                    first.size() <= 64 &&
                    !detail::ContainsControlOrNull(first)) {
                    return std::string(first);
                }
            }
            if (const auto real_ip = request.header("X-Real-IP")) {
                if (!real_ip->empty() &&
                    real_ip->size() <= 64 &&
                    !detail::ContainsControlOrNull(*real_ip)) {
                    return std::string(*real_ip);
                }
            }
        }
        return std::string(peer_remote_ip);
    }
};

// ============================================================
// Bridge Envelope Security Sanitizer
// ============================================================

class EnvelopeSecuritySanitizer final {
public:
    [[nodiscard]]
    static SecurityValidationResult SanitizeAndValidate(
        bridge::RequestEnvelope& envelope,
        const SecurityLimits& limits = {},
        const bridge::OperationDescriptor* descriptor = nullptr
    ) {
        if (!bridge::OperationDescriptor::IsValidId(envelope.operation_id)) {
            return SecurityValidationResult::Reject(
                bridge::OperationErrorCode::InvalidArgument,
                http::HttpStatus::BadRequest,
                "INVALID_OPERATION_ID",
                "Operation identifier is invalid"
            );
        }

        const std::size_t effective_max_input =
            descriptor ? (std::min)(limits.max_body_bytes, descriptor->limits.max_input_bytes)
                       : limits.max_body_bytes;

        if (envelope.payload.size() > effective_max_input) {
            return SecurityValidationResult::Reject(
                bridge::OperationErrorCode::InvalidArgument,
                http::HttpStatus::PayloadTooLarge,
                "PAYLOAD_TOO_LARGE",
                "Request payload exceeds configured limit"
            );
        }

        // Always strip reserved internal/auth metadata before validation.
        StripUntrustedMetadata(envelope);

        if (envelope.metadata.size() > limits.max_metadata_entries) {
            return SecurityValidationResult::Reject(
                bridge::OperationErrorCode::InvalidArgument,
                http::HttpStatus::BadRequest,
                "TOO_MANY_METADATA_ENTRIES",
                "Request metadata count exceeds limit"
            );
        }

        for (const auto& entry : envelope.metadata) {
            if (entry.key.size() > limits.max_metadata_key_bytes ||
                !detail::IsMetadataKeyAllowed(entry.key)) {
                return SecurityValidationResult::Reject(
                    bridge::OperationErrorCode::InvalidArgument,
                    http::HttpStatus::BadRequest,
                    "INVALID_METADATA_KEY",
                    "Request metadata key contains invalid characters or exceeds length"
                );
            }
            if (entry.value.size() > limits.max_metadata_value_bytes ||
                detail::ContainsControlOrNull(entry.value, true)) {
                return SecurityValidationResult::Reject(
                    bridge::OperationErrorCode::InvalidArgument,
                    http::HttpStatus::BadRequest,
                    "INVALID_METADATA_VALUE",
                    "Request metadata value contains invalid characters or exceeds length"
                );
            }
        }

        if (detail::ContainsControlOrNull(envelope.trace_id) ||
            detail::ContainsControlOrNull(envelope.span_id) ||
            envelope.trace_id.size() > 128 ||
            envelope.span_id.size() > 128) {
            return SecurityValidationResult::Reject(
                bridge::OperationErrorCode::InvalidArgument,
                http::HttpStatus::BadRequest,
                "INVALID_TRACE_CONTEXT",
                "Trace context contains disallowed characters"
            );
        }

        return SecurityValidationResult::Allow();
    }
};

} // namespace lamp::security
