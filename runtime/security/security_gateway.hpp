#pragma once

// ============================================================
// Lamp Framework
// Phase 10 - Security and Reliability
//
// File: runtime/security/security_gateway.hpp
//
// Purpose:
//   Unified security & reliability enforcement layer for:
//     - Operation dispatch (SecuredOperationDispatcher)
//     - HTTP transport middleware (HttpSecurityGateway /
//       MakeHttpSecurityMiddleware)
// ============================================================

#include "bridge/execution_dispatcher.hpp"
#include "bridge/http_transport_adapter.hpp"
#include "bridge/request_envelope.hpp"
#include "bridge/result_envelope.hpp"
#include "http/http_middleware.hpp"
#include "http/http_request.hpp"
#include "http/http_response.hpp"
#include "security/auth_policy.hpp"
#include "security/input_sanitizer.hpp"
#include "security/rate_limiter.hpp"
#include "security/security_context.hpp"

#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace lamp::security {

// ============================================================
// Secured Operation Dispatcher
// ============================================================

struct SecurityGatewayConfig final {
    SecurityLimits limits;
    SafeHeadersConfig safe_headers;
    bool enable_rate_limiting = true;
    bool enable_circuit_breaker = true;
    bool sanitize_result_errors = true;
    bool enforce_authorization = true;
};

class SecuredOperationDispatcher final {
public:
    SecuredOperationDispatcher(
        std::shared_ptr<bridge::ExecutionDispatcher> dispatcher,
        std::shared_ptr<IAuthenticator> authenticator = {},
        std::shared_ptr<AuthorizationPolicyEngine> authz =
            std::make_shared<AuthorizationPolicyEngine>(true),
        std::shared_ptr<TokenBucketRateLimiter> rate_limiter = {},
        std::shared_ptr<CircuitBreakerRegistry> circuit_breakers = {},
        SecurityGatewayConfig config = {}
    )
        : dispatcher_(std::move(dispatcher)),
          authenticator_(std::move(authenticator)),
          authz_(std::move(authz)),
          rate_limiter_(std::move(rate_limiter)),
          circuit_breakers_(std::move(circuit_breakers)),
          config_(std::move(config)) {}

    [[nodiscard]]
    bridge::ResultEnvelope Dispatch(
        bridge::RequestEnvelope request,
        const RawCredentials& credentials = {},
        std::string_view client_ip = {},
        const std::optional<SecurityContext>& trusted_override = std::nullopt
    ) {
        if (!dispatcher_) {
            return MakeSecurityFailure(
                request,
                bridge::OperationErrorCode::Unavailable,
                "DISPATCHER_UNAVAILABLE",
                "Execution dispatcher is unavailable",
                true
            );
        }

        // 1. Validate request & always strip spoofed reserved metadata.
        const auto validation =
            EnvelopeSecuritySanitizer::SanitizeAndValidate(request, config_.limits);
        if (!validation.valid) {
            return MakeSecurityFailure(
                request,
                validation.error_code,
                validation.identifier,
                validation.message,
                false
            );
        }

        // 2. Establish verified SecurityContext (authentication).
        SecurityContext ctx;
        ctx.source_transport = request.transport;
        ctx.client_ip = std::string(client_ip);

        if (trusted_override.has_value()) {
            ctx = *trusted_override;
            if (ctx.client_ip.empty() && !client_ip.empty()) {
                ctx.client_ip = std::string(client_ip);
            }
        } else if (authenticator_ && !credentials.secret.empty()) {
            const auto auth_res = authenticator_->Authenticate(credentials);
            if (!auth_res.Ok()) {
                return MakeSecurityFailure(
                    request,
                    bridge::OperationErrorCode::Unauthenticated,
                    auth_res.identifier,
                    auth_res.message,
                    false
                );
            }
            ctx.principal = auth_res.principal;
        }

        // 3. Enforce authorization policy before injecting verified identity.
        if (config_.enforce_authorization && authz_) {
            const auto decision = authz_->Evaluate(ctx, request);
            if (!decision.allowed) {
                return MakeSecurityFailure(
                    request,
                    decision.error_code,
                    decision.identifier,
                    decision.message,
                    false
                );
            }
        }

        // Attach verified principal metadata cleanly after spoofing strip.
        AttachSecurityContext(request, ctx);

        // 4. Enforce token-bucket rate limit.
        if (config_.enable_rate_limiting && rate_limiter_) {
            const std::string_view rate_key =
                ctx.principal.authenticated && !ctx.principal.subject_id.empty()
                    ? std::string_view(ctx.principal.subject_id)
                    : (!ctx.client_ip.empty()
                           ? std::string_view(ctx.client_ip)
                           : std::string_view("anonymous"));

            const auto limit_decision =
                rate_limiter_->TryAcquire(rate_key, request.operation_id);
            if (!limit_decision.allowed) {
                return MakeSecurityFailure(
                    request,
                    bridge::OperationErrorCode::ResourceExhausted,
                    "RATE_LIMIT_EXCEEDED",
                    "Rate limit exceeded for operation",
                    true
                );
            }
        }

        // 5. Enforce circuit breaker if enabled.
        std::shared_ptr<CircuitBreaker> breaker;
        if (config_.enable_circuit_breaker && circuit_breakers_) {
            breaker = circuit_breakers_->GetOrCreate(request.operation_id);
            if (!breaker->TryAcquire()) {
                return MakeSecurityFailure(
                    request,
                    bridge::OperationErrorCode::Unavailable,
                    "CIRCUIT_OPEN",
                    "Operation temporarily unavailable due to circuit breaker",
                    true
                );
            }
        }

        // 6. Execute via underlying bridge dispatcher.
        auto result = dispatcher_->Dispatch(request);

        // 7. Update circuit breaker only for backend/transient failures.
        if (breaker) {
            if (IsCircuitFailure(result)) {
                breaker->RecordFailure();
            } else {
                breaker->RecordSuccess();
            }
        }

        // 8. Sanitize error messages before returning across boundary.
        if (config_.sanitize_result_errors) {
            SanitizeResultError(result);
        }

        return result;
    }

    [[nodiscard]]
    const SecurityGatewayConfig& Config() const noexcept {
        return config_;
    }

private:
    [[nodiscard]]
    static bool IsCircuitFailure(const bridge::ResultEnvelope& result) noexcept {
        if (result.IsSuccess()) {
            return false;
        }
        using E = bridge::OperationErrorCode;
        switch (result.error.code) {
            case E::Unavailable:
            case E::Internal:
            case E::ExecutionFailed:
            case E::DeadlineExceeded:
                return true;
            default:
                return false;
        }
    }

    [[nodiscard]]
    bridge::ResultEnvelope MakeSecurityFailure(
        const bridge::RequestEnvelope& request,
        bridge::OperationErrorCode code,
        std::string identifier,
        std::string message,
        bool retryable
    ) const {
        bridge::OperationError err;
        err.code = code;
        err.identifier = std::move(identifier);
        err.message = config_.sanitize_result_errors
            ? SanitizeErrorMessage(message, code)
            : std::move(message);
        err.retryable = retryable;
        return bridge::ResultEnvelope::Failure(request, std::move(err));
    }

    std::shared_ptr<bridge::ExecutionDispatcher> dispatcher_;
    std::shared_ptr<IAuthenticator> authenticator_;
    std::shared_ptr<AuthorizationPolicyEngine> authz_;
    std::shared_ptr<TokenBucketRateLimiter> rate_limiter_;
    std::shared_ptr<CircuitBreakerRegistry> circuit_breakers_;
    SecurityGatewayConfig config_;
};

// ============================================================
// HTTP Security Middleware Factory
// ============================================================

struct HttpSecurityPolicy final {
    using OperationResolver = std::function<
        std::string(const http::HttpRequest&, const http::HttpRouteContext&)
    >;

    SecurityLimits limits;
    SafeHeadersConfig safe_headers;
    std::shared_ptr<IAuthenticator> authenticator;
    std::shared_ptr<AuthorizationPolicyEngine> authz;
    std::shared_ptr<TokenBucketRateLimiter> rate_limiter;
    OperationResolver operation_resolver;
    std::string default_peer_ip = "unknown";
};

[[nodiscard]]
inline http::HttpMiddleware MakeHttpSecurityMiddleware(
    HttpSecurityPolicy policy
) {
    return [policy = std::move(policy)](
               const http::HttpRequest& request,
               const http::HttpRouteContext& context,
               const http::HttpMiddlewareNext& next
           ) -> http::HttpResponse {
        // 1. Structural & safety validation on raw HTTP request.
        const auto valid =
            HttpRequestSecurityValidator::Validate(request, policy.limits);
        if (!valid.valid) {
            return BuildSafeErrorResponse(
                valid.http_status,
                valid.identifier,
                request.version(),
                std::nullopt,
                policy.safe_headers
            );
        }

        // Determine logical operation name if resolver is installed.
        std::string operation_id = "http.request";
        if (policy.operation_resolver) {
            auto resolved = policy.operation_resolver(request, context);
            if (!resolved.empty()) {
                operation_id = std::move(resolved);
            }
        } else if (!request.path().empty()) {
            operation_id = "http:" + request.path();
        }

        // 2. Authenticate HTTP credentials before header stripping.
        SecurityContext sec_ctx;
        sec_ctx.source_transport = bridge::TransportKind::Http;
        sec_ctx.client_ip = HttpRequestSecurityValidator::ExtractClientIp(
            request,
            policy.default_peer_ip,
            policy.limits
        );

        const RawCredentials creds = ExtractHttpCredentials(request);
        if (policy.authenticator && !creds.secret.empty()) {
            const auto auth_res = policy.authenticator->Authenticate(creds);
            if (!auth_res.Ok()) {
                return BuildSafeErrorResponse(
                    http::HttpStatus::Unauthorized,
                    auth_res.identifier,
                    request.version(),
                    std::nullopt,
                    policy.safe_headers
                );
            }
            sec_ctx.principal = auth_res.principal;
        }

        // 3. Enforce Authorization policy if configured.
        if (policy.authz) {
            bridge::RequestEnvelope probe;
            probe.operation_id = operation_id;
            probe.transport = bridge::TransportKind::Http;

            const auto decision = policy.authz->Evaluate(sec_ctx, probe);
            if (!decision.allowed) {
                const auto status = bridge::HttpTransportAdapter::MapError(
                    decision.error_code
                );
                return BuildSafeErrorResponse(
                    status,
                    decision.identifier,
                    request.version(),
                    std::nullopt,
                    policy.safe_headers
                );
            }
        }

        // 4. Rate limit by authenticated subject or client IP.
        std::optional<RateLimitDecision> rl_decision;
        if (policy.rate_limiter) {
            const std::string_view key =
                sec_ctx.principal.authenticated && !sec_ctx.principal.subject_id.empty()
                    ? std::string_view(sec_ctx.principal.subject_id)
                    : std::string_view(sec_ctx.client_ip);
            rl_decision = policy.rate_limiter->TryAcquire(key, operation_id);
            if (!rl_decision->allowed) {
                const auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                    rl_decision->retry_after + std::chrono::milliseconds(999)
                );
                auto resp = BuildSafeErrorResponse(
                    http::HttpStatus::TooManyRequests,
                    "rate_limit_exceeded",
                    request.version(),
                    secs,
                    policy.safe_headers
                );
                resp.setHeader("X-RateLimit-Limit", std::to_string(rl_decision->limit));
                resp.setHeader("X-RateLimit-Remaining", "0");
                return resp;
            }
        }

        // 5. Strip untrusted/spoofing headers and credential headers before downstream.
        http::HttpRequest sanitized = request;
        HttpRequestSecurityValidator::StripUntrustedHeaders(
            sanitized,
            policy.limits,
            policy.limits.strip_credential_headers_after_auth
        );

        // Attach verified internal principal header ONLY after stripping untrusted inputs.
        if (sec_ctx.principal.authenticated) {
            sanitized.setHeader(
                "X-Lamp-Verified-Subject",
                sec_ctx.principal.subject_id
            );
            if (!sec_ctx.principal.tenant_id.empty()) {
                sanitized.setHeader(
                    "X-Lamp-Verified-Tenant",
                    sec_ctx.principal.tenant_id
                );
            }
        }

        http::HttpResponse response = next(sanitized, context);
        ApplySafeDefaultHeaders(response, policy.safe_headers);
        if (rl_decision.has_value()) {
            if (!response.header("X-RateLimit-Limit")) {
                response.setHeader("X-RateLimit-Limit", std::to_string(rl_decision->limit));
            }
            if (!response.header("X-RateLimit-Remaining")) {
                response.setHeader(
                    "X-RateLimit-Remaining",
                    std::to_string(rl_decision->remaining_tokens)
                );
            }
        }
        return response;
    };
}

} // namespace lamp::security
