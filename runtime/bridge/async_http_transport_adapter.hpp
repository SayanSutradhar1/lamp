
#pragma once

// ============================================================
// Lamp Framework
// Phase 6B - Async HTTP Transport Adapter
//
// File: runtime/bridge/async_http_transport_adapter.hpp
//
// Purpose:
//   Connect HttpAsyncConnection to AsyncHttpOperationPump
//   without blocking the HTTP owner thread.
//
// Reuses HttpTransportAdapter's route validation, request
// envelope construction, error mapping, and response mapping.
// ============================================================

#include "bridge/async_http_operation_pump.hpp"
#include "bridge/http_transport_adapter.hpp"

#include "http/http_async_connection.hpp"
#include "http/http_async_server.hpp"
#include "http/http_router.hpp"

#include <exception>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

namespace lamp::bridge {

struct AsyncHttpOperationRoute final {
    HttpOperationRoute route;
    execution::AsyncScheduleOptions schedule_options{};
};

class AsyncHttpTransportAdapter final {
public:
    explicit AsyncHttpTransportAdapter(
        std::shared_ptr<AsyncHttpOperationPump> pump
    )
        : state_(std::make_shared<State>(std::move(pump))) {
        if (!state_->pump) {
            throw HttpTransportAdapterError(
                "Async HTTP adapter requires an operation pump"
            );
        }
    }

    AsyncHttpTransportAdapter(
        const AsyncHttpTransportAdapter&
    ) = delete;

    AsyncHttpTransportAdapter& operator=(
        const AsyncHttpTransportAdapter&
    ) = delete;

    // ========================================================
    // Default Response Mapper
    //
    // Reuses HttpTransportAdapter::BuildResponse so async and
    // sync HTTP endpoints produce identical status codes,
    // Content-Type headers, and sanitized error bodies.
    // ========================================================

    [[nodiscard]]
    static AsyncHttpOperationPump::ResponseMapper
    DefaultResponseMapper() {
        return [](AsyncHttpOperationPump::TaskHandle handle)
                   -> http::HttpResponse {
            const auto result = handle.Get();
            return HttpTransportAdapter::BuildResponse(
                result,
                http::HttpVersion{}
            );
        };
    }

    // ========================================================
    // Route Registration
    // ========================================================

    void Register(
        HttpOperationRoute route,
        execution::AsyncScheduleOptions options = {}
    ) {
        HttpTransportAdapter::ValidateRoute(route);

        http::HttpRoutePattern compiledPattern(
            route.pattern
        );

        std::unique_lock lock(state_->routesMutex);

        for (const auto& existing : state_->routes) {
            if (existing.route.method == route.method &&
                existing.route.pattern == route.pattern) {
                throw HttpTransportAdapterError(
                    "Duplicate async HTTP operation route"
                );
            }
        }

        state_->routes.push_back(CompiledRoute{
            std::move(route),
            std::move(compiledPattern),
            options
        });
    }

    [[nodiscard]]
    std::size_t RouteCount() const noexcept {
        std::shared_lock lock(state_->routesMutex);
        return state_->routes.size();
    }

    // ========================================================
    // Connection Dispatcher Factory
    //
    // Returns a callback suitable for HttpAsyncConnection.
    // Captures shared adapter state (not `this`) and weak_ptr
    // to the connection so adapter destruction cannot dangle.
    // ========================================================

    [[nodiscard]]
    http::HttpAsyncConnection::DispatchCallback
    MakeConnectionDispatcher(
        std::weak_ptr<http::HttpAsyncConnection> connection
    ) const {
        return [
            state = state_,
            connection = std::move(connection)
        ](
            const http::HttpCompletionToken& token,
            http::HttpRequest request
        ) {
            DispatchWithState(*state, token, connection, request);
        };
    }

    void Dispatch(
        const http::HttpCompletionToken& token,
        std::weak_ptr<http::HttpAsyncConnection> connection,
        const http::HttpRequest& request
    ) const {
        DispatchWithState(*state_, token, std::move(connection), request);
    }

    // ========================================================
    // Async HTTP Server Callback Binding
    //
    // Produces owner-thread callbacks for HttpAsyncServer:
    //   - onAccept: routes requests via MakeConnectionDispatcher
    //   - onTick: advances pump (Poll + Drain)
    //   - onConnectionClosed: cancels disconnected client tasks
    // ========================================================

    [[nodiscard]]
    http::HttpAsyncServerCallbacks CreateServerCallbacks(
        std::size_t maxDrainPerTick = 1024
    ) const {
        http::HttpAsyncServerCallbacks callbacks;

        callbacks.onAccept = [state = state_](
            std::weak_ptr<http::HttpAsyncConnection> connection
        ) -> http::HttpAsyncConnection::DispatchCallback {
            return [state, connection = std::move(connection)](
                       const http::HttpCompletionToken& token,
                       http::HttpRequest request
                   ) {
                DispatchWithState(
                    *state,
                    token,
                    connection,
                    request
                );
            };
        };

        callbacks.onTick = [state = state_, maxDrainPerTick]() {
            if (!state->pump->Closed()) {
                (void)state->pump->Advance(maxDrainPerTick);
            }
        };

        callbacks.onConnectionClosed = [state = state_](
            std::uint64_t connectionId,
            std::uint64_t generation
        ) {
            if (!state->pump->Closed()) {
                (void)state->pump->CancelConnection(
                    connectionId,
                    generation
                );
            }
        };

        return callbacks;
    }

    [[nodiscard]]
    std::shared_ptr<AsyncHttpOperationPump>
    Pump() const noexcept {
        return state_->pump;
    }

private:
    struct CompiledRoute final {
        HttpOperationRoute route;
        http::HttpRoutePattern pattern;
        execution::AsyncScheduleOptions options;
    };

    struct State final {
        explicit State(
            std::shared_ptr<AsyncHttpOperationPump> p
        )
            : pump(std::move(p)) {}

        std::shared_ptr<AsyncHttpOperationPump> pump;
        mutable std::shared_mutex routesMutex;
        std::vector<CompiledRoute> routes;
    };

    static void DispatchWithState(
        const State& state,
        const http::HttpCompletionToken& token,
        std::weak_ptr<http::HttpAsyncConnection> connection,
        const http::HttpRequest& request
    ) {
        auto target = connection.lock();

        if (!target || target->closed()) {
            return;
        }

        try {
            const CompiledRoute* matched = nullptr;
            bool pathMatchedOtherMethod = false;
            std::vector<http::HttpMethod> allowedMethods;

            {
                std::shared_lock lock(state.routesMutex);
                http::HttpRouteParams params;

                for (const auto& entry : state.routes) {
                    params.clear();

                    if (!entry.pattern.matches(
                            request.path(),
                            params
                        )) {
                        continue;
                    }

                    if (entry.route.method == request.method()) {
                        matched = &entry;
                        break;
                    }

                    pathMatchedOtherMethod = true;
                    allowedMethods.push_back(entry.route.method);
                }

                if (matched != nullptr) {
                    RequestEnvelope envelope =
                        HttpTransportAdapter::BuildRequest(
                            matched->route.operation_id,
                            matched->route.input_format,
                            request
                        );

                    const auto options = matched->options;
                    lock.unlock();

                    (void)state.pump->SubmitOrComplete(
                        token,
                        std::move(connection),
                        std::move(envelope),
                        options,
                        request.version()
                    );
                    return;
                }
            }

            if (pathMatchedOtherMethod) {
                http::HttpResponse response(
                    http::HttpStatus::MethodNotAllowed,
                    request.version()
                );

                std::string allow;
                for (std::size_t i = 0;
                     i < allowedMethods.size();
                     ++i) {
                    if (i > 0) {
                        allow.append(", ");
                    }
                    allow.append(
                        http::httpMethodToString(
                            allowedMethods[i]
                        )
                    );
                }

                if (!allow.empty()) {
                    response.setHeader(
                        "Allow",
                        std::move(allow)
                    );
                }

                response.setHeader(
                    "Content-Type",
                    "application/json"
                );
                response.setHeader(
                    "Cache-Control",
                    "no-store"
                );
                response.setBody(
                    R"({"error":"method_not_allowed"})"
                );

                (void)target->complete(
                    token,
                    std::move(response)
                );
                return;
            }

            http::HttpResponse notFound(
                http::HttpStatus::NotFound,
                request.version()
            );
            notFound.setHeader(
                "Content-Type",
                "application/json"
            );
            notFound.setHeader("Cache-Control", "no-store");
            notFound.setBody(R"({"error":"not_found"})");

            (void)target->complete(token, std::move(notFound));
        } catch (const std::exception&) {
            (void)target->complete(
                token,
                HttpTransportAdapter::InternalError(
                    request.version()
                )
            );
        } catch (...) {
            (void)target->complete(
                token,
                HttpTransportAdapter::InternalError(
                    request.version()
                )
            );
        }
    }

    std::shared_ptr<State> state_;
};

} // namespace lamp::bridge
