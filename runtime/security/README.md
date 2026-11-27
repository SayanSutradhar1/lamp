# runtime/security

Authentication, authorization (RBAC/scopes/tenants), rate limiting, and input sanitization enforced natively before handler execution.

## Key Components

- `security_gateway.hpp` & `security_context.hpp` — Request security evaluation and verified `SecurityContext`
- `auth_policy.hpp` — Role, scope, tenant, and anonymous access policies
- `crypto_utils.hpp` — SHA-256 / HMAC-SHA256 token verification and API key hashing
- `rate_limiter.hpp` — Sharded LRU token-bucket rate limiter
- `input_sanitizer.hpp` — Payload sanitization and bounds validation
