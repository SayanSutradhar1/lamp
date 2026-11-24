
#pragma once

#include "core/origin_policy.hpp"
#include "http/http_router.hpp"

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace lamp::http {

// Middleware is synchronous and executes on the HTTP dispatch thread.
// A middleware may:
//   - Return a response without calling next (short-circuit).
//   - Call next exactly once and modify its returned response.
//
// The next continuation is valid only during the middleware invocation.
// It must never be retained or called asynchronously.

class HttpMiddlewareError final : public std::logic_error {
public:
    explicit HttpMiddlewareError(const char* message)
        : std::logic_error(message) {}
};

using HttpMiddlewareNext = HttpRouteHandler;

using HttpMiddleware = std::function<
    HttpResponse(
        const HttpRequest&,
        const HttpRouteContext&,
        const HttpMiddlewareNext&
    )
>;

class HttpMiddlewareChain final {
public:
    HttpMiddlewareChain() = default;

    explicit HttpMiddlewareChain(
        std::vector<HttpMiddleware> middleware
    ) {
        for (auto& entry : middleware) {
            add(std::move(entry));
        }
    }

    void add(HttpMiddleware middleware) {
        if (!middleware) {
            throw HttpMiddlewareError(
                "Cannot register empty HTTP middleware"
            );
        }

        middleware_.push_back(std::move(middleware));
    }

    [[nodiscard]]
    std::size_t size() const noexcept {
        return middleware_.size();
    }

    [[nodiscard]]
    bool empty() const noexcept {
        return middleware_.empty();
    }

    void clear() noexcept {
        middleware_.clear();
    }

    // Produces a self-contained handler snapshot.
    //
    // Middleware executes in registration order on the
    // way in, and reverse order on the way out.
    //
    // The resulting handler does not depend on the lifetime
    // of this chain and can be registered with HttpRouter.
    //
    // After composition, the middleware collection is
    // immutable for each handler snapshot. Individual
    // middleware callables must be concurrency-safe.

    [[nodiscard]]
    HttpRouteHandler compose(
        HttpRouteHandler terminal
    ) const {
        if (!terminal) {
            throw HttpMiddlewareError(
                "Cannot compose an empty HTTP route handler"
            );
        }

        HttpRouteHandler current = std::move(terminal);

        for (auto it = middleware_.rbegin();
             it != middleware_.rend();
             ++it) {

            HttpMiddleware middleware = *it;
            HttpRouteHandler downstream = std::move(current);

            current = [
                middleware = std::move(middleware),
                downstream = std::move(downstream)
            ](
                const HttpRequest& request,
                const HttpRouteContext& context
            ) -> HttpResponse {

                // Each request invocation owns its own guard.
                // This prevents duplicate downstream execution
                // even when a handler is invoked concurrently.
                auto called = std::make_shared<
                    std::atomic<bool>
                >(false);

                HttpMiddlewareNext next = [
                    downstream,
                    called
                ](
                    const HttpRequest& nextRequest,
                    const HttpRouteContext& nextContext
                ) -> HttpResponse {

                    bool expected = false;

                    if (!called->compare_exchange_strong(
                            expected,
                            true,
                            std::memory_order_acq_rel,
                            std::memory_order_acquire
                        )) {
                        throw HttpMiddlewareError(
                            "HTTP middleware called next more than once"
                        );
                    }

                    return downstream(
                        nextRequest,
                        nextContext
                    );
                };

                return middleware(
                    request,
                    context,
                    next
                );
            };
        }

        return current;
    }

private:
    std::vector<HttpMiddleware> middleware_;
};

// Convenience function for composing an existing handler
// without explicitly constructing a chain.

[[nodiscard]]
inline HttpRouteHandler ComposeHttpMiddleware(
    HttpRouteHandler terminal,
    std::vector<HttpMiddleware> middleware
) {
    return HttpMiddlewareChain(
        std::move(middleware)
    ).compose(std::move(terminal));
}

[[nodiscard]]
inline HttpMiddleware MakeOriginAllowanceMiddleware(
    std::shared_ptr<core::OriginAllowancePolicy> policy
) {
    return [policy = std::move(policy)](
        const HttpRequest& request,
        const HttpRouteContext& context,
        const HttpMiddlewareNext& next
    ) -> HttpResponse {
        if (!policy) {
            return next(request, context);
        }

        const auto originHeader = request.header("Origin");
        const auto eval = policy->evaluate(originHeader);
        if (!eval.allowed) {
            HttpResponse denied;
            denied.setStatus(HttpStatus::Forbidden);
            denied.setHeader("Content-Type", "application/json");
            denied.setHeader("X-Lamp-Cors-Error", eval.errorCode);
            denied.setBody(
                "{\"error\":true,\"code\":\"PERMISSION_DENIED\",\"identifier\":\"" +
                eval.errorCode + "\",\"message\":\"" + eval.errorMessage + "\"}"
            );
            return denied;
        }

        if (request.method() == HttpMethod::Options && originHeader.has_value()) {
            HttpResponse preflight;
            preflight.setStatus(HttpStatus::NoContent);
            if (!eval.accessControlAllowOrigin.empty()) {
                preflight.setHeader("Access-Control-Allow-Origin", eval.accessControlAllowOrigin);
                preflight.setHeader("Vary", "Origin");
            }
            preflight.setHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, PATCH, OPTIONS");
            preflight.setHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");
            return preflight;
        }

        HttpResponse response = next(request, context);
        if (!eval.accessControlAllowOrigin.empty()) {
            response.setHeader("Access-Control-Allow-Origin", eval.accessControlAllowOrigin);
            response.setHeader("Vary", "Origin");
        }
        return response;
    };
}

} // namespace lamp::http
