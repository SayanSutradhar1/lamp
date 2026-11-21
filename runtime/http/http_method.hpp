
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace lamp::http {

// ============================================================
// HTTP Method
// ============================================================
//
// Represents standard HTTP request methods.
//
// Unknown is used for syntactically valid extension methods
// that Lamp does not recognize as standard methods.
//
// HTTP method names are case-sensitive.
//
// Examples:
//   GET     -> Get
//   POST    -> Post
//   PATCH   -> Patch
//   CUSTOM  -> Unknown
//   get     -> Unknown
//
// The original method token should be preserved separately
// by HttpRequest when extension methods are supported.
//
// ============================================================

enum class HttpMethod : std::uint8_t {
    Unknown = 0,

    Get,
    Head,
    Post,
    Put,
    Delete,
    Connect,
    Options,
    Trace,
    Patch
};

// ============================================================
// HTTP Method Metadata
// ============================================================

inline constexpr std::size_t kStandardHttpMethodCount = 9;

inline constexpr std::string_view kUnknownHttpMethodName =
    "UNKNOWN";

// ============================================================
// HTTP Token Validation
// ============================================================
//
// RFC 9110 defines method names as HTTP tokens.
//
// A token contains one or more ASCII characters from:
//
//   ALPHA / DIGIT / "!#$%&'*+-.^_`|~"
//
// Spaces, control characters, and non-ASCII bytes are invalid.
//
// This function validates the syntax of a method token.
// It does not require the method to be recognized by Lamp.
//
// ============================================================

[[nodiscard]]
constexpr bool isHttpTokenCharacter(
    unsigned char character
) noexcept {
    if (character >= 'A' && character <= 'Z') {
        return true;
    }

    if (character >= 'a' && character <= 'z') {
        return true;
    }

    if (character >= '0' && character <= '9') {
        return true;
    }

    switch (character) {
        case '!':
        case '#':
        case '$':
        case '%':
        case '&':
        case '\'':
        case '*':
        case '+':
        case '-':
        case '.':
        case '^':
        case '_':
        case '`':
        case '|':
        case '~':
            return true;

        default:
            return false;
    }
}

[[nodiscard]]
constexpr bool isValidHttpMethodToken(
    std::string_view method
) noexcept {
    if (method.empty()) {
        return false;
    }

    for (const char character : method) {
        if (!isHttpTokenCharacter(
                static_cast<unsigned char>(character)
            )) {
            return false;
        }
    }

    return true;
}

// ============================================================
// HTTP Method Parsing
// ============================================================
//
// Converts a standard HTTP method name to HttpMethod.
//
// Matching is intentionally case-sensitive.
//
// Unknown and extension methods return HttpMethod::Unknown.
//
// This function does not validate the method token.
// Use parseHttpMethodToken() when validation is required.
//
// ============================================================

[[nodiscard]]
constexpr HttpMethod httpMethodFromString(
    std::string_view method
) noexcept {
    if (method == "GET") {
        return HttpMethod::Get;
    }

    if (method == "HEAD") {
        return HttpMethod::Head;
    }

    if (method == "POST") {
        return HttpMethod::Post;
    }

    if (method == "PUT") {
        return HttpMethod::Put;
    }

    if (method == "DELETE") {
        return HttpMethod::Delete;
    }

    if (method == "CONNECT") {
        return HttpMethod::Connect;
    }

    if (method == "OPTIONS") {
        return HttpMethod::Options;
    }

    if (method == "TRACE") {
        return HttpMethod::Trace;
    }

    if (method == "PATCH") {
        return HttpMethod::Patch;
    }

    return HttpMethod::Unknown;
}

// ============================================================
// Validated HTTP Method Parsing
// ============================================================
//
// Returns:
//   std::nullopt         -> Invalid HTTP method token
//   HttpMethod::Unknown  -> Valid extension/unknown method
//   Standard method      -> Recognized standard method
//
// This distinction allows the HTTP parser to reject malformed
// method tokens without rejecting valid extension methods.
//
// ============================================================

[[nodiscard]]
constexpr std::optional<HttpMethod> parseHttpMethodToken(
    std::string_view method
) noexcept {
    if (!isValidHttpMethodToken(method)) {
        return std::nullopt;
    }

    return httpMethodFromString(method);
}

// ============================================================
// HTTP Method String Conversion
// ============================================================
//
// Returns a static string_view.
//
// No heap allocation occurs.
//
// Unknown returns "UNKNOWN", which is a display fallback,
// not necessarily the original incoming method token.
//
// ============================================================

[[nodiscard]]
constexpr std::string_view httpMethodToString(
    HttpMethod method
) noexcept {
    switch (method) {
        case HttpMethod::Get:
            return "GET";

        case HttpMethod::Head:
            return "HEAD";

        case HttpMethod::Post:
            return "POST";

        case HttpMethod::Put:
            return "PUT";

        case HttpMethod::Delete:
            return "DELETE";

        case HttpMethod::Connect:
            return "CONNECT";

        case HttpMethod::Options:
            return "OPTIONS";

        case HttpMethod::Trace:
            return "TRACE";

        case HttpMethod::Patch:
            return "PATCH";

        case HttpMethod::Unknown:
            return kUnknownHttpMethodName;
    }

    return kUnknownHttpMethodName;
}

// ============================================================
// HTTP Method Classification
// ============================================================

[[nodiscard]]
constexpr bool isKnownHttpMethod(
    HttpMethod method
) noexcept {
    switch (method) {
        case HttpMethod::Get:
        case HttpMethod::Head:
        case HttpMethod::Post:
        case HttpMethod::Put:
        case HttpMethod::Delete:
        case HttpMethod::Connect:
        case HttpMethod::Options:
        case HttpMethod::Trace:
        case HttpMethod::Patch:
            return true;

        case HttpMethod::Unknown:
            return false;
    }

    return false;
}

// ============================================================
// HTTP Method Semantics
// ============================================================
//
// Safe methods are defined by HTTP semantics as read-only.
//
// Safe:
//   GET, HEAD, OPTIONS, TRACE
//
// Idempotent:
//   GET, HEAD, PUT, DELETE, OPTIONS, TRACE
//
// These classifications describe protocol semantics.
// Application handlers must still honor those semantics.
//
// ============================================================

[[nodiscard]]
constexpr bool isSafeHttpMethod(
    HttpMethod method
) noexcept {
    switch (method) {
        case HttpMethod::Get:
        case HttpMethod::Head:
        case HttpMethod::Options:
        case HttpMethod::Trace:
            return true;

        default:
            return false;
    }
}

[[nodiscard]]
constexpr bool isIdempotentHttpMethod(
    HttpMethod method
) noexcept {
    switch (method) {
        case HttpMethod::Get:
        case HttpMethod::Head:
        case HttpMethod::Put:
        case HttpMethod::Delete:
        case HttpMethod::Options:
        case HttpMethod::Trace:
            return true;

        default:
            return false;
    }
}

// ============================================================
// Compile-Time Contract Checks
// ============================================================

static_assert(kStandardHttpMethodCount == 9);

static_assert(
    httpMethodFromString("GET") == HttpMethod::Get
);

static_assert(
    httpMethodFromString("POST") == HttpMethod::Post
);

static_assert(
    httpMethodFromString("PATCH") == HttpMethod::Patch
);

static_assert(
    httpMethodFromString("get") == HttpMethod::Unknown
);

static_assert(
    httpMethodFromString("CUSTOM") == HttpMethod::Unknown
);

static_assert(
    isValidHttpMethodToken("CUSTOM-METHOD")
);

static_assert(
    !isValidHttpMethodToken("")
);

static_assert(
    !isValidHttpMethodToken("GET POST")
);

static_assert(
    !isValidHttpMethodToken("GET\r\n")
);

static_assert(
    parseHttpMethodToken("CUSTOM").has_value()
);

static_assert(
    !parseHttpMethodToken("BAD METHOD").has_value()
);

static_assert(
    httpMethodToString(HttpMethod::Delete) == "DELETE"
);

static_assert(
    isSafeHttpMethod(HttpMethod::Get)
);

static_assert(
    !isSafeHttpMethod(HttpMethod::Post)
);

static_assert(
    isIdempotentHttpMethod(HttpMethod::Put)
);

static_assert(
    !isIdempotentHttpMethod(HttpMethod::Patch)
);

} // namespace lamp::http
