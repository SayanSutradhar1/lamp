#pragma once

// ============================================================
// Lamp Framework
// Phase 10 - Security and Reliability
//
// File: runtime/security/auth_policy.hpp
//
// Purpose:
//   Authentication providers (HMAC-SHA256 compact signed
//   tokens, SHA-256 hashed API keys, composite chains) and
//   RBAC / scope / tenant operation authorization engine.
// ============================================================

#include "bridge/operation_descriptor.hpp"
#include "bridge/request_envelope.hpp"
#include "bridge/result_envelope.hpp"
#include "http/http_request.hpp"
#include "security/crypto_utils.hpp"
#include "security/security_context.hpp"

#include <chrono>
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

namespace lamp::security {

// ============================================================
// Authentication Result & Interface
// ============================================================

enum class AuthOutcome : std::uint8_t {
    NoCredentials = 0,
    Authenticated = 1,
    InvalidCredentials = 2,
    ExpiredCredentials = 3
};

struct AuthResult final {
    AuthOutcome outcome = AuthOutcome::NoCredentials;
    Principal principal;
    std::string identifier;
    std::string message;

    [[nodiscard]]
    bool Ok() const noexcept {
        return outcome == AuthOutcome::Authenticated && principal.authenticated;
    }

    [[nodiscard]]
    static AuthResult Missing() {
        AuthResult r;
        r.outcome = AuthOutcome::NoCredentials;
        r.identifier = "MISSING_CREDENTIALS";
        r.message = "Authentication credentials were not provided";
        return r;
    }

    [[nodiscard]]
    static AuthResult Success(Principal p) {
        p.authenticated = true;
        AuthResult r;
        r.outcome = AuthOutcome::Authenticated;
        r.principal = std::move(p);
        return r;
    }

    [[nodiscard]]
    static AuthResult Invalid(std::string msg = "Invalid authentication credentials") {
        AuthResult r;
        r.outcome = AuthOutcome::InvalidCredentials;
        r.identifier = "INVALID_CREDENTIALS";
        r.message = std::move(msg);
        return r;
    }

    [[nodiscard]]
    static AuthResult Expired() {
        AuthResult r;
        r.outcome = AuthOutcome::ExpiredCredentials;
        r.identifier = "CREDENTIALS_EXPIRED";
        r.message = "Authentication credentials have expired";
        return r;
    }
};

struct RawCredentials final {
    std::string scheme; // e.g. "bearer", "apikey"
    std::string secret;
};

class IAuthenticator {
public:
    virtual ~IAuthenticator() = default;

    [[nodiscard]]
    virtual AuthResult Authenticate(const RawCredentials& credentials) const = 0;
};

[[nodiscard]]
inline RawCredentials ExtractHttpCredentials(const http::HttpRequest& request) {
    if (const auto auth = request.header("Authorization")) {
        constexpr std::string_view kBearer = "Bearer ";
        if (detail::StartsWithIgnoreCase(*auth, kBearer)) {
            auto token = auth->substr(kBearer.size());
            while (!token.empty() && token.front() == ' ') {
                token.remove_prefix(1);
            }
            if (!token.empty()) {
                return RawCredentials{"bearer", std::string(token)};
            }
        }
        return RawCredentials{"unknown", std::string(*auth)};
    }

    if (const auto api_key = request.header("X-API-Key")) {
        if (!api_key->empty()) {
            return RawCredentials{"apikey", std::string(*api_key)};
        }
    }

    return RawCredentials{};
}

// ============================================================
// Signed Compact Token Authenticator (v1.<payload_b64u>.<sig_b64u>)
// ============================================================

struct SignedTokenClaims final {
    std::string subject_id;
    std::string tenant_id;
    std::vector<std::string> roles;
    std::vector<std::string> scopes;
    std::int64_t expires_at_epoch_seconds = 0; // 0 = non-expiring
};

class SignedTokenAuthenticator final : public IAuthenticator {
public:
    using ClockFn = std::function<std::chrono::system_clock::time_point()>;

    explicit SignedTokenAuthenticator(
        std::string hmac_secret,
        ClockFn clock = [] { return std::chrono::system_clock::now(); },
        std::chrono::seconds clock_skew = std::chrono::seconds(0)
    )
        : hmac_secret_(std::move(hmac_secret)),
          clock_(std::move(clock)),
          clock_skew_(clock_skew) {}

    [[nodiscard]]
    std::string IssueToken(const SignedTokenClaims& claims) const {
        // Line-delimited key=value payload encoded in Base64URL.
        // Control characters in claims are stripped to prevent injection.
        std::string payload;
        payload.append("sub=").append(SanitizeClaimValue(claims.subject_id)).push_back('\n');
        payload.append("ten=").append(SanitizeClaimValue(claims.tenant_id)).push_back('\n');
        payload.append("exp=").append(std::to_string(claims.expires_at_epoch_seconds)).push_back('\n');
        payload.append("rol=").append(SanitizeListClaim(claims.roles)).push_back('\n');
        payload.append("scp=").append(SanitizeListClaim(claims.scopes));

        const std::string header_plus_payload = "v1." + Base64UrlEncode(payload);
        const auto sig = HmacSha256(hmac_secret_, header_plus_payload);
        return header_plus_payload + "." + Base64UrlEncode(sig);
    }

    [[nodiscard]]
    AuthResult Authenticate(const RawCredentials& credentials) const override {
        if (credentials.secret.empty()) {
            return AuthResult::Missing();
        }
        if (!detail::StartsWithIgnoreCase(credentials.scheme, "bearer")) {
            return AuthResult::Missing();
        }
        return VerifyToken(credentials.secret);
    }

    [[nodiscard]]
    AuthResult VerifyToken(std::string_view token) const {
        if (hmac_secret_.empty() || token.empty() || token.size() > 8192) {
            return AuthResult::Invalid("Malformed or oversized bearer token");
        }

        const auto dot1 = token.find('.');
        if (dot1 == std::string_view::npos) {
            return AuthResult::Invalid("Malformed bearer token");
        }
        const auto dot2 = token.find('.', dot1 + 1);
        if (dot2 == std::string_view::npos || token.find('.', dot2 + 1) != std::string_view::npos) {
            return AuthResult::Invalid("Malformed bearer token");
        }

        const std::string_view version = token.substr(0, dot1);
        const std::string_view payload_b64 = token.substr(dot1 + 1, dot2 - dot1 - 1);
        const std::string_view sig_b64 = token.substr(dot2 + 1);

        if (version != "v1" || payload_b64.empty() || sig_b64.empty()) {
            return AuthResult::Invalid("Unsupported or empty bearer token");
        }

        const auto sig_bytes = Base64UrlDecode(sig_b64);
        if (!sig_bytes || sig_bytes->size() != 32) {
            return AuthResult::Invalid("Invalid token signature encoding");
        }

        const std::string_view signing_input = token.substr(0, dot2);
        if (!HmacSha256Verify(hmac_secret_, signing_input, *sig_bytes)) {
            return AuthResult::Invalid("Token signature verification failed");
        }

        const auto payload_bytes = Base64UrlDecode(payload_b64);
        if (!payload_bytes) {
            return AuthResult::Invalid("Invalid token payload encoding");
        }

        const std::string payload_str(
            reinterpret_cast<const char*>(payload_bytes->data()),
            payload_bytes->size()
        );

        Principal p;
        p.auth_scheme = "bearer";
        std::int64_t exp = 0;

        std::size_t pos = 0;
        while (pos < payload_str.size()) {
            const auto nl = payload_str.find('\n', pos);
            const std::string_view line = nl == std::string_view::npos
                ? std::string_view(payload_str).substr(pos)
                : std::string_view(payload_str).substr(pos, nl - pos);

            const auto eq = line.find('=');
            if (eq != std::string_view::npos) {
                const auto k = line.substr(0, eq);
                const auto v = line.substr(eq + 1);
                if (k == "sub") {
                    p.subject_id = std::string(v);
                } else if (k == "ten") {
                    p.tenant_id = std::string(v);
                } else if (k == "rol") {
                    p.roles = detail::SplitCsv(v);
                } else if (k == "scp") {
                    p.scopes = detail::SplitCsv(v);
                } else if (k == "exp") {
                    try {
                        exp = std::stoll(std::string(v));
                    } catch (...) {
                        return AuthResult::Invalid("Invalid token expiration claim");
                    }
                }
            }

            if (nl == std::string_view::npos) {
                break;
            }
            pos = nl + 1;
        }

        if (p.subject_id.empty()) {
            return AuthResult::Invalid("Token missing subject claim");
        }

        if (exp > 0) {
            const auto now_sec = std::chrono::duration_cast<std::chrono::seconds>(
                clock_().time_since_epoch()
            ).count();
            if (now_sec - clock_skew_.count() >= exp) {
                return AuthResult::Expired();
            }
        }

        return AuthResult::Success(std::move(p));
    }

private:
    static std::string SanitizeClaimValue(std::string_view v) {
        std::string out;
        out.reserve(v.size());
        for (unsigned char c : v) {
            if (c >= 0x20 && c != 0x7F && c != '\n' && c != '\r') {
                out.push_back(static_cast<char>(c));
            }
        }
        return out;
    }

    static std::string SanitizeListClaim(const std::vector<std::string>& items) {
        std::string out;
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (i > 0) {
                out.push_back(',');
            }
            for (unsigned char c : items[i]) {
                if (c >= 0x20 && c != 0x7F && c != ',') {
                    out.push_back(static_cast<char>(c));
                }
            }
        }
        return out;
    }

    std::string hmac_secret_;
    ClockFn clock_;
    std::chrono::seconds clock_skew_{0};
};

// ============================================================
// Hashed API Key Authenticator
// ============================================================

class ApiKeyAuthenticator final : public IAuthenticator {
public:
    void RegisterKey(std::string_view raw_key, Principal principal) {
        if (raw_key.empty()) {
            return;
        }
        principal.authenticated = true;
        if (principal.auth_scheme.empty()) {
            principal.auth_scheme = "apikey";
        }
        const std::string digest_hex = Sha256Hex(raw_key);
        std::unique_lock lock(mutex_);
        keys_[digest_hex] = std::move(principal);
    }

    bool RevokeKey(std::string_view raw_key) {
        const std::string digest_hex = Sha256Hex(raw_key);
        std::unique_lock lock(mutex_);
        return keys_.erase(digest_hex) > 0;
    }

    [[nodiscard]]
    AuthResult Authenticate(const RawCredentials& credentials) const override {
        if (credentials.secret.empty()) {
            return AuthResult::Missing();
        }
        if (!detail::StartsWithIgnoreCase(credentials.scheme, "apikey")) {
            return AuthResult::Missing();
        }

        const auto presented_digest = Sha256(credentials.secret);
        std::shared_lock lock(mutex_);

        // Iterate keys with constant-time digest comparison to avoid
        // leaking key digest timing via early exit or map bucket timing.
        const Principal* matched = nullptr;
        for (const auto& [hex_key, principal] : keys_) {
            const auto stored_bytes = HexToBytes(hex_key);
            if (stored_bytes &&
                ConstantTimeEquals(presented_digest, *stored_bytes)) {
                matched = &principal;
            }
        }

        if (matched != nullptr) {
            return AuthResult::Success(*matched);
        }
        return AuthResult::Invalid("API key is invalid or revoked");
    }

private:
    static std::optional<Sha256Digest> HexToBytes(std::string_view hex) noexcept {
        if (hex.size() != 64) {
            return std::nullopt;
        }
        auto nibble = [](char c) noexcept -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        Sha256Digest out{};
        for (std::size_t i = 0; i < 32; ++i) {
            const int hi = nibble(hex[i * 2]);
            const int lo = nibble(hex[i * 2 + 1]);
            if (hi < 0 || lo < 0) {
                return std::nullopt;
            }
            out[i] = static_cast<std::uint8_t>((hi << 4) | lo);
        }
        return out;
    }

    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, Principal> keys_;
};

// ============================================================
// Composite Authenticator
// ============================================================

class CompositeAuthenticator final : public IAuthenticator {
public:
    void AddProvider(std::shared_ptr<IAuthenticator> provider) {
        if (provider) {
            providers_.push_back(std::move(provider));
        }
    }

    [[nodiscard]]
    AuthResult Authenticate(const RawCredentials& credentials) const override {
        if (credentials.secret.empty()) {
            return AuthResult::Missing();
        }
        AuthResult last_failure = AuthResult::Missing();
        for (const auto& provider : providers_) {
            auto res = provider->Authenticate(credentials);
            if (res.Ok()) {
                return res;
            }
            if (res.outcome != AuthOutcome::NoCredentials) {
                last_failure = std::move(res);
            }
        }
        return last_failure.outcome == AuthOutcome::NoCredentials
            ? AuthResult::Invalid("No authenticator accepted the supplied credentials")
            : last_failure;
    }

private:
    std::vector<std::shared_ptr<IAuthenticator>> providers_;
};

// ============================================================
// Authorization Policy & RBAC/Scope Engine
// ============================================================

struct OperationAuthPolicy final {
    bool allow_anonymous = false;
    bool require_all_roles = false;
    std::vector<std::string> required_roles;
    std::vector<std::string> required_scopes;
    std::vector<std::string> allowed_tenants;

    std::function<bool(const SecurityContext&, const bridge::RequestEnvelope&)> custom_authorizer;
};

struct AuthzDecision final {
    bool allowed = false;
    bridge::OperationErrorCode error_code = bridge::OperationErrorCode::PermissionDenied;
    std::string identifier;
    std::string message;

    [[nodiscard]]
    static AuthzDecision Allow() {
        AuthzDecision d;
        d.allowed = true;
        d.error_code = bridge::OperationErrorCode::None;
        return d;
    }

    [[nodiscard]]
    static AuthzDecision Deny(
        bridge::OperationErrorCode code,
        std::string id,
        std::string msg
    ) {
        AuthzDecision d;
        d.allowed = false;
        d.error_code = code;
        d.identifier = std::move(id);
        d.message = std::move(msg);
        return d;
    }
};

class AuthorizationPolicyEngine final {
public:
    explicit AuthorizationPolicyEngine(bool default_deny = true) noexcept
        : default_deny_(default_deny) {}

    void SetDefaultDeny(bool default_deny) noexcept {
        std::unique_lock lock(mutex_);
        default_deny_ = default_deny;
    }

    void SetPolicy(
        std::string operation_pattern,
        OperationAuthPolicy policy
    ) {
        std::unique_lock lock(mutex_);
        if (operation_pattern.size() >= 2 &&
            operation_pattern.ends_with(".*")) {
            const auto prefix = operation_pattern.substr(0, operation_pattern.size() - 1);
            for (auto& rule : prefix_policies_) {
                if (rule.first == prefix) {
                    rule.second = std::move(policy);
                    return;
                }
            }
            prefix_policies_.emplace_back(prefix, std::move(policy));
        } else {
            exact_policies_[std::move(operation_pattern)] = std::move(policy);
        }
    }

    bool RemovePolicy(const std::string& operation_pattern) {
        std::unique_lock lock(mutex_);
        if (operation_pattern.size() >= 2 &&
            operation_pattern.ends_with(".*")) {
            const auto prefix = operation_pattern.substr(0, operation_pattern.size() - 1);
            const auto removed = std::erase_if(
                prefix_policies_,
                [&](const auto& item) { return item.first == prefix; }
            );
            return removed > 0;
        }
        return exact_policies_.erase(operation_pattern) > 0;
    }

    [[nodiscard]]
    AuthzDecision Evaluate(
        const SecurityContext& ctx,
        const bridge::RequestEnvelope& request
    ) const {
        std::optional<OperationAuthPolicy> matched_policy;
        bool default_deny_snapshot = true;

        {
            std::shared_lock lock(mutex_);
            default_deny_snapshot = default_deny_;

            if (const auto it = exact_policies_.find(request.operation_id);
                it != exact_policies_.end()) {
                matched_policy = it->second;
            } else {
                // Longest prefix wins among wildcard rules (e.g. "admin.users.*").
                std::size_t best_len = 0;
                for (const auto& [prefix, policy] : prefix_policies_) {
                    if (request.operation_id.starts_with(prefix) &&
                        prefix.size() > best_len) {
                        best_len = prefix.size();
                        matched_policy = policy;
                    }
                }
            }
        }

        if (!matched_policy) {
            if (default_deny_snapshot) {
                if (!ctx.principal.authenticated) {
                    return AuthzDecision::Deny(
                        bridge::OperationErrorCode::Unauthenticated,
                        "UNAUTHENTICATED",
                        "Authentication required for operation"
                    );
                }
                return AuthzDecision::Deny(
                    bridge::OperationErrorCode::PermissionDenied,
                    "NO_MATCHING_POLICY",
                    "Operation access denied by default policy"
                );
            }
            return AuthzDecision::Allow();
        }

        const auto& policy = *matched_policy;
        if (policy.allow_anonymous) {
            return AuthzDecision::Allow();
        }

        if (!ctx.principal.authenticated) {
            return AuthzDecision::Deny(
                bridge::OperationErrorCode::Unauthenticated,
                "UNAUTHENTICATED",
                "Authentication required"
            );
        }

        if (!policy.allowed_tenants.empty()) {
            bool tenant_ok = false;
            for (const auto& t : policy.allowed_tenants) {
                if (ctx.principal.tenant_id == t) {
                    tenant_ok = true;
                    break;
                }
            }
            if (!tenant_ok) {
                return AuthzDecision::Deny(
                    bridge::OperationErrorCode::PermissionDenied,
                    "TENANT_FORBIDDEN",
                    "Principal tenant is not authorized for operation"
                );
            }
        }

        const bool roles_ok = policy.require_all_roles
            ? ctx.principal.HasAllRoles(policy.required_roles)
            : ctx.principal.HasAnyRole(policy.required_roles);
        if (!roles_ok) {
            return AuthzDecision::Deny(
                bridge::OperationErrorCode::PermissionDenied,
                "ROLE_FORBIDDEN",
                "Principal lacks required role"
            );
        }

        if (!ctx.principal.HasAllScopes(policy.required_scopes)) {
            return AuthzDecision::Deny(
                bridge::OperationErrorCode::PermissionDenied,
                "SCOPE_FORBIDDEN",
                "Principal lacks required scope"
            );
        }

        if (policy.custom_authorizer) {
            bool custom_ok = false;
            try {
                custom_ok = policy.custom_authorizer(ctx, request);
            } catch (...) {
                custom_ok = false;
            }
            if (!custom_ok) {
                return AuthzDecision::Deny(
                    bridge::OperationErrorCode::PermissionDenied,
                    "POLICY_DENIED",
                    "Authorization policy denied request"
                );
            }
        }

        return AuthzDecision::Allow();
    }

private:
    mutable std::shared_mutex mutex_;
    bool default_deny_ = true;
    std::unordered_map<std::string, OperationAuthPolicy> exact_policies_;
    std::vector<std::pair<std::string, OperationAuthPolicy>> prefix_policies_;
};

} // namespace lamp::security
