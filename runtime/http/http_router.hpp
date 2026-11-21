
#pragma once

#include "http/http_method.hpp"
#include "http/http_request.hpp"
#include "http/http_response.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lamp::http {

// ============================================================
// HTTP Router Errors
// ============================================================

class HttpRouterError final : public std::runtime_error {
public:
    explicit HttpRouterError(const std::string& message)
        : std::runtime_error(message) {}
};

// ============================================================
// Route Parameters
// ============================================================
//
// Named parameters are extracted from matching paths.
//
// Example:
//
//   Pattern: /users/:id
//   Request: /users/42
//
//   params["id"] == "42"
//
// Values are stored without percent-decoding.
// The application must decode and validate them according
// to its own security and routing requirements.
//
// ============================================================

using HttpRouteParams =
    std::unordered_map<std::string, std::string>;

// ============================================================
// Route Context
// ============================================================

struct HttpRouteContext final {
    HttpRouteParams params;

    [[nodiscard]]
    std::string_view param(
        std::string_view name
    ) const noexcept {
        const auto iterator = params.find(
            std::string(name)
        );

        if (iterator == params.end()) {
            return {};
        }

        return iterator->second;
    }

    [[nodiscard]]
    bool hasParam(
        std::string_view name
    ) const {
        return params.find(std::string(name)) != params.end();
    }
};

// ============================================================
// Route Handler
// ============================================================
//
// Handlers receive the parsed request and route context.
//
// The handler returns a fully owned HTTP response.
//
// Handlers must be safe for concurrent invocation when
// the HTTP server dispatches requests across workers.
//
// ============================================================

using HttpRouteHandler = std::function<
    HttpResponse(
        const HttpRequest&,
        const HttpRouteContext&
    )
>;

// ============================================================
// Route Pattern
// ============================================================
//
// Supported syntax:
//
//   /users           Static route
//   /users/:id       Named parameter
//   /files/*         Terminal wildcard
//   /               Root route
//
// Wildcards must be the final path segment.
//
// Matching is case-sensitive.
//
// Trailing slashes are significant:
//
//   /users != /users/
//
// ============================================================

class HttpRoutePattern final {
public:
    enum class SegmentType {
        Static,
        Parameter,
        Wildcard
    };

    struct Segment final {
        SegmentType type;
        std::string value;
    };

    explicit HttpRoutePattern(
        std::string pattern
    )
        : pattern_(std::move(pattern)) {
        parse();
    }

    [[nodiscard]]
    const std::string& value() const noexcept {
        return pattern_;
    }

    [[nodiscard]]
    const std::vector<Segment>& segments() const noexcept {
        return segments_;
    }

    [[nodiscard]]
    bool matches(
        std::string_view path,
        HttpRouteParams& params
    ) const {
        HttpRouteParams extracted;

        if (path.empty() || path.front() != '/') {
            return false;
        }

        const auto pathSegments = splitPath(path);

        std::size_t patternIndex = 0;
        std::size_t pathIndex = 0;

        while (patternIndex < segments_.size()) {
            const auto& segment = segments_[patternIndex];

            if (segment.type == SegmentType::Wildcard) {
                params = std::move(extracted);
                return true;
            }

            if (pathIndex >= pathSegments.size()) {
                return false;
            }

            const auto candidate = pathSegments[pathIndex];

            if (segment.type == SegmentType::Static) {
                if (segment.value != candidate) {
                    return false;
                }
            } else {
                if (candidate.empty()) {
                    return false;
                }

                extracted.emplace(
                    segment.value,
                    std::string(candidate)
                );
            }

            ++patternIndex;
            ++pathIndex;
        }

        if (pathIndex != pathSegments.size()) {
            return false;
        }

        params = std::move(extracted);
        return true;
    }

    [[nodiscard]]
    std::size_t specificity() const noexcept {
        std::size_t score = 0;

        for (const auto& segment : segments_) {
            switch (segment.type) {
                case SegmentType::Static:
                    score += 100;
                    break;

                case SegmentType::Parameter:
                    score += 10;
                    break;

                case SegmentType::Wildcard:
                    score += 1;
                    break;
            }
        }

        return score;
    }

private:
    // ========================================================
    // Path Splitting
    // ========================================================

    [[nodiscard]]
    static std::vector<std::string_view> splitPath(
        std::string_view path
    ) {
        std::vector<std::string_view> result;

        if (path == "/") {
            return result;
        }

        if (path.empty() || path.front() != '/') {
            return result;
        }

        path.remove_prefix(1);

        while (true) {
            const auto separator = path.find('/');

            if (separator == std::string_view::npos) {
                result.push_back(path);
                break;
            }

            result.push_back(path.substr(0, separator));
            path.remove_prefix(separator + 1);
        }

        return result;
    }

    // ========================================================
    // Pattern Parsing
    // ========================================================

    void parse() {
        if (pattern_.empty() ||
            pattern_.front() != '/') {
            throw HttpRouterError(
                "Route patterns must begin with '/'"
            );
        }

        const auto parts = splitPath(pattern_);

        std::unordered_set<std::string> parameterNames;

        for (std::size_t i = 0; i < parts.size(); ++i) {
            const auto part = parts[i];

            if (part == "*") {
                if (i + 1 != parts.size()) {
                    throw HttpRouterError(
                        "Wildcard must be the final "
                        "route segment"
                    );
                }

                segments_.push_back({
                    SegmentType::Wildcard,
                    "*"
                });

                continue;
            }

            if (!part.empty() && part.front() == ':') {
                const auto name = part.substr(1);

                if (name.empty()) {
                    throw HttpRouterError(
                        "Route parameter name cannot be empty"
                    );
                }

                if (!parameterNames.emplace(name).second) {
                    throw HttpRouterError(
                        "Duplicate route parameter name"
                    );
                }

                segments_.push_back({
                    SegmentType::Parameter,
                    std::string(name)
                });

                continue;
            }

            segments_.push_back({
                SegmentType::Static,
                std::string(part)
            });
        }
    }

    std::string pattern_;
    std::vector<Segment> segments_;
};

// ============================================================
// HTTP Router
// ============================================================
//
// Thread safety:
//
// - Route registration uses exclusive locking.
// - Route matching uses shared locking.
// - Handler execution occurs outside the router lock.
//
// Route selection:
//
// 1. Match request path.
// 2. Prefer the most specific matching route.
// 3. Select the matching HTTP method.
// 4. Return 405 if the path exists but method does not.
// 5. Return 404 if no path matches.
//
// HEAD requests can fall back to GET handlers.
//
// The connection layer remains responsible for suppressing
// response body bytes for HEAD requests.
//
// ============================================================

class HttpRouter final {
public:
    HttpRouter() = default;

    HttpRouter(const HttpRouter&) = delete;
    HttpRouter& operator=(const HttpRouter&) = delete;

    HttpRouter(HttpRouter&&) = delete;
    HttpRouter& operator=(HttpRouter&&) = delete;

    // ========================================================
    // Register Route
    // ========================================================

    void add(
        HttpMethod method,
        std::string pattern,
        HttpRouteHandler handler
    ) {
        if (!isKnownHttpMethod(method)) {
            throw HttpRouterError(
                "Cannot register an unknown HTTP method"
            );
        }

        if (!handler) {
            throw HttpRouterError(
                "Cannot register an empty route handler"
            );
        }

        HttpRoutePattern compiled(std::move(pattern));

        std::unique_lock<std::shared_mutex> lock(mutex_);

        for (const auto& route : routes_) {
            if (route.method == method &&
                route.pattern.value() == compiled.value()) {
                throw HttpRouterError(
                    "Duplicate HTTP route registration"
                );
            }
        }

        routes_.push_back({
            method,
            std::move(compiled),
            std::move(handler),
            nextRegistrationOrder_++
        });
    }

    // ========================================================
    // Convenience Registration
    // ========================================================

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

    // ========================================================
    // Dispatch Request
    // ========================================================

    [[nodiscard]]
    HttpResponse dispatch(
        const HttpRequest& request
    ) const {
        HttpRouteHandler selectedHandler;
        HttpRouteContext selectedContext;

        bool pathMatched = false;
        bool methodMatched = false;

        std::size_t bestSpecificity = 0;
        std::size_t bestOrder = 0;

        bool bestExactMethod = false;

        {
            std::shared_lock<std::shared_mutex> lock(mutex_);

            for (const auto& route : routes_) {
                HttpRouteParams params;

                if (!route.pattern.matches(
                        request.path(),
                        params
                    )) {
                    continue;
                }

                pathMatched = true;

                const bool exactMethod =
                    route.method == request.method();

                const bool headFallback =
                    request.method() == HttpMethod::Head &&
                    route.method == HttpMethod::Get;

                if (!exactMethod && !headFallback) {
                    continue;
                }

                const auto specificity =
                    route.pattern.specificity();

                const bool betterMatch =
                    !methodMatched ||
                    (
                        exactMethod && !bestExactMethod
                    ) ||
                    (
                        exactMethod == bestExactMethod &&
                        (
                            specificity > bestSpecificity ||
                            (
                                specificity == bestSpecificity &&
                                route.order < bestOrder
                            )
                        )
                    );

                if (!betterMatch) {
                    continue;
                }

                methodMatched = true;
                bestSpecificity = specificity;
                bestOrder = route.order;

                bestExactMethod = exactMethod;

                selectedHandler = route.handler;
                selectedContext.params = std::move(params);
            }
        }

        if (methodMatched) {
            return selectedHandler(
                request,
                selectedContext
            );
        }

        if (pathMatched) {
            return HttpResponse(
                HttpStatus::MethodNotAllowed
            );
        }

        return HttpResponse(HttpStatus::NotFound);
    }

    // ========================================================
    // Inspection
    // ========================================================

    [[nodiscard]]
    std::size_t size() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return routes_.size();
    }

    [[nodiscard]]
    bool empty() const {
        return size() == 0;
    }

    // ========================================================
    // Clear
    // ========================================================

    void clear() {
        std::unique_lock<std::shared_mutex> lock(mutex_);

        routes_.clear();
        nextRegistrationOrder_ = 0;
    }

private:
    struct Route final {
        HttpMethod method;
        HttpRoutePattern pattern;
        HttpRouteHandler handler;
        std::size_t order;
    };

    mutable std::shared_mutex mutex_;

    std::vector<Route> routes_;

    std::size_t nextRegistrationOrder_ = 0;
};

} // namespace lamp::http
