
#include "http/http_router.hpp"

#include <atomic>
#include <cstddef>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using lamp::http::HttpMethod;
using lamp::http::HttpRequest;
using lamp::http::HttpResponse;
using lamp::http::HttpRouteContext;
using lamp::http::HttpRoutePattern;
using lamp::http::HttpRouter;
using lamp::http::HttpRouterError;
using lamp::http::HttpStatus;

// ============================================================
// Minimal Test Framework
// ============================================================

class TestFailure final : public std::runtime_error {
public:
    explicit TestFailure(std::string message)
        : std::runtime_error(std::move(message)) {}
};

void require(
    bool condition,
    std::string_view message
) {
    if (!condition) {
        throw TestFailure(std::string(message));
    }
}

template <typename Left, typename Right>
void requireEqual(
    const Left& actual,
    const Right& expected,
    std::string_view message
) {
    if (!(actual == expected)) {
        throw TestFailure(std::string(message));
    }
}

using TestFunction = void (*)();

struct TestCase final {
    const char* name;
    TestFunction function;
};

std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

struct Registration final {
    Registration(
        const char* name,
        TestFunction function
    ) {
        registry().push_back({name, function});
    }
};

#define LAMP_TEST(name)                         \
    void name();                                \
    const Registration register_##name(         \
        #name, &name                            \
    );                                          \
    void name()

// ============================================================
// Test Helpers
// ============================================================

[[nodiscard]]
HttpResponse textResponse(
    std::string_view body,
    HttpStatus status = HttpStatus::Ok
) {
    HttpResponse response(status);
    response.setBody(body);
    return response;
}

[[nodiscard]]
std::string bodyAsString(
    const HttpResponse& response
) {
    std::string result;
    result.reserve(response.bodySize());

    for (const auto byte : response.body()) {
        result.push_back(
            static_cast<char>(
                std::to_integer<unsigned char>(byte)
            )
        );
    }

    return result;
}

[[nodiscard]]
HttpRequest makeRequest(
    HttpMethod method,
    std::string target
) {
    return HttpRequest(
        method,
        std::move(target)
    );
}

[[nodiscard]]
HttpResponse dispatch(
    HttpRouter& router,
    HttpMethod method,
    std::string target
) {
    return router.dispatch(
        makeRequest(method, std::move(target))
    );
}

void expectRouterError(
    const std::function<void()>& operation
) {
    bool threw = false;

    try {
        operation();
    } catch (const HttpRouterError&) {
        threw = true;
    }

    require(
        threw,
        "Expected HttpRouterError"
    );
}

// ============================================================
// Test 1: Empty Router
// ============================================================

LAMP_TEST(empty_router_has_no_routes) {
    HttpRouter router;

    require(
        router.empty(),
        "New router should be empty"
    );

    requireEqual(
        router.size(),
        std::size_t{0},
        "New router size must be zero"
    );
}

// ============================================================
// Test 2: Missing Route
// ============================================================

LAMP_TEST(empty_router_returns_404) {
    HttpRouter router;

    const auto response = dispatch(
        router,
        HttpMethod::Get,
        "/missing"
    );

    requireEqual(
        response.statusCode(),
        std::uint16_t{404},
        "Missing route must return HTTP 404"
    );
}

// ============================================================
// Test 3: Static GET Route
// ============================================================

LAMP_TEST(static_get_route_matches) {
    HttpRouter router;

    router.get(
        "/hello",
        [](const auto&, const auto&) {
            return textResponse("Hello Lamp");
        }
    );

    const auto response = dispatch(
        router,
        HttpMethod::Get,
        "/hello"
    );

    requireEqual(
        response.statusCode(),
        std::uint16_t{200},
        "Static GET route should return HTTP 200"
    );

    requireEqual(
        bodyAsString(response),
        std::string("Hello Lamp"),
        "Static route response mismatch"
    );
}

// ============================================================
// Test 4: Root Route
// ============================================================

LAMP_TEST(root_route_matches) {
    HttpRouter router;

    router.get(
        "/",
        [](const auto&, const auto&) {
            return textResponse("root");
        }
    );

    const auto response = dispatch(
        router,
        HttpMethod::Get,
        "/"
    );

    requireEqual(
        bodyAsString(response),
        std::string("root"),
        "Root route mismatch"
    );
}

// ============================================================
// Test 5: Static Route Mismatch
// ============================================================

LAMP_TEST(static_route_does_not_match_other_path) {
    HttpRouter router;

    router.get(
        "/users",
        [](const auto&, const auto&) {
            return textResponse("users");
        }
    );

    const auto response = dispatch(
        router,
        HttpMethod::Get,
        "/posts"
    );

    requireEqual(
        response.statusCode(),
        std::uint16_t{404},
        "Unrelated path must not match"
    );
}

// ============================================================
// Test 6: GET Convenience Method
// ============================================================

LAMP_TEST(get_registration_works) {
    HttpRouter router;

    router.get(
        "/get",
        [](const auto&, const auto&) {
            return textResponse("GET");
        }
    );

    requireEqual(
        bodyAsString(
            dispatch(router, HttpMethod::Get, "/get")
        ),
        std::string("GET"),
        "GET registration failed"
    );
}

// ============================================================
// Test 7: POST Convenience Method
// ============================================================

LAMP_TEST(post_registration_works) {
    HttpRouter router;

    router.post(
        "/post",
        [](const auto&, const auto&) {
            return textResponse("POST");
        }
    );

    requireEqual(
        bodyAsString(
            dispatch(router, HttpMethod::Post, "/post")
        ),
        std::string("POST"),
        "POST registration failed"
    );
}

// ============================================================
// Test 8: PUT Convenience Method
// ============================================================

LAMP_TEST(put_registration_works) {
    HttpRouter router;

    router.put(
        "/put",
        [](const auto&, const auto&) {
            return textResponse("PUT");
        }
    );

    requireEqual(
        bodyAsString(
            dispatch(router, HttpMethod::Put, "/put")
        ),
        std::string("PUT"),
        "PUT registration failed"
    );
}

// ============================================================
// Test 9: PATCH Convenience Method
// ============================================================

LAMP_TEST(patch_registration_works) {
    HttpRouter router;

    router.patch(
        "/patch",
        [](const auto&, const auto&) {
            return textResponse("PATCH");
        }
    );

    requireEqual(
        bodyAsString(
            dispatch(router, HttpMethod::Patch, "/patch")
        ),
        std::string("PATCH"),
        "PATCH registration failed"
    );
}

// ============================================================
// Test 10: DELETE Convenience Method
// ============================================================

LAMP_TEST(delete_registration_works) {
    HttpRouter router;

    router.del(
        "/delete",
        [](const auto&, const auto&) {
            return textResponse("DELETE");
        }
    );

    requireEqual(
        bodyAsString(
            dispatch(router, HttpMethod::Delete, "/delete")
        ),
        std::string("DELETE"),
        "DELETE registration failed"
    );
}

// ============================================================
// Test 11: OPTIONS Convenience Method
// ============================================================

LAMP_TEST(options_registration_works) {
    HttpRouter router;

    router.options(
        "/options",
        [](const auto&, const auto&) {
            return textResponse("OPTIONS");
        }
    );

    requireEqual(
        bodyAsString(
            dispatch(
                router,
                HttpMethod::Options,
                "/options"
            )
        ),
        std::string("OPTIONS"),
        "OPTIONS registration failed"
    );
}

// ============================================================
// Test 12: Generic Registration
// ============================================================

LAMP_TEST(generic_method_registration_works) {
    HttpRouter router;

    router.add(
        HttpMethod::Trace,
        "/trace",
        [](const auto&, const auto&) {
            return textResponse("TRACE");
        }
    );

    const auto response = dispatch(
        router,
        HttpMethod::Trace,
        "/trace"
    );

    requireEqual(
        bodyAsString(response),
        std::string("TRACE"),
        "Generic registration failed"
    );
}

// ============================================================
// Test 13: Method Isolation
// ============================================================

LAMP_TEST(routes_are_isolated_by_method) {
    HttpRouter router;

    router.get(
        "/resource",
        [](const auto&, const auto&) {
            return textResponse("GET");
        }
    );

    router.post(
        "/resource",
        [](const auto&, const auto&) {
            return textResponse("POST");
        }
    );

    requireEqual(
        bodyAsString(
            dispatch(
                router,
                HttpMethod::Get,
                "/resource"
            )
        ),
        std::string("GET"),
        "GET route returned wrong handler"
    );

    requireEqual(
        bodyAsString(
            dispatch(
                router,
                HttpMethod::Post,
                "/resource"
            )
        ),
        std::string("POST"),
        "POST route returned wrong handler"
    );
}

// ============================================================
// Test 14: Method Not Allowed
// ============================================================

LAMP_TEST(wrong_method_returns_405) {
    HttpRouter router;

    router.get(
        "/resource",
        [](const auto&, const auto&) {
            return textResponse("resource");
        }
    );

    const auto response = dispatch(
        router,
        HttpMethod::Post,
        "/resource"
    );

    requireEqual(
        response.statusCode(),
        std::uint16_t{405},
        "Wrong method should return HTTP 405"
    );
}

// ============================================================
// Test 15: Dynamic Parameter
// ============================================================

LAMP_TEST(single_parameter_matches) {
    HttpRouter router;

    router.get(
        "/users/:id",
        [](const auto&, const auto& context) {
            return textResponse(context.param("id"));
        }
    );

    const auto response = dispatch(
        router,
        HttpMethod::Get,
        "/users/42"
    );

    requireEqual(
        bodyAsString(response),
        std::string("42"),
        "Dynamic parameter mismatch"
    );
}

// ============================================================
// Test 16: Multiple Parameters
// ============================================================

LAMP_TEST(multiple_parameters_match) {
    HttpRouter router;

    router.get(
        "/users/:userId/posts/:postId",
        [](const auto&, const auto& context) {
            return textResponse(
                std::string(context.param("userId")) +
                ":" +
                std::string(context.param("postId"))
            );
        }
    );

    const auto response = dispatch(
        router,
        HttpMethod::Get,
        "/users/12/posts/99"
    );

    requireEqual(
        bodyAsString(response),
        std::string("12:99"),
        "Multiple parameters mismatch"
    );
}

// ============================================================
// Test 17: Missing Parameter
// ============================================================

LAMP_TEST(missing_parameter_returns_empty_view) {
    HttpRouteContext context;

    require(
        context.param("missing").empty(),
        "Missing parameter should be empty"
    );

    require(
        !context.hasParam("missing"),
        "Missing parameter must not exist"
    );
}

// ============================================================
// Test 18: Parameter Presence
// ============================================================

LAMP_TEST(parameter_presence_is_reported) {
    HttpRouteContext context;

    context.params.emplace("id", "123");

    require(
        context.hasParam("id"),
        "Existing parameter should be found"
    );

    requireEqual(
        context.param("id"),
        std::string_view("123"),
        "Parameter value mismatch"
    );
}

// ============================================================
// Test 19: Parameter Does Not Match Empty Segment
// ============================================================

LAMP_TEST(parameter_requires_path_segment) {
    HttpRouter router;

    router.get(
        "/users/:id",
        [](const auto&, const auto&) {
            return textResponse("matched");
        }
    );

    const auto response = dispatch(
        router,
        HttpMethod::Get,
        "/users"
    );

    requireEqual(
        response.statusCode(),
        std::uint16_t{404},
        "Missing parameter segment must not match"
    );
}

// ============================================================
// Test 20: Wildcard Route
// ============================================================

LAMP_TEST(wildcard_route_matches_nested_path) {
    HttpRouter router;

    router.get(
        "/assets/*",
        [](const auto&, const auto&) {
            return textResponse("asset");
        }
    );

    const auto response = dispatch(
        router,
        HttpMethod::Get,
        "/assets/css/app.css"
    );

    requireEqual(
        bodyAsString(response),
        std::string("asset"),
        "Wildcard route did not match nested path"
    );
}

// ============================================================
// Test 21: Wildcard Does Not Match Unrelated Path
// ============================================================

LAMP_TEST(wildcard_respects_static_prefix) {
    HttpRouter router;

    router.get(
        "/assets/*",
        [](const auto&, const auto&) {
            return textResponse("asset");
        }
    );

    const auto response = dispatch(
        router,
        HttpMethod::Get,
        "/api/users"
    );

    requireEqual(
        response.statusCode(),
        std::uint16_t{404},
        "Wildcard matched unrelated path"
    );
}

// ============================================================
// Test 22: Static Route Precedence
// ============================================================

LAMP_TEST(static_route_beats_parameter_route) {
    HttpRouter router;

    router.get(
        "/users/:id",
        [](const auto&, const auto&) {
            return textResponse("parameter");
        }
    );

    router.get(
        "/users/me",
        [](const auto&, const auto&) {
            return textResponse("static");
        }
    );

    const auto response = dispatch(
        router,
        HttpMethod::Get,
        "/users/me"
    );

    requireEqual(
        bodyAsString(response),
        std::string("static"),
        "Static route must take precedence"
    );
}

// ============================================================
// Test 23: Parameter Precedence
// ============================================================

LAMP_TEST(parameter_route_beats_wildcard_route) {
    HttpRouter router;

    router.get(
        "/files/*",
        [](const auto&, const auto&) {
            return textResponse("wildcard");
        }
    );

    router.get(
        "/files/:name",
        [](const auto&, const auto&) {
            return textResponse("parameter");
        }
    );

    const auto response = dispatch(
        router,
        HttpMethod::Get,
        "/files/readme"
    );

    requireEqual(
        bodyAsString(response),
        std::string("parameter"),
        "Parameter route must beat wildcard"
    );
}

// ============================================================
// Test 24: Route Registration Order
// ============================================================

LAMP_TEST(equal_specificity_preserves_registration_order) {
    HttpRouter router;

    router.get(
        "/items/:id",
        [](const auto&, const auto&) {
            return textResponse("first");
        }
    );

    router.get(
        "/items/:name",
        [](const auto&, const auto&) {
            return textResponse("second");
        }
    );

    const auto response = dispatch(
        router,
        HttpMethod::Get,
        "/items/123"
    );

    requireEqual(
        bodyAsString(response),
        std::string("first"),
        "Equal specificity should preserve first route"
    );
}

// ============================================================
// Test 25: Query String Does Not Affect Matching
// ============================================================

LAMP_TEST(query_string_is_excluded_from_route_matching) {
    HttpRouter router;

    router.get(
        "/search",
        [](const auto& request, const auto&) {
            return textResponse(request.query());
        }
    );

    const auto response = dispatch(
        router,
        HttpMethod::Get,
        "/search?q=lamp"
    );

    requireEqual(
        bodyAsString(response),
        std::string("q=lamp"),
        "Query string should be preserved separately"
    );
}

// ============================================================
// Test 26: HEAD Fallback
// ============================================================

LAMP_TEST(head_falls_back_to_get) {
    HttpRouter router;

    router.get(
        "/resource",
        [](const auto&, const auto&) {
            return textResponse("GET");
        }
    );

    const auto response = dispatch(
        router,
        HttpMethod::Head,
        "/resource"
    );

    requireEqual(
        response.statusCode(),
        std::uint16_t{200},
        "HEAD should fall back to GET"
    );
}

// ============================================================
// Test 27: Explicit HEAD Handler
// ============================================================

LAMP_TEST(explicit_head_handler_is_used) {
    HttpRouter router;

    router.get(
        "/resource",
        [](const auto&, const auto&) {
            return textResponse("GET");
        }
    );

    router.head(
        "/resource",
        [](const auto&, const auto&) {
            return textResponse("HEAD");
        }
    );

    const auto response = dispatch(
        router,
        HttpMethod::Head,
        "/resource"
    );

    requireEqual(
        bodyAsString(response),
        std::string("HEAD"),
        "Explicit HEAD handler must be used"
    );
}

// ============================================================
// Test 28: HEAD Precedence Across Patterns
// ============================================================

LAMP_TEST(explicit_head_beats_get_fallback) {
    HttpRouter router;

    router.get(
        "/items/special",
        [](const auto&, const auto&) {
            return textResponse("GET");
        }
    );

    router.head(
        "/items/:id",
        [](const auto&, const auto&) {
            return textResponse("HEAD");
        }
    );

    const auto response = dispatch(
        router,
        HttpMethod::Head,
        "/items/special"
    );

    requireEqual(
        bodyAsString(response),
        std::string("HEAD"),
        "Explicit HEAD should beat GET fallback"
    );
}

// ============================================================
// Test 29: Custom Status
// ============================================================

LAMP_TEST(handler_can_return_custom_status) {
    HttpRouter router;

    router.post(
        "/created",
        [](const auto&, const auto&) {
            return textResponse(
                "created",
                HttpStatus::Created
            );
        }
    );

    const auto response = dispatch(
        router,
        HttpMethod::Post,
        "/created"
    );

    requireEqual(
        response.statusCode(),
        std::uint16_t{201},
        "Custom handler status mismatch"
    );
}

// ============================================================
// Test 30: Handler Receives Request
// ============================================================

LAMP_TEST(handler_receives_request_metadata) {
    HttpRouter router;

    router.get(
        "/metadata",
        [](const auto& request, const auto&) {
            return textResponse(
                std::string(request.methodName()) +
                " " +
                request.path()
            );
        }
    );

    const auto response = dispatch(
        router,
        HttpMethod::Get,
        "/metadata"
    );

    requireEqual(
        bodyAsString(response),
        std::string("GET /metadata"),
        "Handler request metadata mismatch"
    );
}

// ============================================================
// Test 31: Route Count
// ============================================================

LAMP_TEST(route_count_is_updated) {
    HttpRouter router;

    router.get(
        "/one",
        [](const auto&, const auto&) {
            return textResponse("one");
        }
    );

    router.post(
        "/two",
        [](const auto&, const auto&) {
            return textResponse("two");
        }
    );

    requireEqual(
        router.size(),
        std::size_t{2},
        "Route count mismatch"
    );
}

// ============================================================
// Test 32: Clear Routes
// ============================================================

LAMP_TEST(clear_removes_all_routes) {
    HttpRouter router;

    router.get(
        "/hello",
        [](const auto&, const auto&) {
            return textResponse("hello");
        }
    );

    router.clear();

    require(
        router.empty(),
        "Router should be empty after clear"
    );

    const auto response = dispatch(
        router,
        HttpMethod::Get,
        "/hello"
    );

    requireEqual(
        response.statusCode(),
        std::uint16_t{404},
        "Cleared route should return HTTP 404"
    );
}

// ============================================================
// Test 33: Invalid Empty Pattern
// ============================================================

LAMP_TEST(rejects_empty_route_pattern) {
    HttpRouter router;

    expectRouterError(
        [&router]() {
            router.get(
                "",
                [](const auto&, const auto&) {
                    return textResponse("invalid");
                }
            );
        }
    );
}

// ============================================================
// Test 34: Invalid Relative Pattern
// ============================================================

LAMP_TEST(rejects_relative_route_pattern) {
    HttpRouter router;

    expectRouterError(
        [&router]() {
            router.get(
                "users/:id",
                [](const auto&, const auto&) {
                    return textResponse("invalid");
                }
            );
        }
    );
}

// ============================================================
// Test 35: Empty Handler
// ============================================================

LAMP_TEST(rejects_empty_route_handler) {
    HttpRouter router;

    expectRouterError(
        [&router]() {
            router.get(
                "/empty",
                {}
            );
        }
    );
}

// ============================================================
// Test 36: Invalid Parameter
// ============================================================

LAMP_TEST(rejects_empty_parameter_name) {
    HttpRouter router;

    expectRouterError(
        [&router]() {
            router.get(
                "/users/:",
                [](const auto&, const auto&) {
                    return textResponse("invalid");
                }
            );
        }
    );
}

// ============================================================
// Test 37: Pattern Matching
// ============================================================

LAMP_TEST(route_pattern_matches_path) {
    HttpRoutePattern pattern(
        "/users/:id"
    );

    lamp::http::HttpRouteParams params;

    require(
        pattern.matches("/users/42", params),
        "Pattern should match valid path"
    );

    requireEqual(
        params.at("id"),
        std::string("42"),
        "Pattern parameter mismatch"
    );
}

// ============================================================
// Test 38: Pattern Mismatch
// ============================================================

LAMP_TEST(route_pattern_rejects_wrong_path) {
    HttpRoutePattern pattern(
        "/users/:id"
    );

    lamp::http::HttpRouteParams params;

    require(
        !pattern.matches("/posts/42", params),
        "Pattern must reject unrelated path"
    );
}

// ============================================================
// Test 39: Specificity Ordering
// ============================================================

LAMP_TEST(route_specificity_is_ordered) {
    HttpRoutePattern staticPattern(
        "/users/me"
    );

    HttpRoutePattern parameterPattern(
        "/users/:id"
    );

    HttpRoutePattern wildcardPattern(
        "/users/*"
    );

    require(
        staticPattern.specificity() >
            parameterPattern.specificity(),
        "Static route should be more specific"
    );

    require(
        parameterPattern.specificity() >
            wildcardPattern.specificity(),
        "Parameter route should be more specific"
    );
}

// ============================================================
// Test 40: Concurrent Dispatch
// ============================================================

LAMP_TEST(concurrent_dispatch_is_supported) {
    HttpRouter router;

    router.get(
        "/concurrent/:id",
        [](const auto&, const auto& context) {
            return textResponse(context.param("id"));
        }
    );

    constexpr int threadCount = 8;
    constexpr int iterations = 100;

    std::atomic<int> successes{0};

    std::vector<std::thread> workers;
    workers.reserve(threadCount);

    for (int threadIndex = 0;
         threadIndex < threadCount;
         ++threadIndex) {
        workers.emplace_back(
            [&router, &successes, threadIndex]() {
                for (int i = 0; i < iterations; ++i) {
                    try {
                        const auto id =
                            std::to_string(threadIndex) +
                            "-" +
                            std::to_string(i);

                        const auto response = dispatch(
                            router,
                            HttpMethod::Get,
                            "/concurrent/" + id
                        );

                        if (bodyAsString(response) == id) {
                            successes.fetch_add(
                                1,
                                std::memory_order_relaxed
                            );
                        }
                    } catch (...) {
                        // Failed operations do not count.
                    }
                }
            }
        );
    }

    for (auto& worker : workers) {
        worker.join();
    }

    requireEqual(
        successes.load(),
        threadCount * iterations,
        "Concurrent dispatch produced incorrect results"
    );
}

// ============================================================
// Test 41: Concurrent Registration
// ============================================================

LAMP_TEST(concurrent_route_registration_is_supported) {
    HttpRouter router;

    constexpr int threadCount = 8;
    constexpr int routesPerThread = 25;

    std::vector<std::thread> workers;
    workers.reserve(threadCount);

    for (int threadIndex = 0;
         threadIndex < threadCount;
         ++threadIndex) {
        workers.emplace_back(
            [&router, threadIndex]() {
                for (int i = 0;
                     i < routesPerThread;
                     ++i) {
                    const auto path =
                        "/thread/" +
                        std::to_string(threadIndex) +
                        "/" +
                        std::to_string(i);

                    router.get(
                        path,
                        [](const auto&, const auto&) {
                            return textResponse("ok");
                        }
                    );
                }
            }
        );
    }

    for (auto& worker : workers) {
        worker.join();
    }

    requireEqual(
        router.size(),
        static_cast<std::size_t>(
            threadCount * routesPerThread
        ),
        "Concurrent registration lost routes"
    );
}

// ============================================================
// Test 42: Registration During Dispatch
// ============================================================

LAMP_TEST(registration_and_dispatch_can_overlap) {
    HttpRouter router;

    router.get(
        "/stable",
        [](const auto&, const auto&) {
            return textResponse("stable");
        }
    );

    std::atomic<bool> failed{false};

    std::thread reader(
        [&router, &failed]() {
            for (int i = 0; i < 500; ++i) {
                try {
                    const auto response = dispatch(
                        router,
                        HttpMethod::Get,
                        "/stable"
                    );

                    if (bodyAsString(response) != "stable") {
                        failed.store(true);
                    }
                } catch (...) {
                    failed.store(true);
                }
            }
        }
    );

    std::thread writer(
        [&router, &failed]() {
            for (int i = 0; i < 100; ++i) {
                try {
                    router.get(
                        "/dynamic/" + std::to_string(i),
                        [](const auto&, const auto&) {
                            return textResponse("dynamic");
                        }
                    );
                } catch (...) {
                    failed.store(true);
                }
            }
        }
    );

    reader.join();
    writer.join();

    require(
        !failed.load(),
        "Concurrent registration and dispatch failed"
    );
}

// ============================================================
// Test 43: Handler Exception Propagation
// ============================================================

LAMP_TEST(handler_exception_propagates_from_router) {
    HttpRouter router;

    router.get(
        "/throw",
        [](const auto&, const auto&) -> HttpResponse {
            throw std::runtime_error(
                "Intentional route failure"
            );
        }
    );

    bool threw = false;

    try {
        const auto response = dispatch(
            router,
            HttpMethod::Get,
            "/throw"
        );

        (void)response;
    } catch (const std::runtime_error&) {
        threw = true;
    }

    require(
        threw,
        "Router should propagate handler exceptions"
    );
}

// ============================================================
// Test 44: Independent Parameter Contexts
// ============================================================

LAMP_TEST(route_parameters_do_not_leak_between_requests) {
    HttpRouter router;

    router.get(
        "/users/:id",
        [](const auto&, const auto& context) {
            return textResponse(context.param("id"));
        }
    );

    const auto first = dispatch(
        router,
        HttpMethod::Get,
        "/users/one"
    );

    const auto second = dispatch(
        router,
        HttpMethod::Get,
        "/users/two"
    );

    requireEqual(
        bodyAsString(first),
        std::string("one"),
        "First parameter mismatch"
    );

    requireEqual(
        bodyAsString(second),
        std::string("two"),
        "Second parameter mismatch"
    );
}

// ============================================================
// Test 45: Clear and Reuse
// ============================================================

LAMP_TEST(router_can_be_reused_after_clear) {
    HttpRouter router;

    router.get(
        "/old",
        [](const auto&, const auto&) {
            return textResponse("old");
        }
    );

    router.clear();

    router.get(
        "/new",
        [](const auto&, const auto&) {
            return textResponse("new");
        }
    );

    const auto oldResponse = dispatch(
        router,
        HttpMethod::Get,
        "/old"
    );

    const auto newResponse = dispatch(
        router,
        HttpMethod::Get,
        "/new"
    );

    requireEqual(
        oldResponse.statusCode(),
        std::uint16_t{404},
        "Old route should be removed"
    );

    requireEqual(
        bodyAsString(newResponse),
        std::string("new"),
        "New route should work after clear"
    );
}

// ============================================================
// Test Runner
// ============================================================

} // namespace

int main() {
    std::size_t passed = 0;
    std::size_t failed = 0;

    for (const auto& test : registry()) {
        try {
            test.function();

            ++passed;

            std::cout
                << "[PASS] "
                << test.name
                << '\n';

        } catch (const std::exception& error) {
            ++failed;

            std::cerr
                << "[FAIL] "
                << test.name
                << ": "
                << error.what()
                << '\n';

        } catch (...) {
            ++failed;

            std::cerr
                << "[FAIL] "
                << test.name
                << ": unknown exception\n";
        }
    }

    std::cout
        << "\n========================================\n"
        << "Lamp HTTP Router Tests\n"
        << "========================================\n"
        << "Passed: " << passed << '\n'
        << "Failed: " << failed << '\n'
        << "Total : " << passed + failed << '\n'
        << "========================================\n";

    return failed == 0 ? 0 : 1;
}
