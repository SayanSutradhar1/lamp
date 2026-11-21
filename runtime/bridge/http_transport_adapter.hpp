
#pragma once

// ============================================================
// Lamp Framework
// Phase 4 - Native Execution Bridge
//
// File: runtime/bridge/http_transport_adapter.hpp
//
// Purpose:
//   Adapt native HTTP requests to Lamp operations.
//
// Dependencies:
//   - C++20 standard library
//   - http/http_server.hpp
//   - execution_dispatcher.hpp
//
// Design:
//   - HTTP is an optional transport.
//   - Explicit route-to-operation mapping.
//   - No V8 dependency.
//   - No Express-style application API.
//   - Binary-safe request/response bodies.
//   - No automatic exposure of arbitrary HTTP headers.
//   - Native HTTP server retains socket ownership.
//
// Execution:
//   The existing HTTP connection invokes a route handler.
//   The route handler calls ExecutionDispatcher::Dispatch().
//   Dispatch is synchronous in the current implementation.
//
// Limitations:
//   - No streaming.
//   - No asynchronous HTTP suspension.
//   - No automatic route removal.
//   - No implicit path/query decoding.
// ============================================================

#include "execution_dispatcher.hpp"

#include "http/http_server.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lamp::bridge {

// ============================================================
// HTTP adapter errors
// ============================================================

class HttpTransportAdapterError final
    : public std::runtime_error {
public:
    explicit HttpTransportAdapterError(
        const std::string& message
    )
        : std::runtime_error(message) {}
};

// ============================================================
// Route binding
// ============================================================

// A transport route is not an operation.
//
// Multiple HTTP routes may reference the same logical
// Lamp operation.
//
// Example:
//
//   GET /users/:id  -> users.get
//   POST /rpc/users  -> users.get
//
// The operation remains independent of HTTP.

struct HttpOperationRoute final {
    http::HttpMethod method = http::HttpMethod::Post;

    std::string pattern;

    OperationId operation_id;

    // Request body serialization expected from HTTP.
    //
    // Auto is permitted only when the body is empty.
    SerializationFormat input_format =
        SerializationFormat::Json;
};

// ============================================================
// HTTP transport adapter
// ============================================================

class HttpTransportAdapter final {
public:
    explicit HttpTransportAdapter(
        std::shared_ptr<ExecutionDispatcher> dispatcher
    )
        : dispatcher_(std::move(dispatcher)) {
        if (!dispatcher_) {
            throw HttpTransportAdapterError(
                "HTTP adapter requires an execution dispatcher"
            );
        }
    }

    HttpTransportAdapter(
        const HttpTransportAdapter&
    ) = delete;

    HttpTransportAdapter& operator=(
        const HttpTransportAdapter&
    ) = delete;

    // --------------------------------------------------------
    // Install route on native HTTP server
    // --------------------------------------------------------

    // The registered handler captures the dispatcher, not
    // the adapter itself.
    //
    // This allows the adapter to be destroyed without
    // invalidating the server's route handler.
    //
    // Route removal/replacement is governed by HttpRouter.
    //
    // Register routes during application startup.

    void Register(
        http::HttpServer& server,
        HttpOperationRoute route
    ) {
        ValidateRoute(route);

        auto dispatcher = dispatcher_;
        auto operation_id = std::move(route.operation_id);
        const auto format = route.input_format;

        server.route(
            route.method,
            std::move(route.pattern),
            [
                dispatcher = std::move(dispatcher),
                operation_id = std::move(operation_id),
                format
            ](
                const http::HttpRequest& request,
                const http::HttpRouteContext& context
            ) -> http::HttpResponse {
                return Handle(
                    *dispatcher,
                    operation_id,
                    format,
                    request,
                    context
                );
            }
        );
    }

    // --------------------------------------------------------
    // Shared HTTP <-> Bridge translation helpers
    //
    // Exposed statically so both synchronous HttpTransportAdapter
    // and asynchronous HTTP transport adapters share identical
    // route validation, envelope construction, error mapping,
    // and response serialization rules.
    // --------------------------------------------------------

    [[nodiscard]]
    static RequestId NextRequestId() noexcept {
        RequestId id;

        do {
            id = next_request_id_.fetch_add(
                1,
                std::memory_order_relaxed
            );
        } while (id == 0);

        return id;
    }

    static void ValidateRoute(
        const HttpOperationRoute& route
    ) {
        if (!http::isKnownHttpMethod(route.method)) {
            throw HttpTransportAdapterError(
                "HTTP operation route requires "
                "a known HTTP method"
            );
        }

        if (route.pattern.empty() ||
            route.pattern.front() != '/') {
            throw HttpTransportAdapterError(
                "HTTP route pattern must begin with '/'"
            );
        }

        if (!OperationDescriptor::IsValidId(
                route.operation_id)) {
            throw HttpTransportAdapterError(
                "Invalid Lamp operation identifier"
            );
        }

        switch (route.input_format) {
            case SerializationFormat::Auto:
            case SerializationFormat::Json:
            case SerializationFormat::Protobuf:
            case SerializationFormat::Binary:
                break;

            default:
                throw HttpTransportAdapterError(
                    "Invalid route input serialization"
                );
        }
    }

    [[nodiscard]]
    static RequestEnvelope BuildRequest(
        const OperationId& operation_id,
        SerializationFormat format,
        const http::HttpRequest& request
    ) {
        RequestEnvelope envelope;

        envelope.request_id = NextRequestId();
        envelope.operation_id = operation_id;
        envelope.transport = TransportKind::Http;
        envelope.format = format;

        // The HTTP parser owns std::byte body storage.
        //
        // The bridge owns uint8_t payload storage.
        //
        // Copy explicitly to avoid aliasing or lifetime
        // assumptions across transport boundaries.

        const auto& body = request.body();

        envelope.payload.reserve(body.size());

        for (const auto byte : body) {
            envelope.payload.push_back(
                std::to_integer<std::uint8_t>(byte)
            );
        }

        // Preserve selected transport context.
        //
        // Do not blindly forward Authorization, Cookie,
        // Host or other untrusted HTTP headers into the
        // operation metadata collection.
        //
        // Authentication and authorization must be handled
        // by a separate trusted policy layer.

        envelope.AddMetadata(
            "http.method",
            std::string(request.methodName())
        );

        envelope.AddMetadata(
            "http.path",
            request.path()
        );

        if (!request.query().empty()) {
            envelope.AddMetadata(
                "http.query",
                request.query()
            );
        }

        return envelope;
    }

    [[nodiscard]]
    static http::HttpStatus MapError(
        OperationErrorCode code
    ) noexcept {
        using E = OperationErrorCode;
        using S = http::HttpStatus;

        switch (code) {
            case E::None:
                return S::InternalServerError;

            case E::InvalidArgument:
                return S::BadRequest;

            case E::Unauthenticated:
                return S::Unauthorized;

            case E::PermissionDenied:
                return S::Forbidden;

            case E::NotFound:
                return S::NotFound;

            case E::AlreadyExists:
            case E::Conflict:
                return S::Conflict;

            case E::ResourceExhausted:
                return S::TooManyRequests;

            case E::FailedPrecondition:
                return S::PreconditionFailed;

            case E::Unavailable:
                return S::ServiceUnavailable;

            case E::Internal:
            case E::ExecutionFailed:
                return S::InternalServerError;

            case E::NotImplemented:
                return S::NotImplemented;

            case E::Cancelled:
                // HTTP has no universal client-cancelled
                // response code. Do not use nonstandard
                // status 499 as a public contract.
                return S::ServiceUnavailable;

            case E::DeadlineExceeded:
                return S::GatewayTimeout;

            case E::SerializationFailed:
                return S::InternalServerError;
        }

        return S::InternalServerError;
    }

    [[nodiscard]]
    static std::string_view ContentType(
        SerializationFormat format
    ) noexcept {
        switch (format) {
            case SerializationFormat::Json:
                return "application/json";

            case SerializationFormat::Protobuf:
                return "application/x-protobuf";

            case SerializationFormat::Binary:
                return "application/octet-stream";

            case SerializationFormat::Auto:
                return "application/octet-stream";
        }

        return "application/octet-stream";
    }

    [[nodiscard]]
    static http::HttpResponse BuildResponse(
        const ResultEnvelope& result,
        http::HttpVersion version
    ) {
        if (!result.IsSuccess()) {
            const auto status = MapError(
                result.error.code
            );

            http::HttpResponse response(
                status,
                version
            );

            // Public error responses use fixed messages.
            //
            // Handler-provided error messages are not
            // automatically exposed because they may
            // contain sensitive application details.

            response.setHeader(
                "Content-Type",
                "application/json"
            );

            response.setHeader(
                "Cache-Control",
                "no-store"
            );

            response.setBody(
                R"({"error":"operation_failed"})"
            );

            return response;
        }

        http::HttpResponse response(
            http::HttpStatus::Ok,
            version
        );

        response.setHeader(
            "Content-Type",
            std::string(ContentType(result.format))
        );

        http::HttpResponse::Body body;

        body.reserve(result.payload.size());

        for (const auto byte : result.payload) {
            body.push_back(
                static_cast<std::byte>(byte)
            );
        }

        response.setBody(std::move(body));

        return response;
    }

    [[nodiscard]]
    static http::HttpResponse InternalError(
        http::HttpVersion version
    ) {
        http::HttpResponse response(
            http::HttpStatus::InternalServerError,
            version
        );

        response.setHeader(
            "Content-Type",
            "application/json"
        );

        response.setHeader(
            "Cache-Control",
            "no-store"
        );

        response.setBody(
            R"({"error":"internal_server_error"})"
        );

        return response;
    }

private:

    // ========================================================
    // Request handler
    // ========================================================

    [[nodiscard]]
    static http::HttpResponse Handle(
        ExecutionDispatcher& dispatcher,
        const OperationId& operation_id,
        SerializationFormat format,
        const http::HttpRequest& request,
        const http::HttpRouteContext& context
    ) {
        // Route parameters belong to the HTTP transport.
        //
        // They are not implicitly merged into the binary
        // application payload. This avoids changing the
        // operation's input schema according to transport.
        //
        // A future codec layer may explicitly map route
        // parameters into schema-validated input.

        (void)context;

        try {
            RequestEnvelope envelope = BuildRequest(
                operation_id,
                format,
                request
            );

            // Reject requests with an unsupported content
            // type before interpreting the payload.
            //
            // This first adapter uses route configuration
            // as the serialization contract.
            //
            // Media-type negotiation will be implemented
            // by a dedicated codec layer.

            const auto result =
                dispatcher.Dispatch(envelope);

            return BuildResponse(
                result,
                request.version()
            );
        } catch (const std::exception&) {
            return InternalError(request.version());
        } catch (...) {
            return InternalError(request.version());
        }
    }

    // ========================================================
    // State
    // ========================================================

    std::shared_ptr<ExecutionDispatcher> dispatcher_;

    inline static std::atomic<RequestId>
        next_request_id_{1};
};

} // namespace lamp::bridge
