
#pragma once

#include "http/http_headers.hpp"
#include "http/http_method.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lamp::http {

// ============================================================
// HTTP Request Error
// ============================================================

class HttpRequestError final : public std::invalid_argument {
public:
    explicit HttpRequestError(const std::string& message)
        : std::invalid_argument(message) {}
};

// ============================================================
// HTTP Version
// ============================================================

struct HttpVersion final {
    std::uint16_t major = 1;
    std::uint16_t minor = 1;

    [[nodiscard]]
    constexpr bool operator==(
        const HttpVersion&
    ) const noexcept = default;

    [[nodiscard]]
    constexpr bool isHttp10() const noexcept {
        return major == 1 && minor == 0;
    }

    [[nodiscard]]
    constexpr bool isHttp11() const noexcept {
        return major == 1 && minor == 1;
    }

    [[nodiscard]]
    constexpr bool isSupported() const noexcept {
        return isHttp10() || isHttp11();
    }

    [[nodiscard]]
    constexpr std::string_view toString() const noexcept {
        if (isHttp10()) {
            return "HTTP/1.0";
        }

        if (isHttp11()) {
            return "HTTP/1.1";
        }

        return {};
    }
};

// ============================================================
// Request Target Form
// ============================================================
//
// HTTP/1.1 supports four request-target forms:
//
// Origin:
//   GET /users?id=42 HTTP/1.1
//
// Absolute:
//   GET http://example.com/users HTTP/1.1
//
// Authority:
//   CONNECT example.com:443 HTTP/1.1
//
// Asterisk:
//   OPTIONS * HTTP/1.1
//
// Target-form validation and authority parsing belong to
// the HTTP parser, which has access to the complete request
// line and protocol-specific constraints.
//
// ============================================================

enum class HttpTargetForm : std::uint8_t {
    Origin,
    Absolute,
    Authority,
    Asterisk
};

// ============================================================
// Request Target
// ============================================================
//
// The raw target is preserved exactly as received.
//
// For origin-form targets:
//   raw   = "/users?id=42"
//   path  = "/users"
//   query = "id=42"
//
// Path and query are not percent-decoded.
//
// This is intentional: decoding requires routing-specific
// security rules and must not be performed implicitly.
//
// ============================================================

class HttpRequestTarget final {
public:
    HttpRequestTarget() = default;

    explicit HttpRequestTarget(
        std::string target
    ) {
        set(std::move(target));
    }

    void set(std::string target) {
        if (target.empty()) {
            throw HttpRequestError(
                "HTTP request target cannot be empty"
            );
        }

        for (const unsigned char character : target) {
            if (character <= 0x20 || character == 0x7F) {
                throw HttpRequestError(
                    "HTTP request target contains "
                    "an invalid character"
                );
            }
        }

        HttpTargetForm form = HttpTargetForm::Origin;
        std::string_view path;
        std::string_view query;

        const std::string_view view(target);

        if (view == "*") {
            form = HttpTargetForm::Asterisk;
            path = view;
        } else if (view.front() == '/') {
            form = HttpTargetForm::Origin;

            const auto queryPosition = view.find('?');

            if (queryPosition == std::string_view::npos) {
                path = view;
            } else {
                path = view.substr(0, queryPosition);
                query = view.substr(queryPosition + 1);
            }
        } else if (
            view.starts_with("http://") ||
            view.starts_with("https://")
        ) {
            form = HttpTargetForm::Absolute;

            const auto schemeEnd = view.find("://");
            const auto authorityStart = schemeEnd + 3;
            const auto pathStart =
                view.find_first_of("/?", authorityStart);

            if (pathStart == std::string_view::npos) {
                path = "/";
            } else if (view[pathStart] == '?') {
                path = "/";
                query = view.substr(pathStart + 1);
            } else {
                const auto queryPosition =
                    view.find('?', pathStart);

                if (queryPosition == std::string_view::npos) {
                    path = view.substr(pathStart);
                } else {
                    path = view.substr(
                        pathStart,
                        queryPosition - pathStart
                    );

                    query = view.substr(queryPosition + 1);
                }
            }
        } else {
            form = HttpTargetForm::Authority;
            path = view;
        }

        // Views above refer to the local target string.
        // Store owned components before moving the source.

        std::string parsedPath(path);
        std::string parsedQuery(query);

        raw_ = std::move(target);
        path_ = std::move(parsedPath);
        query_ = std::move(parsedQuery);
        form_ = form;
    }

    [[nodiscard]]
    const std::string& raw() const noexcept {
        return raw_;
    }

    [[nodiscard]]
    const std::string& path() const noexcept {
        return path_;
    }

    [[nodiscard]]
    const std::string& query() const noexcept {
        return query_;
    }

    [[nodiscard]]
    HttpTargetForm form() const noexcept {
        return form_;
    }

    [[nodiscard]]
    bool hasQuery() const noexcept {
        return !query_.empty();
    }

    [[nodiscard]]
    bool isOriginForm() const noexcept {
        return form_ == HttpTargetForm::Origin;
    }

private:
    std::string raw_ = "/";
    std::string path_ = "/";
    std::string query_;

    HttpTargetForm form_ = HttpTargetForm::Origin;
};

// ============================================================
// HTTP Request
// ============================================================
//
// Represents a fully parsed native HTTP request.
//
// Ownership:
//   - Method token is owned.
//   - Request target is owned.
//   - Headers are owned.
//   - Body bytes are owned.
//
// The request can be safely moved between native components.
//
// Thread safety:
//   Individual instances are not internally synchronized.
//   Concurrent mutation requires external synchronization.
//
// ============================================================

class HttpRequest final {
public:
    using Body = std::vector<std::byte>;

    HttpRequest() = default;

    HttpRequest(
        HttpMethod method,
        std::string target,
        HttpVersion version = {}
    )
        : method_(method),
          target_(std::move(target)),
          version_(version) {
        if (!isKnownHttpMethod(method_)) {
            throw HttpRequestError(
                "Unknown HTTP method requires "
                "an explicit method token"
            );
        }

        if (!version_.isSupported()) {
            throw HttpRequestError(
                "Unsupported HTTP version"
            );
        }

        methodToken_ =
            std::string(httpMethodToString(method_));
    }

    HttpRequest(
        std::string methodToken,
        std::string target,
        HttpVersion version = {}
    )
        : target_(std::move(target)),
          version_(version) {
        setMethod(std::move(methodToken));

        if (!version_.isSupported()) {
            throw HttpRequestError(
                "Unsupported HTTP version"
            );
        }
    }

    // ========================================================
    // Method
    // ========================================================

    [[nodiscard]]
    HttpMethod method() const noexcept {
        return method_;
    }

    [[nodiscard]]
    std::string_view methodName() const noexcept {
        return methodToken_;
    }

    void setMethod(HttpMethod method) {
        if (!isKnownHttpMethod(method)) {
            throw HttpRequestError(
                "Cannot set an unknown HTTP method "
                "without its original token"
            );
        }

        std::string token(httpMethodToString(method));

        methodToken_ = std::move(token);
        method_ = method;
    }

    void setMethod(std::string methodToken) {
        const auto parsed = parseHttpMethodToken(methodToken);

        if (!parsed.has_value()) {
            throw HttpRequestError(
                "Invalid HTTP method token"
            );
        }

        methodToken_ = std::move(methodToken);
        method_ = *parsed;
    }

    // ========================================================
    // Request Target
    // ========================================================

    [[nodiscard]]
    const HttpRequestTarget& target() const noexcept {
        return target_;
    }

    void setTarget(std::string target) {
        target_.set(std::move(target));
    }

    [[nodiscard]]
    const std::string& path() const noexcept {
        return target_.path();
    }

    [[nodiscard]]
    const std::string& query() const noexcept {
        return target_.query();
    }

    // ========================================================
    // HTTP Version
    // ========================================================

    [[nodiscard]]
    HttpVersion version() const noexcept {
        return version_;
    }

    void setVersion(HttpVersion version) {
        if (!version.isSupported()) {
            throw HttpRequestError(
                "Unsupported HTTP version"
            );
        }

        version_ = version;
    }

    // ========================================================
    // Headers
    // ========================================================

    [[nodiscard]]
    HttpHeaders& headers() noexcept {
        return headers_;
    }

    [[nodiscard]]
    const HttpHeaders& headers() const noexcept {
        return headers_;
    }

    [[nodiscard]]
    std::optional<std::string_view> header(
        std::string_view name
    ) const noexcept {
        return headers_.get(name);
    }

    void setHeader(
        std::string name,
        std::string value
    ) {
        headers_.set(
            std::move(name),
            std::move(value)
        );
    }

    void appendHeader(
        std::string name,
        std::string value
    ) {
        headers_.append(
            std::move(name),
            std::move(value)
        );
    }

    // ========================================================
    // Request Body
    // ========================================================

    [[nodiscard]]
    const Body& body() const noexcept {
        return body_;
    }

    [[nodiscard]]
    Body& body() noexcept {
        return body_;
    }

    void setBody(Body body) noexcept {
        body_ = std::move(body);
    }

    void clearBody() noexcept {
        body_.clear();
    }

    [[nodiscard]]
    bool hasBody() const noexcept {
        return !body_.empty();
    }

    [[nodiscard]]
    std::size_t bodySize() const noexcept {
        return body_.size();
    }

    // ========================================================
    // Request Metadata
    // ========================================================

    [[nodiscard]]
    bool isKeepAlive() const noexcept {
        const auto connection = header("Connection");

        if (connection.has_value()) {
            if (containsConnectionToken(
                    *connection,
                    "close"
                )) {
                return false;
            }

            if (containsConnectionToken(
                    *connection,
                    "keep-alive"
                )) {
                return true;
            }
        }

        // HTTP/1.1 connections are persistent by default.
        // HTTP/1.0 connections are nonpersistent by default.
        return version_.isHttp11();
    }

    // ========================================================
    // Reset
    // ========================================================

    void reset() {
        method_ = HttpMethod::Get;
        methodToken_ = "GET";
        target_ = HttpRequestTarget{};
        version_ = HttpVersion{};
        headers_.clear();
        body_.clear();
    }

private:
    // ========================================================
    // Connection Header Token Matching
    // ========================================================
    //
    // Connection options are case-insensitive tokens.
    //
    // This helper matches complete comma-separated tokens,
    // not substrings.
    //
    // ========================================================

    [[nodiscard]]
    static constexpr bool asciiEqualsIgnoreCase(
        std::string_view left,
        std::string_view right
    ) noexcept {
        if (left.size() != right.size()) {
            return false;
        }

        for (std::size_t i = 0; i < left.size(); ++i) {
            const auto a = static_cast<unsigned char>(left[i]);
            const auto b = static_cast<unsigned char>(right[i]);

            const auto lowerA =
                (a >= 'A' && a <= 'Z')
                    ? static_cast<unsigned char>(a + 32)
                    : a;

            const auto lowerB =
                (b >= 'A' && b <= 'Z')
                    ? static_cast<unsigned char>(b + 32)
                    : b;

            if (lowerA != lowerB) {
                return false;
            }
        }

        return true;
    }

    [[nodiscard]]
    static constexpr std::string_view trimOptionalWhitespace(
        std::string_view value
    ) noexcept {
        while (!value.empty() &&
               (value.front() == ' ' ||
                value.front() == '\t')) {
            value.remove_prefix(1);
        }

        while (!value.empty() &&
               (value.back() == ' ' ||
                value.back() == '\t')) {
            value.remove_suffix(1);
        }

        return value;
    }

    [[nodiscard]]
    static constexpr bool containsConnectionToken(
        std::string_view headerValue,
        std::string_view token
    ) noexcept {
        while (!headerValue.empty()) {
            const auto separator = headerValue.find(',');

            const auto candidate =
                trimOptionalWhitespace(
                    headerValue.substr(0, separator)
                );

            if (asciiEqualsIgnoreCase(candidate, token)) {
                return true;
            }

            if (separator == std::string_view::npos) {
                break;
            }

            headerValue.remove_prefix(separator + 1);
        }

        return false;
    }

    HttpMethod method_ = HttpMethod::Get;
    std::string methodToken_ = "GET";

    HttpRequestTarget target_;
    HttpVersion version_;

    HttpHeaders headers_;
    Body body_;
};

// ============================================================
// Compile-Time Contract Checks
// ============================================================

static_assert(HttpVersion{}.isHttp11());
static_assert(HttpVersion{1, 0}.isHttp10());
static_assert(!HttpVersion{2, 0}.isSupported());

static_assert(
    HttpVersion{1, 1}.toString() == "HTTP/1.1"
);

static_assert(
    HttpVersion{1, 0}.toString() == "HTTP/1.0"
);

} // namespace lamp::http
