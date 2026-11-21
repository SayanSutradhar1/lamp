
#include "http/http_parser.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using lamp::http::HttpParseError;
using lamp::http::HttpParser;
using lamp::http::HttpParserConfig;
using lamp::http::HttpRequest;
using lamp::http::HttpMethod;

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
// Request Helpers
// ============================================================

[[nodiscard]]
std::string makeRequest(
    std::string_view method = "GET",
    std::string_view target = "/",
    std::string_view headers = {},
    std::string_view body = {}
) {
    std::string request;

    request += method;
    request += " ";
    request += target;
    request += " HTTP/1.1\r\n";

    request += "Host: localhost\r\n";

    request += headers;

    request += "\r\n";
    request += body;

    return request;
}

[[nodiscard]]
std::string bodyAsString(
    const HttpRequest& request
) {
    std::string result;
    result.reserve(request.bodySize());

    for (const auto byte : request.body()) {
        result.push_back(
            static_cast<char>(
                std::to_integer<unsigned char>(byte)
            )
        );
    }

    return result;
}

void expectError(
    std::string_view input,
    HttpParseError expected,
    HttpParserConfig config = {}
) {
    HttpParser parser(config);

    const auto result = parser.feed(input);

    require(
        result.failed(),
        "Expected parser to reject the request"
    );

    requireEqual(
        result.error,
        expected,
        "Unexpected HTTP parser error"
    );

    require(
        parser.hasFailed(),
        "Parser must enter Failed state"
    );
}

[[nodiscard]]
HttpRequest parseComplete(
    std::string_view input,
    HttpParserConfig config = {}
) {
    HttpParser parser(config);

    const auto result = parser.feed(input);

    require(
        !result.failed(),
        "Parser unexpectedly rejected request"
    );

    require(
        result.completed,
        "Expected complete HTTP request"
    );

    requireEqual(
        result.consumed,
        input.size(),
        "Parser consumed unexpected number of bytes"
    );

    return parser.takeRequest();
}

// ============================================================
// Test 1: Basic GET
// ============================================================

LAMP_TEST(parses_basic_get_request) {
    const auto request = parseComplete(
        makeRequest("GET", "/hello")
    );

    requireEqual(
        request.method(),
        HttpMethod::Get,
        "Incorrect HTTP method"
    );

    requireEqual(
        request.path(),
        std::string("/hello"),
        "Incorrect request path"
    );

    require(
        request.version().isHttp11(),
        "Expected HTTP/1.1"
    );

    require(
        !request.hasBody(),
        "GET request should have no body"
    );
}

// ============================================================
// Test 2: HTTP/1.0
// ============================================================

LAMP_TEST(parses_http_10_request) {
    const auto request = parseComplete(
        "GET /legacy HTTP/1.0\r\n"
        "\r\n"
    );

    require(
        request.version().isHttp10(),
        "Expected HTTP/1.0"
    );

    requireEqual(
        request.path(),
        std::string("/legacy"),
        "HTTP/1.0 path mismatch"
    );
}

// ============================================================
// Test 3: Query String
// ============================================================

LAMP_TEST(preserves_query_string) {
    const auto request = parseComplete(
        makeRequest(
            "GET",
            "/search?q=lamp&page=2"
        )
    );

    requireEqual(
        request.path(),
        std::string("/search"),
        "Query path mismatch"
    );

    requireEqual(
        request.query(),
        std::string("q=lamp&page=2"),
        "Query string mismatch"
    );
}

// ============================================================
// Test 4: Header Lookup
// ============================================================

LAMP_TEST(header_lookup_is_case_insensitive) {
    const auto request = parseComplete(
        makeRequest(
            "GET",
            "/",
            "X-Test-Header: Lamp\r\n"
        )
    );

    const auto value =
        request.header("x-test-header");

    require(
        value.has_value(),
        "Expected custom header"
    );

    requireEqual(
        *value,
        std::string_view("Lamp"),
        "Header value mismatch"
    );
}

// ============================================================
// Test 5: Header Whitespace
// ============================================================

LAMP_TEST(trims_header_optional_whitespace) {
    const auto request = parseComplete(
        makeRequest(
            "GET",
            "/",
            "X-Value: \t hello world \t\r\n"
        )
    );

    const auto value =
        request.header("X-Value");

    require(
        value.has_value(),
        "Expected header value"
    );

    requireEqual(
        *value,
        std::string_view("hello world"),
        "Header whitespace was not trimmed"
    );
}

// ============================================================
// Test 6: Content-Length Body
// ============================================================

LAMP_TEST(parses_content_length_body) {
    const auto request = parseComplete(
        makeRequest(
            "POST",
            "/echo",
            "Content-Length: 5\r\n",
            "hello"
        )
    );

    requireEqual(
        request.method(),
        HttpMethod::Post,
        "Expected POST method"
    );

    requireEqual(
        bodyAsString(request),
        std::string("hello"),
        "Request body mismatch"
    );
}

// ============================================================
// Test 7: Binary Body
// ============================================================

LAMP_TEST(preserves_binary_request_body) {
    const std::string body{
        '\0',
        '\x01',
        '\x7F',
        static_cast<char>(0xFF)
    };

    const auto request = parseComplete(
        makeRequest(
            "POST",
            "/binary",
            "Content-Length: 4\r\n",
            body
        )
    );

    requireEqual(
        bodyAsString(request),
        body,
        "Binary body was modified"
    );
}

// ============================================================
// Test 8: Empty Content-Length
// ============================================================

LAMP_TEST(accepts_zero_content_length) {
    const auto request = parseComplete(
        makeRequest(
            "POST",
            "/empty",
            "Content-Length: 0\r\n"
        )
    );

    require(
        request.bodySize() == 0,
        "Zero-length body must be empty"
    );
}

// ============================================================
// Test 9: Fragmented Request Line
// ============================================================

LAMP_TEST(parses_fragmented_request_line) {
    HttpParser parser;

    const auto first = parser.feed("GE");

    require(
        !first.completed && !first.failed(),
        "Partial method should remain incomplete"
    );

    const auto second = parser.feed(
        "T /hello HTTP/1.1\r"
    );

    require(
        !second.completed && !second.failed(),
        "Partial request line should remain incomplete"
    );

    const auto third = parser.feed(
        "\nHost: localhost\r\n\r\n"
    );

    require(
        third.completed,
        "Fragmented request should complete"
    );

    const auto request = parser.takeRequest();

    requireEqual(
        request.method(),
        HttpMethod::Get,
        "Fragmented method mismatch"
    );
}

// ============================================================
// Test 10: Fragmented Headers
// ============================================================

LAMP_TEST(parses_fragmented_headers) {
    HttpParser parser;

    const auto first = parser.feed(
        "GET / HTTP/1.1\r\n"
        "Host: local"
    );

    require(
        !first.completed && !first.failed(),
        "Headers should remain incomplete"
    );

    const auto second = parser.feed(
        "host\r\n"
        "X-Trace: 123\r\n"
        "\r\n"
    );

    require(
        second.completed,
        "Fragmented headers should complete"
    );

    const auto request = parser.takeRequest();

    requireEqual(
        request.header("X-Trace").value_or(""),
        std::string_view("123"),
        "Fragmented header mismatch"
    );
}

// ============================================================
// Test 11: Fragmented Body
// ============================================================

LAMP_TEST(parses_fragmented_body) {
    HttpParser parser;

    const auto first = parser.feed(
        makeRequest(
            "POST",
            "/upload",
            "Content-Length: 11\r\n",
            "hello"
        )
    );

    require(
        !first.completed && !first.failed(),
        "Partial body should remain incomplete"
    );

    const auto second = parser.feed(" world");

    require(
        second.completed,
        "Body should complete after final fragment"
    );

    const auto request = parser.takeRequest();

    requireEqual(
        bodyAsString(request),
        std::string("hello world"),
        "Fragmented body mismatch"
    );
}

// ============================================================
// Test 12: Byte-by-Byte Input
// ============================================================

LAMP_TEST(parses_one_byte_at_a_time) {
    HttpParser parser;

    const auto input = makeRequest(
        "POST",
        "/slow",
        "Content-Length: 4\r\n",
        "lamp"
    );

    for (std::size_t i = 0; i < input.size(); ++i) {
        const auto result = parser.feed(
            std::string_view(input.data() + i, 1)
        );

        require(
            !result.failed(),
            "Byte-by-byte parsing failed"
        );

        if (i + 1 == input.size()) {
            require(
                result.completed,
                "Final byte should complete request"
            );
        }
    }

    const auto request = parser.takeRequest();

    requireEqual(
        bodyAsString(request),
        std::string("lamp"),
        "Byte-by-byte body mismatch"
    );
}

// ============================================================
// Test 13: Pipelined Requests
// ============================================================

LAMP_TEST(pipelined_requests_preserve_boundaries) {
    HttpParser parser;

    const auto firstRequest =
        makeRequest("GET", "/first");

    const auto secondRequest =
        makeRequest("GET", "/second");

    const auto combined =
        firstRequest + secondRequest;

    const auto first = parser.feed(combined);

    require(
        first.completed,
        "First pipelined request should complete"
    );

    requireEqual(
        first.consumed,
        firstRequest.size(),
        "Parser consumed bytes from second request"
    );

    const auto requestOne = parser.takeRequest();

    requireEqual(
        requestOne.path(),
        std::string("/first"),
        "First pipelined path mismatch"
    );

    const auto second = parser.feed(
        std::string_view(combined).substr(
            first.consumed
        )
    );

    require(
        second.completed,
        "Second pipelined request should complete"
    );

    const auto requestTwo = parser.takeRequest();

    requireEqual(
        requestTwo.path(),
        std::string("/second"),
        "Second pipelined path mismatch"
    );
}

// ============================================================
// Test 14: Chunked Body
// ============================================================

LAMP_TEST(parses_chunked_request_body) {
    const auto request = parseComplete(
        makeRequest(
            "POST",
            "/chunked",
            "Transfer-Encoding: chunked\r\n",
            "5\r\n"
            "hello\r\n"
            "6\r\n"
            " world\r\n"
            "0\r\n"
            "\r\n"
        )
    );

    requireEqual(
        bodyAsString(request),
        std::string("hello world"),
        "Chunked body mismatch"
    );
}

// ============================================================
// Test 15: Fragmented Chunked Body
// ============================================================

LAMP_TEST(parses_fragmented_chunked_body) {
    HttpParser parser;

    const auto first = parser.feed(
        makeRequest(
            "POST",
            "/chunked",
            "Transfer-Encoding: chunked\r\n",
            "4\r\nla"
        )
    );

    require(
        !first.completed && !first.failed(),
        "Chunk should remain incomplete"
    );

    const auto second = parser.feed(
        "mp\r\n0\r\n\r\n"
    );

    require(
        second.completed,
        "Final chunk should complete request"
    );

    const auto request = parser.takeRequest();

    requireEqual(
        bodyAsString(request),
        std::string("lamp"),
        "Fragmented chunked body mismatch"
    );
}

// ============================================================
// Test 16: Chunk Extensions
// ============================================================

LAMP_TEST(accepts_chunk_extensions) {
    const auto request = parseComplete(
        makeRequest(
            "POST",
            "/chunk",
            "Transfer-Encoding: chunked\r\n",
            "4;name=value\r\n"
            "lamp\r\n"
            "0\r\n"
            "\r\n"
        )
    );

    requireEqual(
        bodyAsString(request),
        std::string("lamp"),
        "Chunk extension affected body"
    );
}

// ============================================================
// Test 17: Valid Trailers
// ============================================================

LAMP_TEST(accepts_valid_chunk_trailers) {
    const auto request = parseComplete(
        makeRequest(
            "POST",
            "/trailers",
            "Transfer-Encoding: chunked\r\n",
            "4\r\n"
            "lamp\r\n"
            "0\r\n"
            "X-Checksum: abc123\r\n"
            "\r\n"
        )
    );

    requireEqual(
        bodyAsString(request),
        std::string("lamp"),
        "Trailer parsing changed body"
    );
}

// ============================================================
// Test 18: Invalid Request Line
// ============================================================

LAMP_TEST(rejects_invalid_request_line) {
    expectError(
        "GET /missing-version\r\n",
        HttpParseError::InvalidRequestLine
    );
}

// ============================================================
// Test 19: Invalid Method
// ============================================================

LAMP_TEST(rejects_invalid_method_token) {
    expectError(
        "GE(T / HTTP/1.1\r\n"
        "Host: localhost\r\n\r\n",
        HttpParseError::InvalidMethod
    );
}

// ============================================================
// Test 20: Unsupported Version
// ============================================================

LAMP_TEST(rejects_unsupported_http_version) {
    expectError(
        "GET / HTTP/2.0\r\n"
        "Host: localhost\r\n\r\n",
        HttpParseError::InvalidVersion
    );
}

// ============================================================
// Test 21: Bare LF
// ============================================================

LAMP_TEST(rejects_bare_line_feed) {
    expectError(
        "GET / HTTP/1.1\n",
        HttpParseError::InvalidRequestLine
    );
}

// ============================================================
// Test 22: Invalid Header Name
// ============================================================

LAMP_TEST(rejects_invalid_header_name) {
    expectError(
        makeRequest(
            "GET",
            "/",
            "Bad Header: value\r\n"
        ),
        HttpParseError::InvalidHeader
    );
}

// ============================================================
// Test 23: Whitespace Before Colon
// ============================================================

LAMP_TEST(rejects_whitespace_before_colon) {
    expectError(
        makeRequest(
            "GET",
            "/",
            "X-Test : value\r\n"
        ),
        HttpParseError::InvalidHeader
    );
}

// ============================================================
// Test 24: Obsolete Header Folding
// ============================================================

LAMP_TEST(rejects_folded_headers) {
    expectError(
        makeRequest(
            "GET",
            "/",
            "X-Test: first\r\n"
            " second\r\n"
        ),
        HttpParseError::InvalidHeader
    );
}

// ============================================================
// Test 25: Invalid Content-Length
// ============================================================

LAMP_TEST(rejects_invalid_content_length) {
    expectError(
        makeRequest(
            "POST",
            "/",
            "Content-Length: abc\r\n"
        ),
        HttpParseError::InvalidContentLength
    );
}

// ============================================================
// Test 26: Negative Content-Length
// ============================================================

LAMP_TEST(rejects_negative_content_length) {
    expectError(
        makeRequest(
            "POST",
            "/",
            "Content-Length: -1\r\n"
        ),
        HttpParseError::InvalidContentLength
    );
}

// ============================================================
// Test 27: Content-Length Overflow
// ============================================================

LAMP_TEST(rejects_content_length_overflow) {
    expectError(
        makeRequest(
            "POST",
            "/",
            "Content-Length: "
            "999999999999999999999999999999\r\n"
        ),
        HttpParseError::InvalidContentLength
    );
}

// ============================================================
// Test 28: Conflicting Content-Length
// ============================================================

LAMP_TEST(rejects_conflicting_content_lengths) {
    expectError(
        makeRequest(
            "POST",
            "/",
            "Content-Length: 4\r\n"
            "Content-Length: 5\r\n"
        ),
        HttpParseError::ConflictingContentLength
    );
}

// ============================================================
// Test 29: Matching Content-Length
// ============================================================

LAMP_TEST(accepts_matching_content_lengths) {
    const auto request = parseComplete(
        makeRequest(
            "POST",
            "/",
            "Content-Length: 4\r\n"
            "Content-Length: 4\r\n",
            "lamp"
        )
    );

    requireEqual(
        bodyAsString(request),
        std::string("lamp"),
        "Matching Content-Length body mismatch"
    );
}

// ============================================================
// Test 30: Ambiguous Framing
// ============================================================

LAMP_TEST(rejects_transfer_encoding_with_content_length) {
    expectError(
        makeRequest(
            "POST",
            "/",
            "Transfer-Encoding: chunked\r\n"
            "Content-Length: 4\r\n"
        ),
        HttpParseError::AmbiguousFraming
    );
}

// ============================================================
// Test 31: Unsupported Transfer Coding
// ============================================================

LAMP_TEST(rejects_unsupported_transfer_encoding) {
    expectError(
        makeRequest(
            "POST",
            "/",
            "Transfer-Encoding: gzip\r\n"
        ),
        HttpParseError::InvalidTransferEncoding
    );
}

// ============================================================
// Test 32: Invalid Chunk Size
// ============================================================

LAMP_TEST(rejects_invalid_chunk_size) {
    expectError(
        makeRequest(
            "POST",
            "/",
            "Transfer-Encoding: chunked\r\n",
            "XYZ\r\n"
        ),
        HttpParseError::InvalidChunk
    );
}

// ============================================================
// Test 33: Invalid Chunk Terminator
// ============================================================

LAMP_TEST(rejects_invalid_chunk_terminator) {
    expectError(
        makeRequest(
            "POST",
            "/",
            "Transfer-Encoding: chunked\r\n",
            "4\r\n"
            "lampXX"
        ),
        HttpParseError::InvalidChunk
    );
}

// ============================================================
// Test 34: Forbidden Trailer
// ============================================================

LAMP_TEST(rejects_framing_header_in_trailer) {
    expectError(
        makeRequest(
            "POST",
            "/",
            "Transfer-Encoding: chunked\r\n",
            "0\r\n"
            "Content-Length: 5\r\n"
            "\r\n"
        ),
        HttpParseError::InvalidTrailer
    );
}

// ============================================================
// Test 35: Request Line Limit
// ============================================================

LAMP_TEST(enforces_request_line_limit) {
    HttpParserConfig config;
    config.maxRequestLineSize = 16;

    expectError(
        "GET /this-path-is-too-long HTTP/1.1\r\n",
        HttpParseError::RequestLineTooLarge,
        config
    );
}

// ============================================================
// Test 36: Header Count Limit
// ============================================================

LAMP_TEST(enforces_header_count_limit) {
    HttpParserConfig config;
    config.maxHeaderCount = 2;

    expectError(
        makeRequest(
            "GET",
            "/",
            "X-One: 1\r\n"
            "X-Two: 2\r\n"
        ),
        HttpParseError::TooManyHeaders,
        config
    );
}

// ============================================================
// Test 37: Header Size Limit
// ============================================================

LAMP_TEST(enforces_header_size_limit) {
    HttpParserConfig config;
    config.maxHeaderSize = 24;

    expectError(
        makeRequest(
            "GET",
            "/",
            "X-Long: abcdefghijklmnopqrstuvwxyz\r\n"
        ),
        HttpParseError::HeadersTooLarge,
        config
    );
}

// ============================================================
// Test 38: Body Size Limit
// ============================================================

LAMP_TEST(enforces_content_length_body_limit) {
    HttpParserConfig config;
    config.maxBodySize = 4;

    expectError(
        makeRequest(
            "POST",
            "/",
            "Content-Length: 5\r\n"
        ),
        HttpParseError::BodyTooLarge,
        config
    );
}

// ============================================================
// Test 39: Chunked Body Limit
// ============================================================

LAMP_TEST(enforces_chunked_body_limit) {
    HttpParserConfig config;
    config.maxBodySize = 4;

    expectError(
        makeRequest(
            "POST",
            "/",
            "Transfer-Encoding: chunked\r\n",
            "5\r\n"
        ),
        HttpParseError::BodyTooLarge,
        config
    );
}

// ============================================================
// Test 40: Unsupported Expect
// ============================================================

LAMP_TEST(rejects_unsupported_expectation) {
    expectError(
        makeRequest(
            "POST",
            "/",
            "Expect: custom-expectation\r\n"
        ),
        HttpParseError::UnsupportedExpectation
    );
}

// ============================================================
// Test 41: Failed State Persists
// ============================================================

LAMP_TEST(failed_parser_rejects_subsequent_input) {
    HttpParser parser;

    const auto first = parser.feed(
        "INVALID REQUEST\r\n"
    );

    require(
        first.failed(),
        "Expected initial parsing failure"
    );

    const auto second = parser.feed(
        makeRequest()
    );

    require(
        second.failed(),
        "Failed parser should remain failed"
    );

    requireEqual(
        second.consumed,
        std::size_t{0},
        "Failed parser must not consume input"
    );
}

// ============================================================
// Test 42: Reset After Failure
// ============================================================

LAMP_TEST(reset_recovers_failed_parser) {
    HttpParser parser;

    const auto invalid = parser.feed(
        "BAD REQUEST\r\n"
    );

    require(
        invalid.failed(),
        "Expected parsing failure"
    );

    parser.reset();

    require(
        !parser.hasFailed(),
        "Reset should clear failed state"
    );

    const auto valid = parser.feed(
        makeRequest("GET", "/recovered")
    );

    require(
        valid.completed,
        "Parser should work after reset"
    );

    const auto request = parser.takeRequest();

    requireEqual(
        request.path(),
        std::string("/recovered"),
        "Recovered parser returned wrong request"
    );
}

// ============================================================
// Test 43: Completed State
// ============================================================

LAMP_TEST(completed_parser_waits_for_take_request) {
    HttpParser parser;

    const auto first = parser.feed(
        makeRequest()
    );

    require(
        first.completed,
        "Expected completed request"
    );

    const auto second = parser.feed(
        makeRequest("GET", "/next")
    );

    require(
        second.completed,
        "Completed parser should report completion"
    );

    requireEqual(
        second.consumed,
        std::size_t{0},
        "Completed parser must not consume next request"
    );

    const auto request = parser.takeRequest();

    requireEqual(
        request.path(),
        std::string("/"),
        "Completed request changed unexpectedly"
    );
}

// ============================================================
// Test 44: Take Request Without Completion
// ============================================================

LAMP_TEST(take_request_requires_completion) {
    HttpParser parser;

    bool threw = false;

    try {
        auto request = parser.takeRequest();
        (void)request;
    } catch (const std::logic_error&) {
        threw = true;
    }

    require(
        threw,
        "takeRequest must reject incomplete requests"
    );
}

// ============================================================
// Test 45: Parser Reuse
// ============================================================

LAMP_TEST(parser_can_process_multiple_requests) {
    HttpParser parser;

    for (int i = 0; i < 10; ++i) {
        const auto path =
            "/request/" + std::to_string(i);

        const auto result = parser.feed(
            makeRequest("GET", path)
        );

        require(
            result.completed,
            "Reused parser failed"
        );

        const auto request = parser.takeRequest();

        requireEqual(
            request.path(),
            path,
            "Reused parser path mismatch"
        );
    }
}

// ============================================================
// Test 46: Invalid Configuration
// ============================================================

LAMP_TEST(rejects_invalid_parser_configuration) {
    HttpParserConfig config;
    config.maxHeaderCount = 0;

    bool threw = false;

    try {
        HttpParser parser(config);
        (void)parser;
    } catch (const std::invalid_argument&) {
        threw = true;
    }

    require(
        threw,
        "Invalid parser configuration must throw"
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
        << "Lamp HTTP Parser Tests\n"
        << "========================================\n"
        << "Passed: " << passed << '\n'
        << "Failed: " << failed << '\n'
        << "Total : " << passed + failed << '\n'
        << "========================================\n";

    return failed == 0 ? 0 : 1;
}
