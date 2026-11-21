
#pragma once

#include "http/http_headers.hpp"
#include "http/http_request.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lamp::http {

// ============================================================
// HTTP Response Errors
// ============================================================

class HttpResponseError final : public std::runtime_error {
public:
    explicit HttpResponseError(const std::string& message)
        : std::runtime_error(message) {}
};

// ============================================================
// HTTP Status Codes
// ============================================================

enum class HttpStatus : std::uint16_t {
    Continue = 100,
    SwitchingProtocols = 101,
    Processing = 102,
    EarlyHints = 103,

    Ok = 200,
    Created = 201,
    Accepted = 202,
    NonAuthoritativeInformation = 203,
    NoContent = 204,
    ResetContent = 205,
    PartialContent = 206,
    MultiStatus = 207,
    AlreadyReported = 208,
    ImUsed = 226,

    MultipleChoices = 300,
    MovedPermanently = 301,
    Found = 302,
    SeeOther = 303,
    NotModified = 304,
    UseProxy = 305,
    TemporaryRedirect = 307,
    PermanentRedirect = 308,

    BadRequest = 400,
    Unauthorized = 401,
    PaymentRequired = 402,
    Forbidden = 403,
    NotFound = 404,
    MethodNotAllowed = 405,
    NotAcceptable = 406,
    ProxyAuthenticationRequired = 407,
    RequestTimeout = 408,
    Conflict = 409,
    Gone = 410,
    LengthRequired = 411,
    PreconditionFailed = 412,
    PayloadTooLarge = 413,
    UriTooLong = 414,
    UnsupportedMediaType = 415,
    RangeNotSatisfiable = 416,
    ExpectationFailed = 417,
    ImATeapot = 418,
    MisdirectedRequest = 421,
    UnprocessableContent = 422,
    Locked = 423,
    FailedDependency = 424,
    TooEarly = 425,
    UpgradeRequired = 426,
    PreconditionRequired = 428,
    TooManyRequests = 429,
    RequestHeaderFieldsTooLarge = 431,
    UnavailableForLegalReasons = 451,

    InternalServerError = 500,
    NotImplemented = 501,
    BadGateway = 502,
    ServiceUnavailable = 503,
    GatewayTimeout = 504,
    HttpVersionNotSupported = 505,
    VariantAlsoNegotiates = 506,
    InsufficientStorage = 507,
    LoopDetected = 508,
    NotExtended = 510,
    NetworkAuthenticationRequired = 511
};

// ============================================================
// HTTP Status Utilities
// ============================================================

[[nodiscard]]
constexpr std::uint16_t httpStatusCode(
    HttpStatus status
) noexcept {
    return static_cast<std::uint16_t>(status);
}

[[nodiscard]]
constexpr bool isValidHttpStatusCode(
    std::uint16_t status
) noexcept {
    return status >= 100 && status <= 599;
}

[[nodiscard]]
constexpr bool isInformationalHttpStatus(
    std::uint16_t status
) noexcept {
    return status >= 100 && status < 200;
}

[[nodiscard]]
constexpr bool isSuccessfulHttpStatus(
    std::uint16_t status
) noexcept {
    return status >= 200 && status < 300;
}

[[nodiscard]]
constexpr bool isRedirectionHttpStatus(
    std::uint16_t status
) noexcept {
    return status >= 300 && status < 400;
}

[[nodiscard]]
constexpr bool isClientErrorHttpStatus(
    std::uint16_t status
) noexcept {
    return status >= 400 && status < 500;
}

[[nodiscard]]
constexpr bool isServerErrorHttpStatus(
    std::uint16_t status
) noexcept {
    return status >= 500 && status < 600;
}

// ============================================================
// HTTP Reason Phrases
// ============================================================
//
// Reason phrases are optional in HTTP/1.1 and have no
// protocol semantics.
//
// Unknown but valid status codes receive an empty phrase.
//
// ============================================================

[[nodiscard]]
constexpr std::string_view httpReasonPhrase(
    std::uint16_t status
) noexcept {
    switch (status) {
        case 100: return "Continue";
        case 101: return "Switching Protocols";
        case 102: return "Processing";
        case 103: return "Early Hints";

        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 203: return "Non-Authoritative Information";
        case 204: return "No Content";
        case 205: return "Reset Content";
        case 206: return "Partial Content";
        case 207: return "Multi-Status";
        case 208: return "Already Reported";
        case 226: return "IM Used";

        case 300: return "Multiple Choices";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 303: return "See Other";
        case 304: return "Not Modified";
        case 305: return "Use Proxy";
        case 307: return "Temporary Redirect";
        case 308: return "Permanent Redirect";

        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 402: return "Payment Required";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 406: return "Not Acceptable";
        case 407: return "Proxy Authentication Required";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 410: return "Gone";
        case 411: return "Length Required";
        case 412: return "Precondition Failed";
        case 413: return "Content Too Large";
        case 414: return "URI Too Long";
        case 415: return "Unsupported Media Type";
        case 416: return "Range Not Satisfiable";
        case 417: return "Expectation Failed";
        case 418: return "I'm a Teapot";
        case 421: return "Misdirected Request";
        case 422: return "Unprocessable Content";
        case 423: return "Locked";
        case 424: return "Failed Dependency";
        case 425: return "Too Early";
        case 426: return "Upgrade Required";
        case 428: return "Precondition Required";
        case 429: return "Too Many Requests";
        case 431: return "Request Header Fields Too Large";
        case 451: return "Unavailable For Legal Reasons";

        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        case 505: return "HTTP Version Not Supported";
        case 506: return "Variant Also Negotiates";
        case 507: return "Insufficient Storage";
        case 508: return "Loop Detected";
        case 510: return "Not Extended";
        case 511: return "Network Authentication Required";

        default: return {};
    }
}

[[nodiscard]]
constexpr std::string_view httpReasonPhrase(
    HttpStatus status
) noexcept {
    return httpReasonPhrase(httpStatusCode(status));
}

// ============================================================
// HTTP Response
// ============================================================
//
// An owned, binary-safe HTTP response representation.
//
// Responsibilities:
//
// - Store response status and protocol version
// - Manage response headers
// - Own response body bytes
// - Serialize HTTP/1.0 and HTTP/1.1 responses
// - Enforce basic response framing rules
//
// Limitations:
//
// - Buffered responses only
// - No chunked transfer encoding
// - No streaming bodies
// - No HTTP/2 or HTTP/3 framing
//
// The HTTP connection layer is responsible for handling
// HEAD requests, connection persistence, and protocol
// upgrades.
//
// ============================================================

class HttpResponse final {
public:
    using Body = std::vector<std::byte>;

    HttpResponse() = default;

    explicit HttpResponse(
        HttpStatus status,
        HttpVersion version = {}
    )
        : HttpResponse(
            httpStatusCode(status),
            version
        ) {}

    explicit HttpResponse(
        std::uint16_t status,
        HttpVersion version = {}
    )
        : status_(status),
          version_(version) {
        validateStatus(status_);
        validateVersion(version_);
    }

    // ========================================================
    // Status
    // ========================================================

    [[nodiscard]]
    std::uint16_t statusCode() const noexcept {
        return status_;
    }

    void setStatus(HttpStatus status) {
        setStatus(httpStatusCode(status));
    }

    void setStatus(std::uint16_t status) {
        validateStatus(status);
        status_ = status;
    }

    [[nodiscard]]
    std::string_view reasonPhrase() const noexcept {
        return httpReasonPhrase(status_);
    }

    [[nodiscard]]
    bool isSuccessful() const noexcept {
        return isSuccessfulHttpStatus(status_);
    }

    [[nodiscard]]
    bool isClientError() const noexcept {
        return isClientErrorHttpStatus(status_);
    }

    [[nodiscard]]
    bool isServerError() const noexcept {
        return isServerErrorHttpStatus(status_);
    }

    // ========================================================
    // HTTP Version
    // ========================================================

    [[nodiscard]]
    HttpVersion version() const noexcept {
        return version_;
    }

    void setVersion(HttpVersion version) {
        validateVersion(version);
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

    [[nodiscard]]
    std::optional<std::string_view> header(
        std::string_view name
    ) const noexcept {
        return headers_.get(name);
    }

    // ========================================================
    // Body
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

    void setBody(std::string_view text) {
        Body result;
        result.reserve(text.size());

        for (const unsigned char character : text) {
            result.push_back(
                static_cast<std::byte>(character)
            );
        }

        body_ = std::move(result);
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
    // Body Eligibility
    // ========================================================
    //
    // Certain HTTP statuses cannot carry response content.
    //
    // 1xx, 204, and 304 terminate at the header section.
    //
    // HEAD responses require request-method context and
    // are therefore handled by the connection layer.
    //
    // ========================================================

    [[nodiscard]]
    bool allowsBody() const noexcept {
        return !isInformationalHttpStatus(status_) &&
               status_ != 204 &&
               status_ != 304;
    }

    // ========================================================
    // Serialization
    // ========================================================
    //
    // Produces a complete buffered HTTP response.
    //
    // - Uses CRLF line endings.
    // - Generates Content-Length automatically.
    // - Rejects Transfer-Encoding, which is not supported.
    // - Rejects conflicting Content-Length values.
    // - Suppresses content for statuses that forbid it.
    //
    // Caller must handle partial socket writes.
    //
    // ========================================================

    [[nodiscard]]
    std::string serialize() const {
        validateStatus(status_);
        validateVersion(version_);

        if (headers_.contains("Transfer-Encoding")) {
            throw HttpResponseError(
                "Chunked or other transfer encoding "
                "is not supported by HttpResponse"
            );
        }

        const bool bodyAllowed = allowsBody();

        if (!bodyAllowed && !body_.empty()) {
            throw HttpResponseError(
                "Response status does not permit a body"
            );
        }

        const std::size_t contentLength =
            bodyAllowed ? body_.size() : 0;

        validateContentLength(contentLength);

        std::string output;

        output.reserve(
            128 +
            estimateHeaderSize() +
            contentLength
        );

        output.append(version_.toString());
        output.push_back(' ');
        output.append(std::to_string(status_));

        const auto phrase = reasonPhrase();

        if (!phrase.empty()) {
            output.push_back(' ');
            output.append(phrase);
        }

        output.append("\r\n");

        for (const auto& header : headers_) {
            if (httpHeaderNameEquals(
                    header.name,
                    "Content-Length"
                )) {
                continue;
            }

            output.append(header.name);
            output.append(": ");
            output.append(header.value);
            output.append("\r\n");
        }

        // A 1xx or 204 response must not contain
        // Content-Length. A 304 response may include a
        // representation length supplied by the application,
        // but this serializer does not infer that length.

        if (bodyAllowed) {
            output.append("Content-Length: ");
            output.append(
                std::to_string(contentLength)
            );
            output.append("\r\n");
        }

        output.append("\r\n");

        if (bodyAllowed) {
            for (const auto byte : body_) {
                output.push_back(
                    static_cast<char>(
                        std::to_integer<unsigned char>(byte)
                    )
                );
            }
        }

        return output;
    }

    // ========================================================
    // Reset
    // ========================================================

    void reset() {
        status_ = httpStatusCode(HttpStatus::Ok);
        version_ = HttpVersion{};
        headers_.clear();
        body_.clear();
    }

private:
    // ========================================================
    // Validation
    // ========================================================

    static void validateStatus(
        std::uint16_t status
    ) {
        if (!isValidHttpStatusCode(status)) {
            throw HttpResponseError(
                "Invalid HTTP response status code"
            );
        }
    }

    static void validateVersion(
        HttpVersion version
    ) {
        if (!version.isSupported()) {
            throw HttpResponseError(
                "Unsupported HTTP response version"
            );
        }
    }

    // ========================================================
    // Content-Length Validation
    // ========================================================

    void validateContentLength(
        std::size_t expected
    ) const {
        const auto values =
            headers_.values("Content-Length");

        for (const auto value : values) {
            if (value.empty()) {
                throw HttpResponseError(
                    "Invalid Content-Length header"
                );
            }

            std::size_t parsed = 0;

            for (const char character : value) {
                if (character < '0' ||
                    character > '9') {
                    throw HttpResponseError(
                        "Invalid Content-Length header"
                    );
                }

                const auto digit =
                    static_cast<std::size_t>(
                        character - '0'
                    );

                if (parsed >
                    (std::numeric_limits<std::size_t>::max()
                     - digit) / 10) {
                    throw HttpResponseError(
                        "Content-Length exceeds size_t"
                    );
                }

                parsed = parsed * 10 + digit;
            }

            if (parsed != expected) {
                throw HttpResponseError(
                    "Content-Length does not match "
                    "response body size"
                );
            }
        }
    }

    // ========================================================
    // Header Size Estimation
    // ========================================================

    [[nodiscard]]
    std::size_t estimateHeaderSize() const noexcept {
        std::size_t size = 0;

        for (const auto& header : headers_) {
            size += header.name.size();
            size += header.value.size();
            size += 4;
        }

        return size;
    }

    std::uint16_t status_ =
        httpStatusCode(HttpStatus::Ok);

    HttpVersion version_{};
    HttpHeaders headers_;
    Body body_;
};

// ============================================================
// Compile-Time Contract Checks
// ============================================================

static_assert(
    httpStatusCode(HttpStatus::Ok) == 200
);

static_assert(
    httpStatusCode(HttpStatus::NotFound) == 404
);

static_assert(
    httpReasonPhrase(HttpStatus::Ok) == "OK"
);

static_assert(
    isSuccessfulHttpStatus(201)
);

static_assert(
    isClientErrorHttpStatus(404)
);

static_assert(
    isServerErrorHttpStatus(500)
);

static_assert(
    !isValidHttpStatusCode(99)
);

static_assert(
    !isValidHttpStatusCode(600)
);

} // namespace lamp::http
