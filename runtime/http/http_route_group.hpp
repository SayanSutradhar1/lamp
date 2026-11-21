
#pragma once

#include "http/http_middleware.hpp"
#include "http/http_router.hpp"

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lamp::http {

// ============================================================
// HTTP Route Group
//
// A lightweight registration-time abstraction over HttpRouter.
//
// Features:
//   - Shared route prefixes
//   - Nested groups
//   - Group-level middleware
//   - Middleware inheritance
//   - All supported HttpMethod values
//   - Existing HttpRouter duplicate-route protection
//
// Lifetime:
//   - The router must outlive any group that registers into it.
//   - Registered handlers do not depend on group lifetime.
//   - Child groups snapshot inherited middleware.
//   - Changes to a parent do not affect existing children.
//   - Registration is a startup/configuration operation.
//
// Routing and dispatch remain owned by HttpRouter.
// ============================================================

class HttpRouteGroupError final : public std::invalid_argument {
public:
    explicit HttpRouteGroupError(const std::string& message)
        : std::invalid_argument(message) {}
};

class HttpRouteGroup final {
public:
    explicit HttpRouteGroup(
        HttpRouter& router,
        std::string prefix = "/"
    )
        : router_(&router),
          prefix_(normalizePrefix(prefix)) {}

    HttpRouteGroup(const HttpRouteGroup&) = default;
    HttpRouteGroup& operator=(const HttpRouteGroup&) = delete;

    HttpRouteGroup(HttpRouteGroup&&) noexcept = default;
    HttpRouteGroup& operator=(HttpRouteGroup&&) = delete;

    // --------------------------------------------------------
    // Group inspection
    // --------------------------------------------------------

    [[nodiscard]]
    const std::string& prefix() const noexcept {
        return prefix_;
    }

    [[nodiscard]]
    std::size_t middlewareCount() const noexcept {
        return middleware_.size();
    }

    [[nodiscard]]
    bool hasMiddleware() const noexcept {
        return !middleware_.empty();
    }

    // --------------------------------------------------------
    // Middleware registration
    //
    // Middleware is applied in registration order.
    // A nested group inherits its parent's middleware,
    // followed by middleware added directly to the child.
    //
    // Middleware added after a route is registered does not
    // retroactively affect that route.
    // --------------------------------------------------------

    HttpRouteGroup& use(HttpMiddleware middleware) {
        if (!middleware) {
            throw HttpRouteGroupError(
                "Cannot register empty route-group middleware"
            );
        }

        middleware_.push_back(std::move(middleware));
        return *this;
    }

    // --------------------------------------------------------
    // Nested groups
    //
    // Example:
    //
    //   HttpRouteGroup api(router, "/api");
    //   auto v1 = api.group("/v1");
    //   auto users = v1.group("/users");
    //
    //   users.get("/:id", handler);
    //
    // Registers:
    //   GET /api/v1/users/:id
    // --------------------------------------------------------

    [[nodiscard]]
    HttpRouteGroup group(std::string prefix) const {
        return HttpRouteGroup(
            *router_,
            joinPath(prefix_, normalizePrefix(prefix)),
            middleware_
        );
    }

    // --------------------------------------------------------
    // Generic route registration
    // --------------------------------------------------------

    void add(
        HttpMethod method,
        std::string pattern,
        HttpRouteHandler handler
    ) {
        if (!handler) {
            throw HttpRouteGroupError(
                "Cannot register empty route-group handler"
            );
        }

        const std::string fullPattern =
            joinRoutePattern(prefix_, pattern);

        // Build a handler snapshot before modifying the router.
        // This preserves the router's normal registration
        // validation and duplicate-route detection.
        HttpMiddlewareChain chain(middleware_);

        router_->add(
            method,
            fullPattern,
            chain.compose(std::move(handler))
        );
    }

    // --------------------------------------------------------
    // HTTP method helpers
    // --------------------------------------------------------

    void get(
        std::string pattern,
        HttpRouteHandler handler
    ) {
        add(
            HttpMethod::Get,
            std::move(pattern),
            std::move(handler)
        );
    }

    void post(
        std::string pattern,
        HttpRouteHandler handler
    ) {
        add(
            HttpMethod::Post,
            std::move(pattern),
            std::move(handler)
        );
    }

    void put(
        std::string pattern,
        HttpRouteHandler handler
    ) {
        add(
            HttpMethod::Put,
            std::move(pattern),
            std::move(handler)
        );
    }

    void patch(
        std::string pattern,
        HttpRouteHandler handler
    ) {
        add(
            HttpMethod::Patch,
            std::move(pattern),
            std::move(handler)
        );
    }

    void del(
        std::string pattern,
        HttpRouteHandler handler
    ) {
        add(
            HttpMethod::Delete,
            std::move(pattern),
            std::move(handler)
        );
    }

    void head(
        std::string pattern,
        HttpRouteHandler handler
    ) {
        add(
            HttpMethod::Head,
            std::move(pattern),
            std::move(handler)
        );
    }

    void options(
        std::string pattern,
        HttpRouteHandler handler
    ) {
        add(
            HttpMethod::Options,
            std::move(pattern),
            std::move(handler)
        );
    }

private:
    // Used only for creating nested groups.
    HttpRouteGroup(
        HttpRouter& router,
        std::string prefix,
        std::vector<HttpMiddleware> inheritedMiddleware
    )
        : router_(&router),
          prefix_(std::move(prefix)),
          middleware_(std::move(inheritedMiddleware)) {}

    // --------------------------------------------------------
    // Prefix validation
    //
    // Group prefixes are intentionally static.
    //
    // Dynamic parameters and terminal wildcards remain
    // supported in route patterns, not group prefixes.
    //
    // This avoids ambiguous nested-prefix composition.
    // --------------------------------------------------------

    [[nodiscard]]
    static std::string normalizePrefix(
        std::string_view prefix
    ) {
        if (prefix.empty() || prefix == "/") {
            return "/";
        }

        if (prefix.front() != '/') {
            throw HttpRouteGroupError(
                "Route-group prefix must begin with '/'"
            );
        }

        if (prefix.find('?') != std::string_view::npos ||
            prefix.find('#') != std::string_view::npos ||
            prefix.find('\\') != std::string_view::npos) {
            throw HttpRouteGroupError(
                "Route-group prefix contains an invalid character"
            );
        }

        std::size_t position = 1;

        while (position < prefix.size()) {
            const auto separator = prefix.find('/', position);

            const auto segment = prefix.substr(
                position,
                separator == std::string_view::npos
                    ? std::string_view::npos
                    : separator - position
            );

            if (segment.empty()) {
                if (separator == prefix.size() - 1) {
                    break;
                }

                throw HttpRouteGroupError(
                    "Route-group prefix contains an empty segment"
                );
            }

            if (segment.front() == ':' ||
                segment.find('*') != std::string_view::npos) {
                throw HttpRouteGroupError(
                    "Route-group prefixes must contain "
                    "only static path segments"
                );
            }

            for (const unsigned char character : segment) {
                if (character <= 0x20 || character == 0x7f) {
                    throw HttpRouteGroupError(
                        "Route-group prefix contains "
                        "a control or whitespace character"
                    );
                }
            }

            if (separator == std::string_view::npos) {
                break;
            }

            position = separator + 1;
        }

        std::string result(prefix);

        // Canonicalize trailing slash for prefixes only.
        // Route patterns still retain trailing-slash semantics.
        if (result.size() > 1 && result.back() == '/') {
            result.pop_back();
        }

        return result;
    }

    // --------------------------------------------------------
    // Prefix joining
    // --------------------------------------------------------

    [[nodiscard]]
    static std::string joinPath(
        std::string_view parent,
        std::string_view child
    ) {
        if (child == "/") {
            return std::string(parent);
        }

        if (parent == "/") {
            return std::string(child);
        }

        std::string result;
        result.reserve(parent.size() + child.size());

        result.append(parent);
        result.append(child);

        return result;
    }

    // --------------------------------------------------------
    // Full route-pattern construction
    //
    // The route pattern must begin with '/'.
    //
    // A group's "/" route maps to its prefix exactly.
    //
    // Example:
    //   Prefix: /api
    //   Pattern: /
    //   Result: /api
    //
    // This avoids implicitly registering /api/ instead.
    // --------------------------------------------------------

    [[nodiscard]]
    static std::string joinRoutePattern(
        std::string_view prefix,
        std::string_view pattern
    ) {
        if (pattern.empty() || pattern.front() != '/') {
            throw HttpRouteGroupError(
                "Grouped route pattern must begin with '/'"
            );
        }

        if (prefix == "/") {
            return std::string(pattern);
        }

        if (pattern == "/") {
            return std::string(prefix);
        }

        std::string result;
        result.reserve(prefix.size() + pattern.size());

        result.append(prefix);
        result.append(pattern);

        return result;
    }

    HttpRouter* router_;
    std::string prefix_;
    std::vector<HttpMiddleware> middleware_;
};

} // namespace lamp::http
