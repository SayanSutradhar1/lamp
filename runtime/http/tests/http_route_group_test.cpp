
#include "http/http_route_group.hpp"

#include <cstddef>
#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace lamp::http;

void Check(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

template <typename Exception, typename Function>
void CheckThrows(
    Function&& function,
    std::string_view message
) {
    try {
        std::forward<Function>(function)();
    } catch (const Exception&) {
        return;
    }

    throw std::runtime_error(std::string(message));
}

HttpRequest Request(
    HttpMethod method,
    std::string path
) {
    return HttpRequest(method, std::move(path));
}

HttpResponse Response(
    std::string_view text,
    HttpStatus status = HttpStatus::Ok
) {
    HttpResponse response(status);
    response.setBody(text);
    return response;
}

std::string BodyText(const HttpResponse& response) {
    std::string result;
    result.reserve(response.body().size());

    for (const auto byte : response.body()) {
        result.push_back(
            static_cast<char>(
                std::to_integer<unsigned char>(byte)
            )
        );
    }

    return result;
}

HttpRouteHandler Handler(std::string body) {
    return [body = std::move(body)](
        const HttpRequest&,
        const HttpRouteContext&
    ) {
        return Response(body);
    };
}

HttpMiddleware HeaderMiddleware(
    std::string name,
    std::string value
) {
    return [
        name = std::move(name),
        value = std::move(value)
    ](
        const HttpRequest& request,
        const HttpRouteContext& context,
        const HttpMiddlewareNext& next
    ) {
        auto response = next(request, context);
        response.setHeader(name, value);
        return response;
    };
}

void TestRootGroup() {
    HttpRouter router;
    HttpRouteGroup group(router);

    Check(group.prefix() == "/", "Incorrect root prefix");
    Check(group.middlewareCount() == 0, "Unexpected middleware");

    group.get("/health", Handler("healthy"));

    const auto response = router.dispatch(
        Request(HttpMethod::Get, "/health")
    );

    Check(response.statusCode() == 200, "Root route failed");
    Check(BodyText(response) == "healthy", "Incorrect root response");
}

void TestPrefixRegistration() {
    HttpRouter router;
    HttpRouteGroup group(router, "/api");

    group.get("/users", Handler("users"));
    group.post("/users", Handler("created"));

    Check(router.size() == 2, "Incorrect route count");

    Check(
        BodyText(router.dispatch(
            Request(HttpMethod::Get, "/api/users")
        )) == "users",
        "GET route prefix failed"
    );

    Check(
        BodyText(router.dispatch(
            Request(HttpMethod::Post, "/api/users")
        )) == "created",
        "POST route prefix failed"
    );

    Check(
        router.dispatch(
            Request(HttpMethod::Get, "/users")
        ).statusCode() == 404,
        "Unprefixed route should not match"
    );
}

void TestNestedGroups() {
    HttpRouter router;
    HttpRouteGroup api(router, "/api");

    auto v1 = api.group("/v1");
    auto users = v1.group("/users");

    Check(
        users.prefix() == "/api/v1/users",
        "Nested prefix is incorrect"
    );

    users.get("/:id", [](
        const HttpRequest&,
        const HttpRouteContext& context
    ) {
        return Response(context.param("id"));
    });

    const auto response = router.dispatch(
        Request(HttpMethod::Get, "/api/v1/users/42")
    );

    Check(response.statusCode() == 200, "Nested route failed");
    Check(BodyText(response) == "42", "Nested parameter failed");
}

void TestRootPatternUnderPrefix() {
    HttpRouter router;
    HttpRouteGroup api(router, "/api");

    api.get("/", Handler("api-root"));

    Check(
        BodyText(router.dispatch(
            Request(HttpMethod::Get, "/api")
        )) == "api-root",
        "Group root pattern did not map to prefix"
    );

    Check(
        router.dispatch(
            Request(HttpMethod::Get, "/api/")
        ).statusCode() == 404,
        "Trailing slash unexpectedly matched"
    );
}

void TestTrailingSlashPrefix() {
    HttpRouter router;
    HttpRouteGroup api(router, "/api/");

    Check(api.prefix() == "/api", "Trailing slash not normalized");

    api.get("/health", Handler("ok"));

    Check(
        router.dispatch(
            Request(HttpMethod::Get, "/api/health")
        ).statusCode() == 200,
        "Normalized prefix route failed"
    );
}

void TestMiddlewareInheritance() {
    HttpRouter router;
    HttpRouteGroup api(router, "/api");

    api.use(HeaderMiddleware("X-Parent", "yes"));

    auto v1 = api.group("/v1");

    v1.use(HeaderMiddleware("X-Child", "yes"));

    v1.get("/health", Handler("healthy"));

    const auto response = router.dispatch(
        Request(HttpMethod::Get, "/api/v1/health")
    );

    Check(
        response.header("X-Parent").value_or("") == "yes",
        "Parent middleware missing"
    );

    Check(
        response.header("X-Child").value_or("") == "yes",
        "Child middleware missing"
    );

    Check(v1.middlewareCount() == 2, "Incorrect inherited count");
}

void TestMiddlewareExecutionOrder() {
    HttpRouter router;
    HttpRouteGroup api(router, "/api");

    std::vector<std::string> events;

    api.use([&](
        const HttpRequest& request,
        const HttpRouteContext& context,
        const HttpMiddlewareNext& next
    ) {
        events.emplace_back("parent-before");
        auto response = next(request, context);
        events.emplace_back("parent-after");
        return response;
    });

    auto child = api.group("/child");

    child.use([&](
        const HttpRequest& request,
        const HttpRouteContext& context,
        const HttpMiddlewareNext& next
    ) {
        events.emplace_back("child-before");
        auto response = next(request, context);
        events.emplace_back("child-after");
        return response;
    });

    child.get("/", [&](
        const HttpRequest&,
        const HttpRouteContext&
    ) {
        events.emplace_back("handler");
        return Response("ok");
    });

    (void)router.dispatch(
        Request(HttpMethod::Get, "/api/child")
    );

    const std::vector<std::string> expected = {
        "parent-before",
        "child-before",
        "handler",
        "child-after",
        "parent-after"
    };

    Check(events == expected, "Incorrect middleware order");
}

void TestParentSnapshot() {
    HttpRouter router;
    HttpRouteGroup parent(router, "/api");

    parent.use(HeaderMiddleware("X-Initial", "yes"));

    auto child = parent.group("/v1");

    parent.use(HeaderMiddleware("X-Later", "yes"));

    child.get("/health", Handler("ok"));
    parent.get("/health", Handler("ok"));

    const auto childResponse = router.dispatch(
        Request(HttpMethod::Get, "/api/v1/health")
    );

    const auto parentResponse = router.dispatch(
        Request(HttpMethod::Get, "/api/health")
    );

    Check(
        childResponse.header("X-Initial").has_value(),
        "Inherited middleware missing"
    );

    Check(
        !childResponse.header("X-Later").has_value(),
        "Parent mutation unexpectedly affected child"
    );

    Check(
        parentResponse.header("X-Later").has_value(),
        "Later parent middleware missing"
    );
}

void TestRouteSnapshot() {
    HttpRouter router;
    HttpRouteGroup group(router, "/api");

    group.get("/before", Handler("before"));

    group.use(HeaderMiddleware("X-New", "yes"));

    group.get("/after", Handler("after"));

    const auto before = router.dispatch(
        Request(HttpMethod::Get, "/api/before")
    );

    const auto after = router.dispatch(
        Request(HttpMethod::Get, "/api/after")
    );

    Check(
        !before.header("X-New").has_value(),
        "Existing route was modified"
    );

    Check(
        after.header("X-New").has_value(),
        "New route did not inherit middleware"
    );
}

void TestMiddlewareShortCircuit() {
    HttpRouter router;
    HttpRouteGroup group(router, "/secure");

    int handlerCalls = 0;

    group.use([](
        const HttpRequest&,
        const HttpRouteContext&,
        const HttpMiddlewareNext&
    ) {
        return Response("forbidden", HttpStatus::Forbidden);
    });

    group.get("/data", [&](
        const HttpRequest&,
        const HttpRouteContext&
    ) {
        ++handlerCalls;
        return Response("secret");
    });

    const auto response = router.dispatch(
        Request(HttpMethod::Get, "/secure/data")
    );

    Check(response.statusCode() == 403, "Short-circuit failed");
    Check(handlerCalls == 0, "Protected handler executed");
}

void TestAllHttpMethods() {
    HttpRouter router;
    HttpRouteGroup group(router, "/methods");

    group.get("/get", Handler("GET"));
    group.post("/post", Handler("POST"));
    group.put("/put", Handler("PUT"));
    group.patch("/patch", Handler("PATCH"));
    group.del("/delete", Handler("DELETE"));
    group.head("/head", Handler("HEAD"));
    group.options("/options", Handler("OPTIONS"));

    struct Case {
        HttpMethod method;
        const char* path;
        const char* expected;
    };

    const std::vector<Case> cases = {
        {HttpMethod::Get, "/methods/get", "GET"},
        {HttpMethod::Post, "/methods/post", "POST"},
        {HttpMethod::Put, "/methods/put", "PUT"},
        {HttpMethod::Patch, "/methods/patch", "PATCH"},
        {HttpMethod::Delete, "/methods/delete", "DELETE"},
        {HttpMethod::Head, "/methods/head", "HEAD"},
        {HttpMethod::Options, "/methods/options", "OPTIONS"}
    };

    for (const auto& test : cases) {
        const auto response = router.dispatch(
            Request(test.method, test.path)
        );

        Check(response.statusCode() == 200, "Method route failed");
        Check(
            BodyText(response) == test.expected,
            "Incorrect method handler"
        );
    }
}

void TestWildcardAndParameters() {
    HttpRouter router;
    HttpRouteGroup group(router, "/api");

    group.get("/files/*", Handler("wildcard"));

    group.get("/users/:id", [](
        const HttpRequest&,
        const HttpRouteContext& context
    ) {
        return Response(context.param("id"));
    });

    Check(
        BodyText(router.dispatch(
            Request(HttpMethod::Get, "/api/files/a/b")
        )) == "wildcard",
        "Wildcard route failed"
    );

    Check(
        BodyText(router.dispatch(
            Request(HttpMethod::Get, "/api/users/99")
        )) == "99",
        "Named parameter route failed"
    );
}

void TestDuplicateRegistration() {
    HttpRouter router;
    HttpRouteGroup group(router, "/api");

    group.get("/users", Handler("first"));

    CheckThrows<HttpRouterError>(
        [&] {
            group.get("/users", Handler("second"));
        },
        "Duplicate registration was accepted"
    );

    Check(router.size() == 1, "Duplicate changed router size");
}

void TestInvalidPrefixes() {
    HttpRouter router;

    const std::vector<std::string> invalid = {
        "api",
        "/api//v1",
        "/api/:id",
        "/api/*",
        "/api?version=1",
        "/api#fragment",
        "/api\\v1",
        "/api/hello world"
    };

    for (const auto& prefix : invalid) {
        CheckThrows<HttpRouteGroupError>(
            [&] {
                HttpRouteGroup group(router, prefix);
                (void)group;
            },
            "Invalid prefix was accepted"
        );
    }
}

void TestInvalidHandlersAndMiddleware() {
    HttpRouter router;
    HttpRouteGroup group(router, "/api");

    CheckThrows<HttpRouteGroupError>(
        [&] {
            group.use(HttpMiddleware{});
        },
        "Empty middleware accepted"
    );

    CheckThrows<HttpRouteGroupError>(
        [&] {
            group.get("/test", HttpRouteHandler{});
        },
        "Empty handler accepted"
    );

    CheckThrows<HttpRouteGroupError>(
        [&] {
            group.get("relative", Handler("invalid"));
        },
        "Relative route pattern accepted"
    );

    Check(router.empty(), "Invalid registration changed router");
}

void TestGroupLifetime() {
    HttpRouter router;

    {
        HttpRouteGroup group(router, "/api");
        group.use(HeaderMiddleware("X-Lifetime", "valid"));
        group.get("/health", Handler("alive"));
    }

    const auto response = router.dispatch(
        Request(HttpMethod::Get, "/api/health")
    );

    Check(BodyText(response) == "alive", "Handler lifetime failed");

    Check(
        response.header("X-Lifetime").value_or("") == "valid",
        "Middleware lifetime failed"
    );
}

void TestRouteSpecificity() {
    HttpRouter router;
    HttpRouteGroup group(router, "/api");

    group.get("/users/:id", Handler("parameter"));
    group.get("/users/me", Handler("static"));
    group.get("/users/*", Handler("wildcard"));

    Check(
        BodyText(router.dispatch(
            Request(HttpMethod::Get, "/api/users/me")
        )) == "static",
        "Static route did not win"
    );

    Check(
        BodyText(router.dispatch(
            Request(HttpMethod::Get, "/api/users/42")
        )) == "parameter",
        "Parameter route did not win"
    );

    Check(
        BodyText(router.dispatch(
            Request(HttpMethod::Get, "/api/users/42/details")
        )) == "wildcard",
        "Wildcard route failed"
    );
}

void TestHeadFallbackAndMethodNotAllowed() {
    HttpRouter router;
    HttpRouteGroup group(router, "/api");

    group.get("/health", Handler("healthy"));

    const auto head = router.dispatch(
        Request(HttpMethod::Head, "/api/health")
    );

    Check(head.statusCode() == 200, "HEAD fallback failed");

    // The connection layer suppresses HEAD body bytes.
    Check(
        BodyText(head) == "healthy",
        "HEAD fallback selected wrong handler"
    );

    const auto wrongMethod = router.dispatch(
        Request(HttpMethod::Post, "/api/health")
    );

    Check(
        wrongMethod.statusCode() == 405,
        "Expected method-not-allowed response"
    );
}

struct TestCase {
    const char* name;
    void (*run)();
};

} // namespace

int main() {
    const std::vector<TestCase> tests = {
        {"RootGroup", TestRootGroup},
        {"PrefixRegistration", TestPrefixRegistration},
        {"NestedGroups", TestNestedGroups},
        {"RootPatternUnderPrefix", TestRootPatternUnderPrefix},
        {"TrailingSlashPrefix", TestTrailingSlashPrefix},
        {"MiddlewareInheritance", TestMiddlewareInheritance},
        {"MiddlewareExecutionOrder", TestMiddlewareExecutionOrder},
        {"ParentSnapshot", TestParentSnapshot},
        {"RouteSnapshot", TestRouteSnapshot},
        {"MiddlewareShortCircuit", TestMiddlewareShortCircuit},
        {"AllHttpMethods", TestAllHttpMethods},
        {"WildcardAndParameters", TestWildcardAndParameters},
        {"DuplicateRegistration", TestDuplicateRegistration},
        {"InvalidPrefixes", TestInvalidPrefixes},
        {"InvalidHandlersAndMiddleware", TestInvalidHandlersAndMiddleware},
        {"GroupLifetime", TestGroupLifetime},
        {"RouteSpecificity", TestRouteSpecificity},
        {"HeadFallbackAndMethodNotAllowed",
         TestHeadFallbackAndMethodNotAllowed}
    };

    std::size_t passed = 0;

    for (const auto& test : tests) {
        try {
            test.run();
            ++passed;
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception& exception) {
            std::cerr
                << "[FAIL] " << test.name
                << ": " << exception.what() << '\n';
            return 1;
        } catch (...) {
            std::cerr
                << "[FAIL] " << test.name
                << ": unknown exception\n";
            return 1;
        }
    }

    std::cout
        << "Lamp HTTP route groups: "
        << passed << "/" << tests.size()
        << " tests passed\n";

    return passed == tests.size() ? 0 : 1;
}
