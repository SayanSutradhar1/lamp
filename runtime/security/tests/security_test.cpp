#include "security/auth_policy.hpp"
#include "security/crypto_utils.hpp"
#include "security/input_sanitizer.hpp"
#include "security/rate_limiter.hpp"
#include "security/security_context.hpp"
#include "security/security_gateway.hpp"

#include "bridge/execution_dispatcher.hpp"
#include "bridge/operation_registry.hpp"
#include "http/http_middleware.hpp"
#include "http/http_router.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace lamp;
using namespace lamp::bridge;
using namespace lamp::http;
using namespace lamp::security;

int g_failed = 0;

void Expect(bool cond, const char* expr, const char* file, int line) {
    if (!cond) {
        std::cerr << "[FAIL] " << file << ":" << line << " (" << expr << ")\n";
        ++g_failed;
    }
}

#define EXPECT(expr) Expect(static_cast<bool>(expr), #expr, __FILE__, __LINE__)

RequestEnvelope MakeRequest(OperationId op) {
    static std::atomic<RequestId> next_id{1};
    RequestEnvelope env;
    env.request_id = next_id.fetch_add(1, std::memory_order_relaxed);
    env.operation_id = std::move(op);
    env.transport = TransportKind::Internal;
    env.format = SerializationFormat::Json;
    return env;
}

HttpRequest::Body ToBody(std::string_view text) {
    HttpRequest::Body out;
    out.reserve(text.size());
    for (unsigned char c : text) {
        out.push_back(static_cast<std::byte>(c));
    }
    return out;
}

class FnNativeHandler final : public INativeOperationHandler {
public:
    using Fn = std::function<ResultEnvelope(const RequestEnvelope&)>;
    explicit FnNativeHandler(Fn fn) : fn_(std::move(fn)) {}
    ResultEnvelope Execute(const RequestEnvelope& request) override {
        return fn_(request);
    }
private:
    Fn fn_;
};

std::shared_ptr<OperationRegistry> BuildTestRegistry(
    std::shared_ptr<std::atomic<int>> fail_mode = nullptr,
    std::shared_ptr<std::string> last_observed_subject = nullptr
) {
    auto registry = std::make_shared<OperationRegistry>();

    {
        OperationDescriptor desc;
        desc.id = "system.health";
        desc.execution.mode = ExecutionMode::Native;
        (void)registry->Register(
            desc,
            OperationBackends{
                std::make_shared<FnNativeHandler>([](const RequestEnvelope& req) {
                    return ResultEnvelope::Success(
                        req,
                        {std::uint8_t('o'), std::uint8_t('k')},
                        SerializationFormat::Json,
                        ExecutionBackend::Native
                    );
                }),
                {}
            }
        );
    }

    {
        OperationDescriptor desc;
        desc.id = "users.get";
        desc.execution.mode = ExecutionMode::Native;
        (void)registry->Register(
            desc,
            OperationBackends{
                std::make_shared<FnNativeHandler>(
                    [fail_mode, last_observed_subject](const RequestEnvelope& req) {
                        if (last_observed_subject) {
                            const auto ctx = ExtractSecurityContext(req);
                            *last_observed_subject = ctx.principal.subject_id;
                        }
                        if (fail_mode) {
                            const int mode = fail_mode->load(std::memory_order_acquire);
                            if (mode == 1) {
                                OperationError err;
                                err.code = OperationErrorCode::Unavailable;
                                err.identifier = "UPSTREAM_DOWN";
                                err.message = "C:\\secrets\\db_config.json failed password=supersecret";
                                err.retryable = true;
                                return ResultEnvelope::Failure(req, std::move(err));
                            }
                            if (mode == 2) {
                                OperationError err;
                                err.code = OperationErrorCode::InvalidArgument;
                                err.identifier = "BAD_USER_ID";
                                err.message = "Missing id parameter";
                                return ResultEnvelope::Failure(req, std::move(err));
                            }
                        }
                        return ResultEnvelope::Success(
                            req,
                            {std::uint8_t('{'), std::uint8_t('}')},
                            SerializationFormat::Json,
                            ExecutionBackend::Native
                        );
                    }
                ),
                {}
            }
        );
    }

    {
        OperationDescriptor desc;
        desc.id = "admin.keys.rotate";
        desc.execution.mode = ExecutionMode::Native;
        (void)registry->Register(
            desc,
            OperationBackends{
                std::make_shared<FnNativeHandler>([](const RequestEnvelope& req) {
                    return ResultEnvelope::Success(
                        req,
                        {std::uint8_t('r')},
                        SerializationFormat::Json,
                        ExecutionBackend::Native
                    );
                }),
                {}
            }
        );
    }

    return registry;
}

// ------------------------------------------------------------
// 1. Crypto primitives & constant-time checks
// ------------------------------------------------------------
bool TestCryptoUtils() {
    // FIPS 180-4 SHA-256("abc")
    EXPECT(
        Sha256Hex("abc") ==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
    );

    // RFC 4231 Test Case 2: key = "Jefe", data = "what do ya want for nothing?"
    const auto mac = HmacSha256("Jefe", "what do ya want for nothing?");
    EXPECT(
        ToHexLower(mac) ==
        "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"
    );
    EXPECT(HmacSha256Verify("Jefe", "what do ya want for nothing?", mac));

    auto tampered_mac = mac;
    tampered_mac[0] ^= 0x01U;
    EXPECT(!HmacSha256Verify("Jefe", "what do ya want for nothing?", tampered_mac));

    // Base64URL roundtrip across multiple unpadded lengths
    for (const std::string_view sample : {"", "a", "ab", "abc", "hello lamp security"}) {
        const auto enc = Base64UrlEncode(sample);
        const auto dec = Base64UrlDecode(enc);
        EXPECT(dec.has_value());
        const std::string recovered(
            reinterpret_cast<const char*>(dec->data()),
            dec->size()
        );
        EXPECT(recovered == sample);
    }
    EXPECT(!Base64UrlDecode("invalid!base64").has_value());

    EXPECT(ConstantTimeEquals("secret-token-1", "secret-token-1"));
    EXPECT(!ConstantTimeEquals("secret-token-1", "secret-token-2"));
    EXPECT(!ConstantTimeEquals("short", "longer"));

    return true;
}

// ------------------------------------------------------------
// 2. Untrusted header stripping, path/metadata injection, limits
// ------------------------------------------------------------
bool TestInputValidationAndUntrustedStripping() {
    SecurityLimits limits;
    limits.max_body_bytes = 64;
    limits.max_path_bytes = 40;
    limits.allowed_content_types = {"application/json", "application/x-protobuf"};

    // Path traversal variants rejected
    {
        HttpRequest req(HttpMethod::Get, "/files/../../etc/passwd");
        req.setHeader("Host", "api.example.com");
        const auto res = HttpRequestSecurityValidator::Validate(req, limits);
        EXPECT(!res.valid);
        EXPECT(res.http_status == HttpStatus::BadRequest);
        EXPECT(res.identifier == "PATH_TRAVERSAL_REJECTED");
    }
    {
        HttpRequest req(HttpMethod::Get, "/files/%2e%2e/secret");
        req.setHeader("Host", "api.example.com");
        const auto res = HttpRequestSecurityValidator::Validate(req, limits);
        EXPECT(!res.valid);
        EXPECT(res.identifier == "PATH_TRAVERSAL_REJECTED");
    }
    {
        HttpRequest req(HttpMethod::Get, "/files/..\\secret");
        req.setHeader("Host", "api.example.com");
        const auto res = HttpRequestSecurityValidator::Validate(req, limits);
        EXPECT(!res.valid);
    }

    // Missing Host on HTTP/1.1 & userinfo injection rejected
    {
        HttpRequest req(HttpMethod::Get, "/users");
        const auto res = HttpRequestSecurityValidator::Validate(req, limits);
        EXPECT(!res.valid);
        EXPECT(res.identifier == "INVALID_HOST_HEADER");
    }
    {
        HttpRequest req(HttpMethod::Get, "/users");
        req.setHeader("Host", "attacker@internal.local");
        const auto res = HttpRequestSecurityValidator::Validate(req, limits);
        EXPECT(!res.valid);
        EXPECT(res.identifier == "INVALID_HOST_HEADER");
    }

    // CL + TE smuggling rejected
    {
        HttpRequest req(HttpMethod::Post, "/rpc");
        req.setHeader("Host", "api.example.com");
        req.setHeader("Content-Length", "4");
        req.setHeader("Transfer-Encoding", "chunked");
        const auto res = HttpRequestSecurityValidator::Validate(req, limits);
        EXPECT(!res.valid);
        EXPECT(res.identifier == "AMBIGUOUS_FRAMING_HEADERS");
    }

    // Body size limit -> 413 PayloadTooLarge, Unsupported Media Type -> 415
    {
        HttpRequest req(HttpMethod::Post, "/rpc");
        req.setHeader("Host", "api.example.com");
        req.setHeader("Content-Type", "application/json; charset=utf-8");
        req.setBody(ToBody(std::string(65, 'x')));
        const auto res = HttpRequestSecurityValidator::Validate(req, limits);
        EXPECT(!res.valid);
        EXPECT(res.http_status == HttpStatus::PayloadTooLarge);
    }
    {
        HttpRequest req(HttpMethod::Post, "/rpc");
        req.setHeader("Host", "api.example.com");
        req.setHeader("Content-Type", "text/html");
        req.setBody(ToBody("hello"));
        const auto res = HttpRequestSecurityValidator::Validate(req, limits);
        EXPECT(!res.valid);
        EXPECT(res.http_status == HttpStatus::UnsupportedMediaType);
    }

    // Untrusted header stripping + non-trusting X-Forwarded-For default
    {
        HttpRequest req(HttpMethod::Get, "/users");
        req.setHeader("Host", "api.example.com");
        req.setHeader("Authorization", "Bearer topsecret");
        req.setHeader("Cookie", "session=hijack");
        req.setHeader("X-Lamp-Verified-Subject", "spoofed-admin");
        req.setHeader("X-Forwarded-For", "1.2.3.4");
        req.setHeader("X-Request-Id", "req-123");

        EXPECT(
            HttpRequestSecurityValidator::ExtractClientIp(req, "127.0.0.1", limits) ==
            "127.0.0.1"
        );
        const std::size_t stripped =
            HttpRequestSecurityValidator::StripUntrustedHeaders(req, limits, true);
        EXPECT(stripped == 4);
        EXPECT(!req.header("Authorization").has_value());
        EXPECT(!req.header("Cookie").has_value());
        EXPECT(!req.header("X-Lamp-Verified-Subject").has_value());
        EXPECT(!req.header("X-Forwarded-For").has_value());
        EXPECT(req.header("X-Request-Id") == "req-123");
    }

    // Bridge envelope metadata spoofing strip & key injection rejection
    {
        RequestEnvelope env;
        env.operation_id = "users.get";
        env.AddMetadata("lamp.auth.subject", "spoofed-root");
        env.AddMetadata("lamp.auth.roles", "admin");
        env.AddMetadata("client.region", "us-east");

        const auto res = EnvelopeSecuritySanitizer::SanitizeAndValidate(env, limits);
        EXPECT(res.valid);
        EXPECT(env.metadata.size() == 1);
        EXPECT(env.metadata[0].key == "client.region");
    }
    {
        RequestEnvelope bad;
        bad.operation_id = "users.get";
        bad.AddMetadata("bad key/with$chars", "val");
        const auto res = EnvelopeSecuritySanitizer::SanitizeAndValidate(bad, limits);
        EXPECT(!res.valid);
        EXPECT(res.identifier == "INVALID_METADATA_KEY");
    }

    return true;
}

// ------------------------------------------------------------
// 3. Authn & Authz policy enforcement (401 / 403 / spoof resist)
// ------------------------------------------------------------
bool TestAuthnAndAuthzEnforcement() {
    auto now = std::make_shared<std::chrono::system_clock::time_point>(
        std::chrono::system_clock::time_point(std::chrono::seconds(1000000))
    );

    auto token_auth = std::make_shared<SignedTokenAuthenticator>(
        "super-secret-signing-key-2026",
        [now] { return *now; }
    );
    auto apikey_auth = std::make_shared<ApiKeyAuthenticator>();
    apikey_auth->RegisterKey(
        "lamp_live_key_12345",
        Principal{
            true,
            "svc-worker",
            "tenant-a",
            "apikey",
            {"service"},
            {"users:read"}
        }
    );

    auto composite = std::make_shared<CompositeAuthenticator>();
    composite->AddProvider(token_auth);
    composite->AddProvider(apikey_auth);

    const std::string valid_user_token = token_auth->IssueToken(SignedTokenClaims{
        "user-42",
        "tenant-a",
        {"member"},
        {"users:read"},
        1000100
    });
    const std::string admin_token = token_auth->IssueToken(SignedTokenClaims{
        "admin-1",
        "tenant-ops",
        {"admin"},
        {"keys:rotate"},
        1000100
    });

    auto authz = std::make_shared<AuthorizationPolicyEngine>(true);
    authz->SetPolicy("system.health", OperationAuthPolicy{.allow_anonymous = true});
    authz->SetPolicy(
        "users.get",
        OperationAuthPolicy{
            .allow_anonymous = false,
            .required_roles = {"member", "service"},
            .required_scopes = {"users:read"},
            .allowed_tenants = {"tenant-a"}
        }
    );
    authz->SetPolicy(
        "admin.*",
        OperationAuthPolicy{
            .allow_anonymous = false,
            .required_roles = {"admin"},
            .required_scopes = {"keys:rotate"}
        }
    );

    auto last_subject = std::make_shared<std::string>();
    auto registry = BuildTestRegistry(nullptr, last_subject);
    auto base_disp = std::make_shared<ExecutionDispatcher>(registry);

    SecuredOperationDispatcher secured(base_disp, composite, authz);

    // Public endpoint succeeds anonymously
    {
        auto req = MakeRequest("system.health");
        const auto res = secured.Dispatch(req);
        EXPECT(res.IsSuccess());
    }

    // Protected endpoint without credentials -> Unauthenticated (401)
    // Even if attacker tries to inject spoofed lamp.auth.* metadata!
    {
        auto req = MakeRequest("users.get");
        req.AddMetadata("lamp.auth.authenticated", "true");
        req.AddMetadata("lamp.auth.subject", "spoofed-user");
        req.AddMetadata("lamp.auth.roles", "member,admin");
        const auto res = secured.Dispatch(req);
        EXPECT(!res.IsSuccess());
        EXPECT(res.error.code == OperationErrorCode::Unauthenticated);
    }

    // Tampered token -> Unauthenticated (401)
    {
        std::string bad_tok = valid_user_token;
        bad_tok[bad_tok.size() - 5] = (bad_tok[bad_tok.size() - 5] == 'A') ? 'B' : 'A';
        auto req = MakeRequest("users.get");
        const auto res = secured.Dispatch(req, RawCredentials{"bearer", bad_tok});
        EXPECT(!res.IsSuccess());
        EXPECT(res.error.code == OperationErrorCode::Unauthenticated);
    }

    // Authenticated member succeeds on users.get and receives verified subject
    {
        auto req = MakeRequest("users.get");
        req.AddMetadata("lamp.auth.subject", "forged-override");
        const auto res = secured.Dispatch(req, RawCredentials{"bearer", valid_user_token});
        EXPECT(res.IsSuccess());
        EXPECT(*last_subject == "user-42");
    }

    // Authenticated member lacks admin role/scope -> PermissionDenied (403)
    {
        auto req = MakeRequest("admin.keys.rotate");
        const auto res = secured.Dispatch(req, RawCredentials{"bearer", valid_user_token});
        EXPECT(!res.IsSuccess());
        EXPECT(res.error.code == OperationErrorCode::PermissionDenied);
    }

    // Admin token succeeds on wildcard admin.* policy
    {
        auto req = MakeRequest("admin.keys.rotate");
        const auto res = secured.Dispatch(req, RawCredentials{"bearer", admin_token});
        EXPECT(res.IsSuccess());
    }

    // API key succeeds, then fails after revocation
    {
        auto req = MakeRequest("users.get");
        EXPECT(secured.Dispatch(req, RawCredentials{"apikey", "lamp_live_key_12345"}).IsSuccess());
        EXPECT(*last_subject == "svc-worker");

        EXPECT(apikey_auth->RevokeKey("lamp_live_key_12345"));
        const auto after_revoke =
            secured.Dispatch(req, RawCredentials{"apikey", "lamp_live_key_12345"});
        EXPECT(!after_revoke.IsSuccess());
        EXPECT(after_revoke.error.code == OperationErrorCode::Unauthenticated);
    }

    // Advance virtual clock past token expiry -> Unauthenticated (401)
    *now += std::chrono::seconds(200);
    {
        auto req = MakeRequest("users.get");
        const auto res = secured.Dispatch(req, RawCredentials{"bearer", valid_user_token});
        EXPECT(!res.IsSuccess());
        EXPECT(res.error.code == OperationErrorCode::Unauthenticated);
    }

    return true;
}

// ------------------------------------------------------------
// 4. Token-bucket rate limiter & key-flood eviction
// ------------------------------------------------------------
bool TestRateLimiterAndKeyFloodBound() {
    auto now = std::make_shared<std::chrono::steady_clock::time_point>(
        std::chrono::steady_clock::time_point(std::chrono::seconds(100))
    );

    RateLimitPolicy default_policy;
    default_policy.capacity = 3.0;
    default_policy.refill_per_second = 1.0;
    default_policy.max_tracked_keys = 8;
    default_policy.shard_count = 4;

    TokenBucketRateLimiter limiter(default_policy, [now] { return *now; });
    limiter.SetOperationPolicy("expensive.op", RateLimitPolicy{1.0, 0.5, 8, 4});

    // Exhaust 3 burst tokens
    for (int i = 0; i < 3; ++i) {
        const auto d = limiter.TryAcquire("user-1", "users.get");
        EXPECT(d.allowed);
        EXPECT(d.remaining_tokens == static_cast<std::uint64_t>(2 - i));
    }

    const auto blocked = limiter.TryAcquire("user-1", "users.get");
    EXPECT(!blocked.allowed);
    EXPECT(blocked.retry_after.count() >= 1000);

    // Independent key is unaffected
    EXPECT(limiter.TryAcquire("user-2", "users.get").allowed);

    // Advance clock 2s -> 2 tokens refilled
    *now += std::chrono::seconds(2);
    EXPECT(limiter.TryAcquire("user-1", "users.get").allowed);
    EXPECT(limiter.TryAcquire("user-1", "users.get").allowed);
    EXPECT(!limiter.TryAcquire("user-1", "users.get").allowed);

    // Per-operation stricter override
    EXPECT(limiter.TryAcquire("user-3", "expensive.op").allowed);
    EXPECT(!limiter.TryAcquire("user-3", "expensive.op").allowed);

    // Flood 500 distinct keys -> memory stays bounded by max_tracked_keys (8)
    for (int i = 0; i < 500; ++i) {
        (void)limiter.TryAcquire("attacker-" + std::to_string(i), "users.get");
    }
    EXPECT(limiter.TrackedKeyCount() <= 8);

    return true;
}

// ------------------------------------------------------------
// 5. Circuit breaker & secret-free error sanitization
// ------------------------------------------------------------
bool TestCircuitBreakerAndSecretFreeErrors() {
    auto now = std::make_shared<std::chrono::steady_clock::time_point>(
        std::chrono::steady_clock::time_point(std::chrono::seconds(50))
    );

    auto fail_mode = std::make_shared<std::atomic<int>>(2); // client error InvalidArgument
    auto registry = BuildTestRegistry(fail_mode);
    auto base_disp = std::make_shared<ExecutionDispatcher>(registry);

    auto authz = std::make_shared<AuthorizationPolicyEngine>(false);
    auto breakers = std::make_shared<CircuitBreakerRegistry>(
        CircuitBreakerConfig{3, std::chrono::milliseconds(5000), 1, 2},
        [now] { return *now; }
    );

    SecurityGatewayConfig cfg;
    cfg.enable_rate_limiting = false;

    SecuredOperationDispatcher secured(
        base_disp, nullptr, authz, nullptr, breakers, cfg
    );

    auto req = MakeRequest("users.get");

    // Client 4xx errors do NOT trip the circuit breaker
    for (int i = 0; i < 5; ++i) {
        const auto res = secured.Dispatch(req);
        EXPECT(!res.IsSuccess());
        EXPECT(res.error.code == OperationErrorCode::InvalidArgument);
    }
    EXPECT(breakers->GetOrCreate("users.get")->State() == CircuitState::Closed);

    // Upstream/transient errors trip breaker at threshold=3 AND sanitize secret/path leak
    fail_mode->store(1, std::memory_order_release);
    for (int i = 0; i < 3; ++i) {
        const auto res = secured.Dispatch(req);
        EXPECT(!res.IsSuccess());
        EXPECT(res.error.code == OperationErrorCode::Unavailable);
        EXPECT(res.error.message.find("supersecret") == std::string::npos);
        EXPECT(res.error.message.find("C:\\") == std::string::npos);
    }
    EXPECT(breakers->GetOrCreate("users.get")->State() == CircuitState::Open);

    // While Open, requests fail-fast without calling backend
    fail_mode->store(0, std::memory_order_release);
    {
        const auto fast_fail = secured.Dispatch(req);
        EXPECT(!fast_fail.IsSuccess());
        EXPECT(fast_fail.error.code == OperationErrorCode::Unavailable);
        EXPECT(fast_fail.error.identifier == "CIRCUIT_OPEN");
    }

    // Advance past open_duration -> HalfOpen -> 2 successes -> Closed
    *now += std::chrono::milliseconds(5000);
    EXPECT(breakers->GetOrCreate("users.get")->State() == CircuitState::HalfOpen);
    EXPECT(secured.Dispatch(req).IsSuccess());
    EXPECT(secured.Dispatch(req).IsSuccess());
    EXPECT(breakers->GetOrCreate("users.get")->State() == CircuitState::Closed);

    return true;
}

// ------------------------------------------------------------
// 6. HTTP Security Middleware + Router integration
// ------------------------------------------------------------
bool TestHttpSecurityMiddlewareIntegration() {
    auto token_auth = std::make_shared<SignedTokenAuthenticator>("http-gw-secret");
    const std::string tok = token_auth->IssueToken(SignedTokenClaims{
        "alice",
        "t1",
        {"reader"},
        {"items:read"},
        0
    });

    auto authz = std::make_shared<AuthorizationPolicyEngine>(true);
    authz->SetPolicy(
        "items.list",
        OperationAuthPolicy{
            .allow_anonymous = false,
            .required_roles = {"reader"}
        }
    );
    authz->SetPolicy(
        "items.admin",
        OperationAuthPolicy{
            .allow_anonymous = false,
            .required_roles = {"admin"}
        }
    );

    auto limiter = std::make_shared<TokenBucketRateLimiter>(
        RateLimitPolicy{2.0, 0.0, 64, 4}
    );

    HttpSecurityPolicy gw_policy;
    gw_policy.authenticator = token_auth;
    gw_policy.authz = authz;
    gw_policy.rate_limiter = limiter;
    gw_policy.default_peer_ip = "10.0.0.5";
    gw_policy.operation_resolver = [](const HttpRequest& r, const HttpRouteContext&) {
        if (r.path() == "/admin") {
            return std::string("items.admin");
        }
        return std::string("items.list");
    };

    HttpRouter router;
    auto handler = ComposeHttpMiddleware(
        [](const HttpRequest& r, const HttpRouteContext&) {
            // Confirm credential & spoofed headers were stripped and verified subject injected
            EXPECT(!r.header("Authorization").has_value());
            EXPECT(!r.header("Cookie").has_value());
            EXPECT(!r.header("X-Forwarded-For").has_value());
            EXPECT(r.header("X-Lamp-Verified-Subject") == "alice");
            HttpResponse resp(HttpStatus::Ok, r.version());
            resp.setBody("ok");
            return resp;
        },
        {MakeHttpSecurityMiddleware(gw_policy)}
    );
    router.add(HttpMethod::Get, "/items", handler);
    router.add(HttpMethod::Get, "/admin", handler);

    // 401 when unauthenticated
    {
        HttpRequest req(HttpMethod::Get, "/items");
        req.setHeader("Host", "api.example.com");
        const auto resp = router.dispatch(req);
        EXPECT(resp.statusCode() == 401);
        EXPECT(resp.header("X-Content-Type-Options") == "nosniff");
        EXPECT(resp.header("X-Frame-Options") == "DENY");
        EXPECT(resp.header("Cache-Control") == "no-store");
        EXPECT(resp.header("WWW-Authenticate").has_value());
    }

    // 403 when lacking role
    {
        HttpRequest req(HttpMethod::Get, "/admin");
        req.setHeader("Host", "api.example.com");
        req.setHeader("Authorization", "Bearer " + tok);
        const auto resp = router.dispatch(req);
        EXPECT(resp.statusCode() == 403);
    }

    // 200 x2, then 429 on 3rd request (capacity = 2)
    for (int i = 0; i < 2; ++i) {
        HttpRequest req(HttpMethod::Get, "/items");
        req.setHeader("Host", "api.example.com");
        req.setHeader("Authorization", "Bearer " + tok);
        req.setHeader("Cookie", "evil=1");
        req.setHeader("X-Forwarded-For", "9.9.9.9");
        req.setHeader("X-Lamp-Verified-Subject", "attacker");
        const auto resp = router.dispatch(req);
        EXPECT(resp.statusCode() == 200);
        EXPECT(resp.header("X-Content-Type-Options") == "nosniff");
        EXPECT(resp.header("X-RateLimit-Limit") == "2");
    }

    {
        HttpRequest req(HttpMethod::Get, "/items");
        req.setHeader("Host", "api.example.com");
        req.setHeader("Authorization", "Bearer " + tok);
        const auto resp = router.dispatch(req);
        EXPECT(resp.statusCode() == 429);
        EXPECT(resp.header("Retry-After").has_value());
        EXPECT(resp.header("X-RateLimit-Remaining") == "0");
    }

    // 400 on path traversal attempt even with valid auth
    {
        HttpRequest req(HttpMethod::Get, "/items/../../etc/shadow");
        req.setHeader("Host", "api.example.com");
        req.setHeader("Authorization", "Bearer " + tok);
        // Dispatch directly via composed handler on raw request check:
        const auto resp = handler(req, HttpRouteContext{});
        EXPECT(resp.statusCode() == 400);
    }

    return true;
}

// ------------------------------------------------------------
// 7. Concurrent security gateway stress test
// ------------------------------------------------------------
bool TestConcurrentSecurityStress() {
    auto token_auth = std::make_shared<SignedTokenAuthenticator>("concurrent-secret");
    const std::string user_tok = token_auth->IssueToken(SignedTokenClaims{
        "concurrent-user",
        "t1",
        {"member"},
        {"users:read"},
        0
    });

    auto authz = std::make_shared<AuthorizationPolicyEngine>(true);
    authz->SetPolicy(
        "users.get",
        OperationAuthPolicy{
            .allow_anonymous = false,
            .required_roles = {"member"}
        }
    );

    // 40 tokens available total for concurrent-user
    auto limiter = std::make_shared<TokenBucketRateLimiter>(
        RateLimitPolicy{40.0, 0.0, 1024, 16}
    );

    auto registry = BuildTestRegistry();
    auto base_disp = std::make_shared<ExecutionDispatcher>(registry);
    SecuredOperationDispatcher secured(base_disp, token_auth, authz, limiter);

    std::atomic<int> ok_count{0};
    std::atomic<int> unauth_count{0};
    std::atomic<int> rate_limited_count{0};
    std::atomic<int> unexpected_count{0};

    constexpr int kThreads = 8;
    constexpr int kIters = 25; // 200 requests total
    std::vector<std::thread> workers;
    workers.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&, t] {
            for (int i = 0; i < kIters; ++i) {
                auto req = MakeRequest("users.get");
                req.AddMetadata("lamp.auth.subject", "spoofed");

                if ((t + i) % 5 == 0) {
                    // Invalid token batch -> 401
                    const auto res = secured.Dispatch(req, RawCredentials{"bearer", "v1.bad.sig"});
                    if (res.error.code == OperationErrorCode::Unauthenticated) {
                        unauth_count.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        unexpected_count.fetch_add(1, std::memory_order_relaxed);
                    }
                } else {
                    const auto res = secured.Dispatch(req, RawCredentials{"bearer", user_tok});
                    if (res.IsSuccess()) {
                        ok_count.fetch_add(1, std::memory_order_relaxed);
                    } else if (res.error.code == OperationErrorCode::ResourceExhausted) {
                        rate_limited_count.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        unexpected_count.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        });
    }

    for (auto& w : workers) {
        w.join();
    }

    EXPECT(unexpected_count.load() == 0);
    EXPECT(ok_count.load() == 40); // exact burst cap enforced thread-safely
    EXPECT(unauth_count.load() == 40);
    EXPECT(rate_limited_count.load() == 120);

    return true;
}

} // namespace

int main() {
    struct Case {
        const char* name;
        bool (*fn)();
    };

    const Case cases[] = {
        {"CryptoUtilsSha256HmacAndConstantTime", TestCryptoUtils},
        {"UntrustedHeaderAndMetadataStrippingAndInjection", TestInputValidationAndUntrustedStripping},
        {"SignedTokenAndApiKeyAuthnAndRbacAuthz", TestAuthnAndAuthzEnforcement},
        {"RateLimiterTokenBucketAndKeyFloodBoundedness", TestRateLimiterAndKeyFloodBound},
        {"CircuitBreakerAndSecretFreeErrorSanitization", TestCircuitBreakerAndSecretFreeErrors},
        {"HttpSecurityMiddlewareEndToEndWithRouter", TestHttpSecurityMiddlewareIntegration},
        {"ConcurrentSecurityGatewayStress", TestConcurrentSecurityStress},
    };

    int passed = 0;
    const int total = static_cast<int>(sizeof(cases) / sizeof(cases[0]));

    for (const auto& c : cases) {
        const int before = g_failed;
        if (c.fn() && g_failed == before) {
            std::cout << "[PASS] " << c.name << "\n";
            ++passed;
        } else {
            std::cout << "[FAIL] " << c.name << "\n";
        }
    }

    std::cout << "Lamp security: " << passed << "/" << total << " tests passed\n";
    return (passed == total && g_failed == 0) ? 0 : 1;
}
