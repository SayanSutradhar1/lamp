
#include "http/http_middleware.hpp"

#include <atomic>
#include <cstddef>
#include <exception>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using lamp::http::HttpMethod;
using lamp::http::HttpMiddleware;
using lamp::http::HttpMiddlewareChain;
using lamp::http::HttpMiddlewareError;
using lamp::http::HttpMiddlewareNext;
using lamp::http::HttpRequest;
using lamp::http::HttpResponse;
using lamp::http::HttpRouteContext;
using lamp::http::HttpRouteHandler;
using lamp::http::HttpRouter;
using lamp::http::HttpStatus;

void Check(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

template <typename Exception, typename Function>
void CheckThrows(Function&& function, std::string_view message) {
    try {
        std::forward<Function>(function)();
    } catch (const Exception&) {
        return;
    }

    throw std::runtime_error(std::string(message));
}

HttpRequest MakeRequest(
    HttpMethod method = HttpMethod::Get,
    std::string target = "/test"
) {
    return HttpRequest(method, std::move(target));
}

HttpResponse MakeResponse(
    std::string_view body = "OK",
    HttpStatus status = HttpStatus::Ok
) {
    HttpResponse response(status);
    response.setBody(body);
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

void TestEmptyChain() {
    HttpMiddlewareChain chain;

    Check(chain.empty(), "New chain must be empty");
    Check(chain.size() == 0, "New chain size must be zero");

    auto handler = chain.compose(
        [](const HttpRequest&, const HttpRouteContext&) {
            return MakeResponse("terminal");
        }
    );

    Check(
        BodyText(handler(MakeRequest(), {})) == "terminal",
        "Empty chain must invoke terminal handler"
    );
}

void TestExecutionOrder() {
    HttpMiddlewareChain chain;
    std::vector<std::string> events;

    chain.add([&](
        const HttpRequest& request,
        const HttpRouteContext& context,
        const HttpMiddlewareNext& next
    ) {
        events.emplace_back("first:before");
        auto response = next(request, context);
        events.emplace_back("first:after");
        return response;
    });

    chain.add([&](
        const HttpRequest& request,
        const HttpRouteContext& context,
        const HttpMiddlewareNext& next
    ) {
        events.emplace_back("second:before");
        auto response = next(request, context);
        events.emplace_back("second:after");
        return response;
    });

    auto handler = chain.compose(
        [&](const HttpRequest&, const HttpRouteContext&) {
            events.emplace_back("terminal");
            return MakeResponse();
        }
    );

    handler(MakeRequest(), {});

    const std::vector<std::string> expected = {
        "first:before",
        "second:before",
        "terminal",
        "second:after",
        "first:after"
    };

    Check(events == expected, "Incorrect middleware execution order");
}

void TestShortCircuit() {
    HttpMiddlewareChain chain;
    int terminalCalls = 0;
    int downstreamCalls = 0;

    chain.add([](
        const HttpRequest&,
        const HttpRouteContext&,
        const HttpMiddlewareNext&
    ) {
        return MakeResponse(
            "blocked",
            HttpStatus::Forbidden
        );
    });

    chain.add([&](
        const HttpRequest& request,
        const HttpRouteContext& context,
        const HttpMiddlewareNext& next
    ) {
        ++downstreamCalls;
        return next(request, context);
    });

    auto handler = chain.compose(
        [&](const HttpRequest&, const HttpRouteContext&) {
            ++terminalCalls;
            return MakeResponse("unexpected");
        }
    );

    const auto response = handler(MakeRequest(), {});

    Check(response.statusCode() == 403, "Expected HTTP 403");
    Check(BodyText(response) == "blocked", "Incorrect response body");
    Check(terminalCalls == 0, "Terminal handler must not execute");
    Check(downstreamCalls == 0, "Downstream middleware must not execute");
}

void TestResponseModification() {
    HttpMiddlewareChain chain;

    chain.add([](
        const HttpRequest& request,
        const HttpRouteContext& context,
        const HttpMiddlewareNext& next
    ) {
        auto response = next(request, context);
        response.setHeader("X-Lamp-Test", "middleware");
        return response;
    });

    auto handler = chain.compose(
        [](const HttpRequest&, const HttpRouteContext&) {
            return MakeResponse("original");
        }
    );

    const auto response = handler(MakeRequest(), {});
    const auto header = response.header("X-Lamp-Test");

    Check(header.has_value(), "Middleware response header missing");
    Check(*header == "middleware", "Incorrect middleware header");
    Check(BodyText(response) == "original", "Body unexpectedly changed");
}

void TestRequestAndContextForwarding() {
    HttpMiddlewareChain chain;

    chain.add([](
        const HttpRequest& request,
        const HttpRouteContext& context,
        const HttpMiddlewareNext& next
    ) {
        Check(request.path() == "/users/42", "Incorrect request path");
        Check(context.param("id") == "42", "Incorrect route parameter");

        return next(request, context);
    });

    auto handler = chain.compose(
        [](const HttpRequest& request,
           const HttpRouteContext& context) {
            Check(
                request.method() == HttpMethod::Get,
                "Incorrect HTTP method"
            );

            return MakeResponse(context.param("id"));
        }
    );

    HttpRouteContext context;
    context.params.emplace("id", "42");

    const auto response = handler(
        MakeRequest(HttpMethod::Get, "/users/42"),
        context
    );

    Check(BodyText(response) == "42", "Context forwarding failed");
}

void TestInvalidRegistrations() {
    HttpMiddlewareChain chain;

    CheckThrows<HttpMiddlewareError>(
        [&] {
            chain.add(HttpMiddleware{});
        },
        "Empty middleware registration must fail"
    );

    CheckThrows<HttpMiddlewareError>(
        [&] {
            (void)chain.compose(HttpRouteHandler{});
        },
        "Empty terminal handler must fail"
    );

    Check(chain.empty(), "Invalid registration modified chain");
}

void TestDoubleNextProtection() {
    HttpMiddlewareChain chain;
    int terminalCalls = 0;

    chain.add([](
        const HttpRequest& request,
        const HttpRouteContext& context,
        const HttpMiddlewareNext& next
    ) {
        auto response = next(request, context);

        CheckThrows<HttpMiddlewareError>(
            [&] {
                (void)next(request, context);
            },
            "Second next invocation must throw"
        );

        return response;
    });

    auto handler = chain.compose(
        [&](const HttpRequest&, const HttpRouteContext&) {
            ++terminalCalls;
            return MakeResponse();
        }
    );

    (void)handler(MakeRequest(), {});

    Check(terminalCalls == 1, "Terminal handler executed multiple times");
}

void TestNextProtectionAfterException() {
    HttpMiddlewareChain chain;
    int terminalCalls = 0;

    chain.add([](
        const HttpRequest& request,
        const HttpRouteContext& context,
        const HttpMiddlewareNext& next
    ) {
        CheckThrows<std::runtime_error>(
            [&] {
                (void)next(request, context);
            },
            "Terminal exception must propagate"
        );

        CheckThrows<HttpMiddlewareError>(
            [&] {
                (void)next(request, context);
            },
            "Next must remain consumed after an exception"
        );

        return MakeResponse("recovered");
    });

    auto handler = chain.compose(
        [&](const HttpRequest&, const HttpRouteContext&)
            -> HttpResponse {
            ++terminalCalls;
            throw std::runtime_error("terminal failure");
        }
    );

    const auto response = handler(MakeRequest(), {});

    Check(terminalCalls == 1, "Terminal retried after exception");
    Check(BodyText(response) == "recovered", "Recovery failed");
}

void TestSnapshotIndependence() {
    HttpRouteHandler handler;

    {
        HttpMiddlewareChain chain;

        chain.add([](
            const HttpRequest& request,
            const HttpRouteContext& context,
            const HttpMiddlewareNext& next
        ) {
            auto response = next(request, context);
            response.setHeader("X-Snapshot", "original");
            return response;
        });

        handler = chain.compose(
            [](const HttpRequest&, const HttpRouteContext&) {
                return MakeResponse("snapshot");
            }
        );

        chain.clear();

        Check(chain.empty(), "Chain clear failed");
    }

    const auto response = handler(MakeRequest(), {});

    Check(
        BodyText(response) == "snapshot",
        "Composed handler did not survive chain destruction"
    );

    Check(
        response.header("X-Snapshot").value_or("") == "original",
        "Middleware snapshot was not preserved"
    );
}

void TestIndependentInvocations() {
    HttpMiddlewareChain chain;
    std::atomic<int> terminalCalls{0};

    chain.add([](
        const HttpRequest& request,
        const HttpRouteContext& context,
        const HttpMiddlewareNext& next
    ) {
        return next(request, context);
    });

    auto handler = chain.compose(
        [&](const HttpRequest&, const HttpRouteContext&) {
            terminalCalls.fetch_add(1, std::memory_order_relaxed);
            return MakeResponse();
        }
    );

    for (int i = 0; i < 100; ++i) {
        (void)handler(MakeRequest(), {});
    }

    Check(
        terminalCalls.load() == 100,
        "Per-request next state leaked between invocations"
    );
}

void TestRouterIntegration() {
    HttpRouter router;
    HttpMiddlewareChain chain;

    chain.add([](
        const HttpRequest& request,
        const HttpRouteContext& context,
        const HttpMiddlewareNext& next
    ) {
        auto response = next(request, context);
        response.setHeader("X-Route-Middleware", "active");
        return response;
    });

    router.get(
        "/users/:id",
        chain.compose(
            [](const HttpRequest&, const HttpRouteContext& context) {
                return MakeResponse(context.param("id"));
            }
        )
    );

    const auto response = router.dispatch(
        MakeRequest(HttpMethod::Get, "/users/123")
    );

    Check(response.statusCode() == 200, "Route dispatch failed");
    Check(BodyText(response) == "123", "Route parameter missing");
    Check(
        response.header("X-Route-Middleware").value_or("") == "active",
        "Middleware did not execute through HttpRouter"
    );

    const auto missing = router.dispatch(
        MakeRequest(HttpMethod::Get, "/missing")
    );

    Check(missing.statusCode() == 404, "Expected HTTP 404");

    const auto wrongMethod = router.dispatch(
        MakeRequest(HttpMethod::Post, "/users/123")
    );

    Check(wrongMethod.statusCode() == 405, "Expected HTTP 405");
}

void TestConcurrentInvocations() {
    HttpMiddlewareChain chain;

    std::atomic<int> middlewareCalls{0};
    std::atomic<int> terminalCalls{0};
    std::atomic<int> failures{0};

    chain.add([&](
        const HttpRequest& request,
        const HttpRouteContext& context,
        const HttpMiddlewareNext& next
    ) {
        middlewareCalls.fetch_add(
            1,
            std::memory_order_relaxed
        );

        auto response = next(request, context);
        response.setHeader("X-Concurrent", "yes");
        return response;
    });

    auto handler = chain.compose(
        [&](const HttpRequest&, const HttpRouteContext&) {
            terminalCalls.fetch_add(
                1,
                std::memory_order_relaxed
            );

            return MakeResponse("concurrent");
        }
    );

    constexpr int threadCount = 8;
    constexpr int requestsPerThread = 200;

    std::vector<std::thread> threads;
    threads.reserve(threadCount);

    for (int i = 0; i < threadCount; ++i) {
        threads.emplace_back([&] {
            try {
                for (int j = 0; j < requestsPerThread; ++j) {
                    const auto response = handler(
                        MakeRequest(),
                        {}
                    );

                    if (response.statusCode() != 200 ||
                        BodyText(response) != "concurrent" ||
                        response.header("X-Concurrent").value_or("") != "yes") {
                        failures.fetch_add(
                            1,
                            std::memory_order_relaxed
                        );
                    }
                }
            } catch (...) {
                failures.fetch_add(
                    1,
                    std::memory_order_relaxed
                );
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    constexpr int expected = threadCount * requestsPerThread;

    Check(failures.load() == 0, "Concurrent request failed");
    Check(
        middlewareCalls.load() == expected,
        "Incorrect concurrent middleware invocation count"
    );
    Check(
        terminalCalls.load() == expected,
        "Incorrect concurrent terminal invocation count"
    );
}

void TestConvenienceComposition() {
    auto handler = lamp::http::ComposeHttpMiddleware(
        [](const HttpRequest&, const HttpRouteContext&) {
            return MakeResponse("convenience");
        },
        {
            [](
                const HttpRequest& request,
                const HttpRouteContext& context,
                const HttpMiddlewareNext& next
            ) {
                auto response = next(request, context);
                response.setHeader("X-Compose", "yes");
                return response;
            }
        }
    );

    const auto response = handler(MakeRequest(), {});

    Check(BodyText(response) == "convenience", "Composition failed");
    Check(
        response.header("X-Compose").value_or("") == "yes",
        "Convenience middleware was not invoked"
    );
}

void TestOriginAllowanceMiddleware() {
    auto policy = std::make_shared<lamp::core::OriginAllowancePolicy>();

    auto handler = lamp::http::ComposeHttpMiddleware(
        [](const HttpRequest&, const HttpRouteContext&) {
            return MakeResponse("ok");
        },
        {lamp::http::MakeOriginAllowanceMiddleware(policy)}
    );

    // 1. Default DenyAll -> 403 CORS error
    {
        HttpRequest req = MakeRequest(HttpMethod::Get, "/api/data");
        req.setHeader("Origin", "https://client.example.com");
        const HttpResponse res = handler(req, HttpRouteContext{});
        Check(res.statusCode() == 403, "Expected 403 for denied origin");
        Check(res.header("X-Lamp-Cors-Error").value_or("") == "CORS_ORIGIN_DENIED",
              "Expected CORS_ORIGIN_DENIED header");
    }

    // 2. AllowAll -> 200 with Access-Control-Allow-Origin: *
    {
        policy->setAllowAll(true);
        HttpRequest req = MakeRequest(HttpMethod::Get, "/api/data");
        req.setHeader("Origin", "https://any.example.com");
        const HttpResponse res = handler(req, HttpRouteContext{});
        Check(res.statusCode() == 200, "Expected 200 for AllowAll");
        Check(res.header("Access-Control-Allow-Origin").value_or("") == "*",
              "Expected wildcard ACAO header");
    }

    // 3. AllowList -> allowed origin passes, disallowed origin fails, OPTIONS preflight returns 204
    {
        policy->setAllowedOrigins({"https://trusted.example.com"});

        HttpRequest okReq = MakeRequest(HttpMethod::Get, "/api/data");
        okReq.setHeader("Origin", "https://trusted.example.com");
        const HttpResponse okRes = handler(okReq, HttpRouteContext{});
        Check(okRes.statusCode() == 200, "Expected 200 for trusted origin");
        Check(okRes.header("Access-Control-Allow-Origin").value_or("") == "https://trusted.example.com",
              "Expected echoed ACAO header");

        HttpRequest preflightReq = MakeRequest(HttpMethod::Options, "/api/data");
        preflightReq.setHeader("Origin", "https://trusted.example.com");
        const HttpResponse preflightRes = handler(preflightReq, HttpRouteContext{});
        Check(preflightRes.statusCode() == 204, "Expected 204 for OPTIONS preflight");

        HttpRequest badReq = MakeRequest(HttpMethod::Get, "/api/data");
        badReq.setHeader("Origin", "https://untrusted.example.com");
        const HttpResponse badRes = handler(badReq, HttpRouteContext{});
        Check(badRes.statusCode() == 403, "Expected 403 for untrusted origin");
    }
}

struct TestCase final {
    const char* name;
    void (*run)();
};

} // namespace

int main() {
    const std::vector<TestCase> tests = {
        {"EmptyChain", TestEmptyChain},
        {"ExecutionOrder", TestExecutionOrder},
        {"ShortCircuit", TestShortCircuit},
        {"ResponseModification", TestResponseModification},
        {"RequestAndContextForwarding", TestRequestAndContextForwarding},
        {"InvalidRegistrations", TestInvalidRegistrations},
        {"DoubleNextProtection", TestDoubleNextProtection},
        {"NextProtectionAfterException", TestNextProtectionAfterException},
        {"SnapshotIndependence", TestSnapshotIndependence},
        {"IndependentInvocations", TestIndependentInvocations},
        {"RouterIntegration", TestRouterIntegration},
        {"ConcurrentInvocations", TestConcurrentInvocations},
        {"ConvenienceComposition", TestConvenienceComposition},
        {"OriginAllowanceMiddleware", TestOriginAllowanceMiddleware}
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
        << "Lamp HTTP middleware: "
        << passed << "/" << tests.size()
        << " tests passed\n";

    return passed == tests.size() ? 0 : 1;
}
