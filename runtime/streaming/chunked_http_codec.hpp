#pragma once

// ============================================================
// Lamp Framework
// Phase 9 - Streaming and Real-Time
//
// File: runtime/streaming/chunked_http_codec.hpp
//
// Purpose:
//   RFC 9112 HTTP/1.1 chunked transfer-encoding encoder and
//   incremental bounds-checked decoder, plus Server-Sent
//   Events (SSE) frame encoding.
// ============================================================

#include "http/http_headers.hpp"
#include "http/http_request.hpp"
#include "http/http_response.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace lamp::streaming {

class ChunkedHttpEncoder final {
public:
    [[nodiscard]]
    static std::string EncodeHeader(
        http::HttpStatus status,
        const http::HttpHeaders& headers = {},
        http::HttpVersion version = {}
    ) {
        const auto code = http::httpStatusCode(status);
        const auto phrase = http::httpReasonPhrase(code);

        std::string out;
        out.reserve(256);
        out.append(version.toString());
        out.push_back(' ');
        out.append(std::to_string(code));
        if (!phrase.empty()) {
            out.push_back(' ');
            out.append(phrase);
        }
        out.append("\r\n");

        bool hasTransferEncoding = false;
        for (const auto& h : headers) {
            if (http::httpHeaderNameEquals(h.name, "Content-Length")) {
                continue;
            }
            if (http::httpHeaderNameEquals(
                    h.name,
                    "Transfer-Encoding"
                )) {
                hasTransferEncoding = true;
            }
            out.append(h.name);
            out.append(": ");
            out.append(h.value);
            out.append("\r\n");
        }

        if (!hasTransferEncoding) {
            out.append("Transfer-Encoding: chunked\r\n");
        }
        out.append("\r\n");
        return out;
    }

    [[nodiscard]]
    static std::string EncodeChunk(
        std::span<const std::uint8_t> bytes
    ) {
        if (bytes.empty()) {
            return {};
        }

        const std::string hexLen = ToHex(bytes.size());
        std::string out;
        out.reserve(hexLen.size() + 4 + bytes.size());
        out.append(hexLen);
        out.append("\r\n");
        out.append(
            reinterpret_cast<const char*>(bytes.data()),
            bytes.size()
        );
        out.append("\r\n");
        return out;
    }

    [[nodiscard]]
    static std::string EncodeChunk(std::string_view text) {
        return EncodeChunk(
            std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(text.data()),
                text.size()
            )
        );
    }

    [[nodiscard]]
    static std::string EncodeFinalChunk() {
        return "0\r\n\r\n";
    }

private:
    [[nodiscard]]
    static std::string ToHex(std::size_t value) {
        static constexpr char kDigits[] = "0123456789abcdef";
        if (value == 0) {
            return "0";
        }
        std::string rev;
        while (value > 0) {
            rev.push_back(kDigits[value & 0xFU]);
            value >>= 4U;
        }
        std::reverse(rev.begin(), rev.end());
        return rev;
    }
};

class ChunkedHttpDecoder final {
public:
    using ChunkCallback =
        std::function<void(std::span<const std::uint8_t>)>;

    explicit ChunkedHttpDecoder(
        std::size_t max_chunk_bytes = 4U * 1024U * 1024U
    )
        : max_chunk_bytes_(max_chunk_bytes) {}

    struct FeedResult final {
        std::size_t consumed = 0;
        bool completed = false;
        bool failed = false;
        std::string error;
    };

    [[nodiscard]]
    FeedResult Feed(
        std::string_view input,
        const ChunkCallback& on_chunk
    ) {
        FeedResult result;
        if (completed_ || failed_) {
            result.completed = completed_;
            result.failed = failed_;
            return result;
        }

        buffer_.append(input.data(), input.size());
        result.consumed = input.size();

        while (!completed_ && !failed_) {
            if (state_ == State::ChunkSizeLine) {
                const auto crlf = buffer_.find("\r\n");
                if (crlf == std::string::npos) {
                    if (buffer_.size() > 1024) {
                        return Fail("Chunk size line too large", result);
                    }
                    break;
                }

                std::string_view line(buffer_.data(), crlf);
                const auto semi = line.find(';');
                if (semi != std::string_view::npos) {
                    line = line.substr(0, semi);
                }
                while (!line.empty() && line.back() == ' ') {
                    line.remove_suffix(1);
                }

                std::size_t parsedSize = 0;
                if (!ParseHexSize(line, parsedSize)) {
                    return Fail("Invalid chunk size hex", result);
                }
                if (parsedSize > max_chunk_bytes_) {
                    return Fail("Chunk exceeds max_chunk_bytes", result);
                }

                buffer_.erase(0, crlf + 2);
                current_chunk_remaining_ = parsedSize;
                if (parsedSize == 0) {
                    state_ = State::Trailers;
                } else {
                    state_ = State::ChunkData;
                }
            } else if (state_ == State::ChunkData) {
                if (buffer_.size() < current_chunk_remaining_) {
                    if (!buffer_.empty() && on_chunk) {
                        const std::size_t take = buffer_.size();
                        on_chunk(
                            std::span<const std::uint8_t>(
                                reinterpret_cast<const std::uint8_t*>(
                                    buffer_.data()
                                ),
                                take
                            )
                        );
                        current_chunk_remaining_ -= take;
                        buffer_.clear();
                    }
                    break;
                }

                if (current_chunk_remaining_ > 0 && on_chunk) {
                    on_chunk(
                        std::span<const std::uint8_t>(
                            reinterpret_cast<const std::uint8_t*>(
                                buffer_.data()
                            ),
                            current_chunk_remaining_
                        )
                    );
                }
                buffer_.erase(0, current_chunk_remaining_);
                current_chunk_remaining_ = 0;
                state_ = State::ChunkDataCrlf;
            } else if (state_ == State::ChunkDataCrlf) {
                if (buffer_.size() < 2) {
                    break;
                }
                if (buffer_[0] != '\r' || buffer_[1] != '\n') {
                    return Fail("Missing CRLF after chunk data", result);
                }
                buffer_.erase(0, 2);
                state_ = State::ChunkSizeLine;
            } else if (state_ == State::Trailers) {
                const auto crlf = buffer_.find("\r\n");
                if (crlf == std::string::npos) {
                    if (buffer_.size() > 8192) {
                        return Fail("Trailers exceed limit", result);
                    }
                    break;
                }
                if (crlf == 0) {
                    buffer_.erase(0, 2);
                    completed_ = true;
                    state_ = State::Done;
                    break;
                }
                buffer_.erase(0, crlf + 2);
            }
        }

        result.completed = completed_;
        result.failed = failed_;
        return result;
    }

    [[nodiscard]] bool Completed() const noexcept { return completed_; }
    [[nodiscard]] bool Failed() const noexcept { return failed_; }

private:
    enum class State : std::uint8_t {
        ChunkSizeLine,
        ChunkData,
        ChunkDataCrlf,
        Trailers,
        Done
    };

    FeedResult Fail(std::string msg, FeedResult& result) {
        failed_ = true;
        result.failed = true;
        result.error = std::move(msg);
        return result;
    }

    [[nodiscard]]
    static bool ParseHexSize(
        std::string_view sv,
        std::size_t& out
    ) noexcept {
        if (sv.empty()) {
            return false;
        }
        std::size_t val = 0;
        for (const char ch : sv) {
            std::size_t digit = 0;
            if (ch >= '0' && ch <= '9') {
                digit = static_cast<std::size_t>(ch - '0');
            } else if (ch >= 'a' && ch <= 'f') {
                digit = static_cast<std::size_t>(ch - 'a' + 10);
            } else if (ch >= 'A' && ch <= 'F') {
                digit = static_cast<std::size_t>(ch - 'A' + 10);
            } else {
                return false;
            }
            if (val > (std::numeric_limits<std::size_t>::max() - digit) / 16) {
                return false;
            }
            val = val * 16 + digit;
        }
        out = val;
        return true;
    }

    std::size_t max_chunk_bytes_;
    State state_ = State::ChunkSizeLine;
    std::string buffer_;
    std::size_t current_chunk_remaining_ = 0;
    bool completed_ = false;
    bool failed_ = false;
};

struct SseEvent final {
    std::string event;
    std::string data;
    std::string id;
    std::optional<std::uint32_t> retry_ms;
};

class SseCodec final {
public:
    [[nodiscard]]
    static std::string Encode(const SseEvent& ev) {
        std::string out;
        out.reserve(ev.event.size() + ev.data.size() + ev.id.size() + 32);

        if (!ev.id.empty()) {
            out.append("id: ");
            AppendSanitizedSingleLine(ev.id, out);
            out.push_back('\n');
        }

        if (!ev.event.empty()) {
            out.append("event: ");
            AppendSanitizedSingleLine(ev.event, out);
            out.push_back('\n');
        }

        if (ev.retry_ms.has_value()) {
            out.append("retry: ");
            out.append(std::to_string(*ev.retry_ms));
            out.push_back('\n');
        }

        std::size_t pos = 0;
        while (pos <= ev.data.size()) {
            auto nl = ev.data.find('\n', pos);
            std::string_view line = nl == std::string::npos
                ? std::string_view(ev.data).substr(pos)
                : std::string_view(ev.data).substr(pos, nl - pos);
            if (!line.empty() && line.back() == '\r') {
                line.remove_suffix(1);
            }
            out.append("data: ");
            out.append(line.data(), line.size());
            out.push_back('\n');

            if (nl == std::string::npos) {
                break;
            }
            pos = nl + 1;
        }

        out.push_back('\n');
        return out;
    }

private:
    static void AppendSanitizedSingleLine(
        std::string_view sv,
        std::string& out
    ) {
        for (const char ch : sv) {
            if (ch != '\r' && ch != '\n') {
                out.push_back(ch);
            }
        }
    }
};

} // namespace lamp::streaming
