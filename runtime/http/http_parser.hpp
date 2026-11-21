
#pragma once

#include "http/http_request.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lamp::http {

// ============================================================
// HTTP Parser Configuration
// ============================================================

struct HttpParserConfig final {
    std::size_t maxRequestLineSize = 8192;
    std::size_t maxHeaderSize = 32768;
    std::size_t maxHeaderCount = 100;
    std::size_t maxBodySize = 10 * 1024 * 1024;
    std::size_t maxChunkLineSize = 1024;
};

// ============================================================
// HTTP Parser Error
// ============================================================

enum class HttpParseError : std::uint8_t {
    None,
    InvalidRequestLine,
    InvalidMethod,
    InvalidTarget,
    InvalidVersion,
    InvalidHeader,
    InvalidContentLength,
    ConflictingContentLength,
    InvalidTransferEncoding,
    AmbiguousFraming,
    InvalidChunk,
    InvalidTrailer,
    RequestLineTooLarge,
    HeadersTooLarge,
    TooManyHeaders,
    BodyTooLarge,
    UnsupportedExpectation,
    InternalError
};

[[nodiscard]]
constexpr std::string_view httpParseErrorMessage(
    HttpParseError error
) noexcept {
    switch (error) {
        case HttpParseError::None:
            return "No error";
        case HttpParseError::InvalidRequestLine:
            return "Invalid HTTP request line";
        case HttpParseError::InvalidMethod:
            return "Invalid HTTP method";
        case HttpParseError::InvalidTarget:
            return "Invalid HTTP request target";
        case HttpParseError::InvalidVersion:
            return "Invalid or unsupported HTTP version";
        case HttpParseError::InvalidHeader:
            return "Invalid HTTP header";
        case HttpParseError::InvalidContentLength:
            return "Invalid Content-Length";
        case HttpParseError::ConflictingContentLength:
            return "Conflicting Content-Length values";
        case HttpParseError::InvalidTransferEncoding:
            return "Unsupported or invalid Transfer-Encoding";
        case HttpParseError::AmbiguousFraming:
            return "Ambiguous HTTP message framing";
        case HttpParseError::InvalidChunk:
            return "Invalid chunked body";
        case HttpParseError::InvalidTrailer:
            return "Invalid HTTP trailer";
        case HttpParseError::RequestLineTooLarge:
            return "HTTP request line exceeds limit";
        case HttpParseError::HeadersTooLarge:
            return "HTTP headers exceed limit";
        case HttpParseError::TooManyHeaders:
            return "Too many HTTP headers";
        case HttpParseError::BodyTooLarge:
            return "HTTP request body exceeds limit";
        case HttpParseError::UnsupportedExpectation:
            return "Unsupported Expect header";
        case HttpParseError::InternalError:
            return "Internal HTTP parser error";
    }

    return "Unknown HTTP parser error";
}

// ============================================================
// Incremental Parser Result
// ============================================================
//
// consumed:
//   Number of input bytes accepted by this invocation.
//
// completed:
//   True when a complete request is available via takeRequest.
//
// error:
//   Non-None when the parser enters its failed state.
//
// The parser stops consuming at the end of one complete
// message. The caller must feed any remaining bytes after
// retrieving the completed request.
//
// ============================================================

struct HttpParseResult final {
    std::size_t consumed = 0;
    bool completed = false;
    HttpParseError error = HttpParseError::None;

    [[nodiscard]]
    constexpr bool failed() const noexcept {
        return error != HttpParseError::None;
    }
};

// ============================================================
// HTTP Request Parser
// ============================================================
//
// Design:
//
// - Incremental, state-machine based
// - No socket or platform dependencies
// - One completed request at a time
// - Supports HTTP/1.0 and HTTP/1.1
// - Supports Content-Length and chunked request bodies
// - Rejects ambiguous message framing
// - Enforces configurable resource limits
//
// Security:
//
// - Strict CRLF line endings
// - No obsolete folded headers
// - No whitespace before header colon
// - Rejects conflicting Content-Length
// - Rejects Transfer-Encoding plus Content-Length
// - Rejects unsupported transfer codings
// - Validates chunk framing and trailer syntax
//
// The parser does not decode percent-encoded URLs or
// interpret application-level content.
//
// ============================================================

class HttpParser final {
public:
    explicit HttpParser(HttpParserConfig config = {})
        : config_(config) {
        validateConfig();
    }

    HttpParser(const HttpParser&) = delete;
    HttpParser& operator=(const HttpParser&) = delete;

    HttpParser(HttpParser&&) = default;
    HttpParser& operator=(HttpParser&&) = default;

    // ========================================================
    // Feed Network Data
    // ========================================================

    [[nodiscard]]
    HttpParseResult feed(std::string_view input) {
        HttpParseResult result;

        if (state_ == State::Failed) {
            result.error = error_;
            return result;
        }

        if (state_ == State::Complete) {
            result.completed = true;
            return result;
        }

        try {
            while (result.consumed < input.size()) {
                const auto remaining =
                    input.substr(result.consumed);

                switch (state_) {
                    case State::RequestLine:
                    case State::Headers:
                    case State::ChunkSize:
                    case State::Trailers:
                        consumeLine(remaining, result);
                        break;

                    case State::FixedBody:
                        consumeFixedBody(remaining, result);
                        break;

                    case State::ChunkData:
                        consumeChunkData(remaining, result);
                        break;

                    case State::ChunkDataCR:
                        consumeExpectedByte(
                            remaining,
                            result,
                            '\r',
                            State::ChunkDataLF
                        );
                        break;

                    case State::ChunkDataLF:
                        consumeExpectedByte(
                            remaining,
                            result,
                            '\n',
                            State::ChunkSize
                        );
                        break;

                    case State::Complete:
                    case State::Failed:
                        break;
                }

                if (state_ == State::Complete ||
                    state_ == State::Failed) {
                    break;
                }
            }
        } catch (const std::length_error&) {
            fail(HttpParseError::BodyTooLarge);
        } catch (const std::exception&) {
            fail(HttpParseError::InternalError);
        }

        result.completed = state_ == State::Complete;
        result.error = error_;

        return result;
    }

    // ========================================================
    // Parser State
    // ========================================================

    [[nodiscard]]
    bool hasRequest() const noexcept {
        return state_ == State::Complete;
    }

    [[nodiscard]]
    bool hasFailed() const noexcept {
        return state_ == State::Failed;
    }

    [[nodiscard]]
    HttpParseError error() const noexcept {
        return error_;
    }

    // ========================================================
    // Retrieve Completed Request
    // ========================================================
    //
    // The parser resets after the request is retrieved.
    //
    // ========================================================

    [[nodiscard]]
    HttpRequest takeRequest() {
        if (state_ != State::Complete) {
            throw std::logic_error(
                "No completed HTTP request available"
            );
        }

        HttpRequest completed = std::move(request_);
        reset();
        return completed;
    }

    // ========================================================
    // Reset
    // ========================================================

    void reset() {
        request_.reset();

        state_ = State::RequestLine;
        error_ = HttpParseError::None;

        lineBuffer_.clear();
        headerBytes_ = 0;
        headerCount_ = 0;

        remainingBodyBytes_ = 0;
        remainingChunkBytes_ = 0;

        contentLength_.reset();
        chunked_ = false;
    }

    [[nodiscard]]
    const HttpParserConfig& config() const noexcept {
        return config_;
    }

private:
    enum class State : std::uint8_t {
        RequestLine,
        Headers,
        FixedBody,
        ChunkSize,
        ChunkData,
        ChunkDataCR,
        ChunkDataLF,
        Trailers,
        Complete,
        Failed
    };

    // ========================================================
    // Configuration
    // ========================================================

    void validateConfig() const {
        if (config_.maxRequestLineSize == 0 ||
            config_.maxHeaderSize == 0 ||
            config_.maxHeaderCount == 0 ||
            config_.maxChunkLineSize == 0) {
            throw std::invalid_argument(
                "HTTP parser limits must be positive"
            );
        }
    }

    // ========================================================
    // Failure Handling
    // ========================================================

    void fail(HttpParseError error) noexcept {
        error_ = error;
        state_ = State::Failed;
    }

    // ========================================================
    // Incremental Line Consumption
    // ========================================================
    //
    // Reads one CRLF-terminated line without consuming bytes
    // belonging to the next parser state.
    //
    // ========================================================

    void consumeLine(
        std::string_view input,
        HttpParseResult& result
    ) {
        const auto currentState = state_;

        std::size_t limit = config_.maxHeaderSize;

        if (currentState == State::RequestLine) {
            limit = config_.maxRequestLineSize;
        } else if (currentState == State::ChunkSize) {
            limit = config_.maxChunkLineSize;
        }

        std::size_t index = 0;

        while (index < input.size()) {
            const char character = input[index];

            ++index;
            ++result.consumed;

            if (character == '\n') {
                if (lineBuffer_.empty() ||
                    lineBuffer_.back() != '\r') {
                    fail(lineError(currentState));
                    return;
                }

                lineBuffer_.pop_back();

                const std::string line =
                    std::move(lineBuffer_);

                lineBuffer_.clear();

                processLine(line, currentState);
                return;
            }

            if (character == '\r') {
                lineBuffer_.push_back(character);
            } else {
                if (!lineBuffer_.empty() &&
                    lineBuffer_.back() == '\r') {
                    fail(lineError(currentState));
                    return;
                }

                lineBuffer_.push_back(character);
            }

            if (lineBuffer_.size() > limit + 1) {
                fail(limitError(currentState));
                return;
            }
        }
    }

    [[nodiscard]]
    static constexpr HttpParseError lineError(
        State state
    ) noexcept {
        switch (state) {
            case State::RequestLine:
                return HttpParseError::InvalidRequestLine;
            case State::Headers:
                return HttpParseError::InvalidHeader;
            case State::ChunkSize:
                return HttpParseError::InvalidChunk;
            case State::Trailers:
                return HttpParseError::InvalidTrailer;
            default:
                return HttpParseError::InternalError;
        }
    }

    [[nodiscard]]
    static constexpr HttpParseError limitError(
        State state
    ) noexcept {
        switch (state) {
            case State::RequestLine:
                return HttpParseError::RequestLineTooLarge;
            case State::Headers:
            case State::Trailers:
                return HttpParseError::HeadersTooLarge;
            case State::ChunkSize:
                return HttpParseError::InvalidChunk;
            default:
                return HttpParseError::InternalError;
        }
    }

    // ========================================================
    // Process Completed Line
    // ========================================================

    void processLine(
        const std::string& line,
        State state
    ) {
        switch (state) {
            case State::RequestLine:
                parseRequestLine(line);
                break;

            case State::Headers:
                parseHeaderLine(line);
                break;

            case State::ChunkSize:
                parseChunkSize(line);
                break;

            case State::Trailers:
                parseTrailerLine(line);
                break;

            default:
                fail(HttpParseError::InternalError);
                break;
        }
    }

    // ========================================================
    // Request Line
    // ========================================================

    void parseRequestLine(
        std::string_view line
    ) {
        const auto firstSpace = line.find(' ');

        if (firstSpace == std::string_view::npos) {
            fail(HttpParseError::InvalidRequestLine);
            return;
        }

        const auto secondSpace =
            line.find(' ', firstSpace + 1);

        if (secondSpace == std::string_view::npos ||
            line.find(' ', secondSpace + 1) !=
                std::string_view::npos) {
            fail(HttpParseError::InvalidRequestLine);
            return;
        }

        const auto method =
            line.substr(0, firstSpace);

        const auto target =
            line.substr(
                firstSpace + 1,
                secondSpace - firstSpace - 1
            );

        const auto version =
            line.substr(secondSpace + 1);

        if (!parseHttpMethodToken(method).has_value()) {
            fail(HttpParseError::InvalidMethod);
            return;
        }

        if (target.empty()) {
            fail(HttpParseError::InvalidTarget);
            return;
        }

        HttpVersion parsedVersion;

        if (version == "HTTP/1.1") {
            parsedVersion = {1, 1};
        } else if (version == "HTTP/1.0") {
            parsedVersion = {1, 0};
        } else {
            fail(HttpParseError::InvalidVersion);
            return;
        }

        try {
            request_ = HttpRequest(
                std::string(method),
                std::string(target),
                parsedVersion
            );
        } catch (const HttpRequestError&) {
            fail(HttpParseError::InvalidTarget);
            return;
        }

        state_ = State::Headers;
    }

    // ========================================================
    // Header Lines
    // ========================================================

    void parseHeaderLine(
        std::string_view line
    ) {
        headerBytes_ += line.size() + 2;

        if (headerBytes_ > config_.maxHeaderSize) {
            fail(HttpParseError::HeadersTooLarge);
            return;
        }

        if (line.empty()) {
            finishHeaders();
            return;
        }

        if (++headerCount_ > config_.maxHeaderCount) {
            fail(HttpParseError::TooManyHeaders);
            return;
        }

        const auto separator = line.find(':');

        if (separator == std::string_view::npos) {
            fail(HttpParseError::InvalidHeader);
            return;
        }

        const auto name = line.substr(0, separator);

        if (!isValidHttpHeaderName(name)) {
            fail(HttpParseError::InvalidHeader);
            return;
        }

        auto value = line.substr(separator + 1);
        value = trimWhitespace(value);

        if (!isValidHttpHeaderValue(value)) {
            fail(HttpParseError::InvalidHeader);
            return;
        }

        request_.appendHeader(
            std::string(name),
            std::string(value)
        );
    }

    // ========================================================
    // Header Finalization
    // ========================================================

    void finishHeaders() {
        const auto transferEncodings =
            request_.headers().values("Transfer-Encoding");

        const auto contentLengths =
            request_.headers().values("Content-Length");

        if (!transferEncodings.empty() &&
            !contentLengths.empty()) {
            fail(HttpParseError::AmbiguousFraming);
            return;
        }

        if (!transferEncodings.empty()) {
            if (request_.version().isHttp10()) {
                fail(HttpParseError::InvalidTransferEncoding);
                return;
            }

            if (transferEncodings.size() != 1 ||
                !asciiEqualsIgnoreCase(
                    trimWhitespace(transferEncodings.front()),
                    "chunked"
                )) {
                fail(HttpParseError::InvalidTransferEncoding);
                return;
            }

            chunked_ = true;
        }

        if (!contentLengths.empty()) {
            std::optional<std::size_t> parsedLength;

            for (const auto value : contentLengths) {
                const auto parsed = parseDecimal(value);

                if (!parsed.has_value()) {
                    fail(HttpParseError::InvalidContentLength);
                    return;
                }

                if (parsedLength.has_value() &&
                    *parsedLength != *parsed) {
                    fail(
                        HttpParseError::ConflictingContentLength
                    );
                    return;
                }

                parsedLength = parsed;
            }

            contentLength_ = parsedLength;
        }

        const auto expectations =
            request_.headers().values("Expect");

        for (const auto expectation : expectations) {
            if (!asciiEqualsIgnoreCase(
                    trimWhitespace(expectation),
                    "100-continue"
                )) {
                fail(HttpParseError::UnsupportedExpectation);
                return;
            }
        }

        if (chunked_) {
            state_ = State::ChunkSize;
            return;
        }

        if (contentLength_.has_value()) {
            if (*contentLength_ > config_.maxBodySize) {
                fail(HttpParseError::BodyTooLarge);
                return;
            }

            remainingBodyBytes_ = *contentLength_;

            state_ = remainingBodyBytes_ == 0
                ? State::Complete
                : State::FixedBody;

            return;
        }

        state_ = State::Complete;
    }

    // ========================================================
    // Fixed-Length Body
    // ========================================================

    void consumeFixedBody(
        std::string_view input,
        HttpParseResult& result
    ) {
        const auto amount =
            std::min(
                remainingBodyBytes_,
                input.size()
            );

        appendBody(input.substr(0, amount));

        result.consumed += amount;
        remainingBodyBytes_ -= amount;

        if (remainingBodyBytes_ == 0) {
            state_ = State::Complete;
        }
    }

    // ========================================================
    // Chunk Size
    // ========================================================

    void parseChunkSize(
        std::string_view line
    ) {
        const auto extension = line.find(';');

        const auto sizeText =
            line.substr(0, extension);

        if (sizeText.empty()) {
            fail(HttpParseError::InvalidChunk);
            return;
        }

        std::size_t chunkSize = 0;

        const auto conversion = std::from_chars(
            sizeText.data(),
            sizeText.data() + sizeText.size(),
            chunkSize,
            16
        );

        if (conversion.ec != std::errc{} ||
            conversion.ptr !=
                sizeText.data() + sizeText.size()) {
            fail(HttpParseError::InvalidChunk);
            return;
        }

        if (chunkSize >
            config_.maxBodySize - request_.bodySize()) {
            fail(HttpParseError::BodyTooLarge);
            return;
        }

        remainingChunkBytes_ = chunkSize;

        if (chunkSize == 0) {
            state_ = State::Trailers;
        } else {
            state_ = State::ChunkData;
        }
    }

    // ========================================================
    // Chunk Data
    // ========================================================

    void consumeChunkData(
        std::string_view input,
        HttpParseResult& result
    ) {
        const auto amount =
            std::min(
                remainingChunkBytes_,
                input.size()
            );

        appendBody(input.substr(0, amount));

        result.consumed += amount;
        remainingChunkBytes_ -= amount;

        if (remainingChunkBytes_ == 0) {
            state_ = State::ChunkDataCR;
        }
    }

    // ========================================================
    // Chunk Delimiter
    // ========================================================

    void consumeExpectedByte(
        std::string_view input,
        HttpParseResult& result,
        char expected,
        State nextState
    ) {
        if (input.empty()) {
            return;
        }

        ++result.consumed;

        if (input.front() != expected) {
            fail(HttpParseError::InvalidChunk);
            return;
        }

        state_ = nextState;
    }

    // ========================================================
    // Trailer Lines
    // ========================================================
    //
    // Trailers are validated but not exposed to applications
    // in this initial request model.
    //
    // ========================================================

    void parseTrailerLine(
        std::string_view line
    ) {
        headerBytes_ += line.size() + 2;

        if (headerBytes_ > config_.maxHeaderSize) {
            fail(HttpParseError::HeadersTooLarge);
            return;
        }

        if (line.empty()) {
            state_ = State::Complete;
            return;
        }

        if (++headerCount_ > config_.maxHeaderCount) {
            fail(HttpParseError::TooManyHeaders);
            return;
        }

        const auto separator = line.find(':');

        if (separator == std::string_view::npos) {
            fail(HttpParseError::InvalidTrailer);
            return;
        }

        const auto name = line.substr(0, separator);
        const auto value =
            trimWhitespace(line.substr(separator + 1));

        if (!isValidHttpHeaderName(name) ||
            !isValidHttpHeaderValue(value)) {
            fail(HttpParseError::InvalidTrailer);
            return;
        }

        // Framing-related fields must not be accepted
        // in trailers.

        if (httpHeaderNameEquals(name, "Content-Length") ||
            httpHeaderNameEquals(name, "Transfer-Encoding") ||
            httpHeaderNameEquals(name, "Host")) {
            fail(HttpParseError::InvalidTrailer);
        }
    }

    // ========================================================
    // Body Append
    // ========================================================

    void appendBody(std::string_view bytes) {
        if (bytes.size() >
            config_.maxBodySize - request_.bodySize()) {
            fail(HttpParseError::BodyTooLarge);
            return;
        }

        auto& body = request_.body();

        body.reserve(body.size() + bytes.size());

        for (const unsigned char character : bytes) {
            body.push_back(
                static_cast<std::byte>(character)
            );
        }
    }

    // ========================================================
    // Helpers
    // ========================================================

    [[nodiscard]]
    static constexpr std::string_view trimWhitespace(
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
    static constexpr bool asciiEqualsIgnoreCase(
        std::string_view left,
        std::string_view right
    ) noexcept {
        if (left.size() != right.size()) {
            return false;
        }

        for (std::size_t i = 0; i < left.size(); ++i) {
            auto a = static_cast<unsigned char>(left[i]);
            auto b = static_cast<unsigned char>(right[i]);

            if (a >= 'A' && a <= 'Z') {
                a = static_cast<unsigned char>(a + 32);
            }

            if (b >= 'A' && b <= 'Z') {
                b = static_cast<unsigned char>(b + 32);
            }

            if (a != b) {
                return false;
            }
        }

        return true;
    }

    [[nodiscard]]
    static std::optional<std::size_t> parseDecimal(
        std::string_view text
    ) noexcept {
        if (text.empty()) {
            return std::nullopt;
        }

        std::size_t result = 0;

        for (const char character : text) {
            if (character < '0' || character > '9') {
                return std::nullopt;
            }

            const auto digit =
                static_cast<std::size_t>(character - '0');

            if (result >
                (std::numeric_limits<std::size_t>::max()
                 - digit) / 10) {
                return std::nullopt;
            }

            result = result * 10 + digit;
        }

        return result;
    }

    State state_ = State::RequestLine;

    HttpParseError error_ = HttpParseError::None;

    HttpParserConfig config_;
    HttpRequest request_;

    std::string lineBuffer_;

    std::size_t headerBytes_ = 0;
    std::size_t headerCount_ = 0;

    std::size_t remainingBodyBytes_ = 0;
    std::size_t remainingChunkBytes_ = 0;

    std::optional<std::size_t> contentLength_;

    bool chunked_ = false;
};

} // namespace lamp::http
