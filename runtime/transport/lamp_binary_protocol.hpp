#pragma once

// ============================================================
// Lamp Framework
// Native Binary Transport Layer (`LMP1` + `LCB1`)
//
// File: runtime/transport/lamp_binary_protocol.hpp
//
// Purpose:
//   Implements Lamp's non-REST multiplexed binary frame wire
//   protocol (LMP1), compact string-deduplicated binary codec
//   with native file attachments (LCB1), and RFC 6455
//   WebSocket binary frame wrapping for browser clients.
// ============================================================

#include "security/crypto_utils.hpp"
#include "serialization/json_codec.hpp"
#include "serialization/protobuf_wire.hpp"
#include "serialization/serialization_value.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lamp::transport {

inline constexpr std::size_t kLmpHeaderBytes = 14;
inline constexpr std::size_t kMaxLmpPayloadBytes = 64U * 1024U * 1024U;

enum class FrameType : std::uint8_t {
    Request = 1,
    Response = 2,
    Error = 3,
    StreamChunk = 4,
    Ping = 5,
    Pong = 6
};

enum class FrameFlags : std::uint8_t {
    None = 0x00,
    EndOfStream = 0x01,
    CacheHit = 0x02,
    HasFiles = 0x04
};

[[nodiscard]]
constexpr std::uint8_t operator|(FrameFlags a, FrameFlags b) noexcept {
    return static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b);
}

enum class WireCodecKind : std::uint16_t {
    LampCompactBinary = 1,
    RawBinary = 2,
    JsonFallback = 3
};

struct LmpFrameHeader final {
    FrameType type = FrameType::Request;
    std::uint8_t flags = 0;
    WireCodecKind codec = WireCodecKind::LampCompactBinary;
    std::uint64_t request_id = 0;
    std::uint32_t payload_size = 0;
};

struct LmpFrame final {
    LmpFrameHeader header;
    std::vector<std::uint8_t> payload;
};

struct LampFilePayload final {
    std::string name;
    std::string mime_type;
    std::vector<std::uint8_t> data;
};

struct DecodedRpcInvocation final {
    std::string operation_id;
    serialization::Value input;
    std::vector<LampFilePayload> files;
};

struct DecodedRpcResult final {
    bool ok = true;
    serialization::Value output;
    std::vector<LampFilePayload> files;
    std::string error_code;
    std::string error_identifier;
    std::string error_message;
    bool retryable = false;
};

// ============================================================
// Big-Endian Wire Helpers & Varints
// ============================================================

namespace detail {

inline void WriteU16BE(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>((v >> 8U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(v & 0xFFU));
}

inline void WriteU32BE(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>((v >> 24U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((v >> 16U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((v >> 8U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(v & 0xFFU));
}

inline void WriteU64BE(std::vector<std::uint8_t>& out, std::uint64_t v) {
    for (int i = 7; i >= 0; --i) {
        out.push_back(static_cast<std::uint8_t>((v >> (i * 8)) & 0xFFULL));
    }
}

[[nodiscard]]
inline std::uint16_t ReadU16BE(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint16_t>(p[0]) << 8U) |
           static_cast<std::uint16_t>(p[1]);
}

[[nodiscard]]
inline std::uint32_t ReadU32BE(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint32_t>(p[0]) << 24U) |
           (static_cast<std::uint32_t>(p[1]) << 16U) |
           (static_cast<std::uint32_t>(p[2]) << 8U) |
           static_cast<std::uint32_t>(p[3]);
}

[[nodiscard]]
inline std::uint64_t ReadU64BE(const std::uint8_t* p) noexcept {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v = (v << 8ULL) | static_cast<std::uint64_t>(p[i]);
    }
    return v;
}

inline void WriteVarint(std::vector<std::uint8_t>& out, std::uint64_t v) {
    while (v >= 0x80ULL) {
        out.push_back(static_cast<std::uint8_t>((v & 0x7FULL) | 0x80ULL));
        v >>= 7U;
    }
    out.push_back(static_cast<std::uint8_t>(v));
}

[[nodiscard]]
inline std::uint64_t ZigZagEncode(std::int64_t n) noexcept {
    return (static_cast<std::uint64_t>(n) << 1U) ^
           static_cast<std::uint64_t>(n >> 63);
}

[[nodiscard]]
inline std::int64_t ZigZagDecode(std::uint64_t n) noexcept {
    return static_cast<std::int64_t>((n >> 1U) ^ (~(n & 1ULL) + 1ULL));
}

inline bool ReadVarint(
    std::span<const std::uint8_t> data,
    std::size_t& pos,
    std::uint64_t& out
) noexcept {
    out = 0;
    unsigned shift = 0;
    while (pos < data.size()) {
        const std::uint8_t byte = data[pos++];
        if (shift >= 64U) {
            return false;
        }
        out |= static_cast<std::uint64_t>(byte & 0x7FU) << shift;
        if ((byte & 0x80U) == 0) {
            return true;
        }
        shift += 7U;
    }
    return false;
}

// ============================================================
// SHA-1 (RFC 3174) for RFC 6455 WebSocket Sec-WebSocket-Accept
// ============================================================

[[nodiscard]]
inline std::array<std::uint8_t, 20> Sha1(std::string_view input) {
    std::uint32_t h0 = 0x67452301U;
    std::uint32_t h1 = 0xEFCDAB89U;
    std::uint32_t h2 = 0x98BADCFEU;
    std::uint32_t h3 = 0x10325476U;
    std::uint32_t h4 = 0xC3D2E1F0U;

    std::vector<std::uint8_t> msg(input.begin(), input.end());
    const std::uint64_t bit_len = static_cast<std::uint64_t>(msg.size()) * 8ULL;
    msg.push_back(0x80U);
    while ((msg.size() % 64U) != 56U) {
        msg.push_back(0x00U);
    }
    for (int i = 7; i >= 0; --i) {
        msg.push_back(static_cast<std::uint8_t>((bit_len >> (i * 8)) & 0xFFULL));
    }

    auto rotl = [](std::uint32_t x, unsigned n) noexcept {
        return (x << n) | (x >> (32U - n));
    };

    for (std::size_t chunk = 0; chunk < msg.size(); chunk += 64U) {
        std::uint32_t w[80]{};
        for (int i = 0; i < 16; ++i) {
            w[i] = ReadU32BE(msg.data() + chunk + static_cast<std::size_t>(i * 4));
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }

        std::uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; ++i) {
            std::uint32_t f = 0, k = 0;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999U;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1U;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCU;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6U;
            }
            const std::uint32_t temp = rotl(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotl(b, 30);
            b = a;
            a = temp;
        }

        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    }

    std::array<std::uint8_t, 20> digest{};
    auto put = [&](std::size_t idx, std::uint32_t val) {
        digest[idx + 0] = static_cast<std::uint8_t>((val >> 24U) & 0xFFU);
        digest[idx + 1] = static_cast<std::uint8_t>((val >> 16U) & 0xFFU);
        digest[idx + 2] = static_cast<std::uint8_t>((val >> 8U) & 0xFFU);
        digest[idx + 3] = static_cast<std::uint8_t>(val & 0xFFU);
    };
    put(0, h0);
    put(4, h1);
    put(8, h2);
    put(12, h3);
    put(16, h4);
    return digest;
}

} // namespace detail

// ============================================================
// LMP1 Frame Serialization & Incremental Parsing
// ============================================================

[[nodiscard]]
inline std::vector<std::uint8_t> EncodeLmpFrame(
    FrameType type,
    std::uint64_t request_id,
    std::span<const std::uint8_t> payload,
    std::uint8_t flags = static_cast<std::uint8_t>(FrameFlags::EndOfStream),
    WireCodecKind codec = WireCodecKind::LampCompactBinary
) {
    std::vector<std::uint8_t> out;
    out.reserve(kLmpHeaderBytes + payload.size());
    out.push_back('L');
    out.push_back('M');
    out.push_back('P');
    out.push_back('1');
    out.push_back(static_cast<std::uint8_t>(
        ((static_cast<std::uint8_t>(codec) & 0x0FU) << 4U) |
        (static_cast<std::uint8_t>(type) & 0x0FU)
    ));
    out.push_back(flags);
    detail::WriteU32BE(out, static_cast<std::uint32_t>(request_id & 0xFFFFFFFFULL));
    detail::WriteU32BE(out, static_cast<std::uint32_t>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

class LmpFrameDecoder final {
public:
    enum class FeedStatus : std::uint8_t {
        NeedMoreData = 0,
        FrameReady = 1,
        ProtocolError = 2
    };

    void Append(std::span<const std::uint8_t> bytes) {
        buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
    }

    [[nodiscard]]
    FeedStatus TryNextFrame(LmpFrame& out) {
        if (buffer_.size() - read_offset_ < kLmpHeaderBytes) {
            Compact();
            return FeedStatus::NeedMoreData;
        }

        const std::uint8_t* hdr = buffer_.data() + read_offset_;
        if (hdr[0] != 'L' || hdr[1] != 'M' || hdr[2] != 'P' || hdr[3] != '1') {
            return FeedStatus::ProtocolError;
        }

        const std::uint32_t payload_size = detail::ReadU32BE(hdr + 10);
        if (payload_size > kMaxLmpPayloadBytes) {
            return FeedStatus::ProtocolError;
        }

        const std::size_t total_frame_bytes = kLmpHeaderBytes + payload_size;
        if (buffer_.size() - read_offset_ < total_frame_bytes) {
            Compact();
            return FeedStatus::NeedMoreData;
        }

        out.header.type = static_cast<FrameType>(hdr[4] & 0x0FU);
        out.header.codec = static_cast<WireCodecKind>((hdr[4] >> 4U) & 0x0FU);
        out.header.flags = hdr[5];
        out.header.request_id = detail::ReadU32BE(hdr + 6);
        out.header.payload_size = payload_size;
        out.payload.assign(
            buffer_.begin() + static_cast<std::ptrdiff_t>(read_offset_ + kLmpHeaderBytes),
            buffer_.begin() + static_cast<std::ptrdiff_t>(read_offset_ + total_frame_bytes)
        );

        read_offset_ += total_frame_bytes;
        if (read_offset_ == buffer_.size()) {
            buffer_.clear();
            read_offset_ = 0;
        } else if (read_offset_ > 65536U) {
            Compact();
        }
        return FeedStatus::FrameReady;
    }

private:
    void Compact() {
        if (read_offset_ > 0) {
            buffer_.erase(
                buffer_.begin(),
                buffer_.begin() + static_cast<std::ptrdiff_t>(read_offset_)
            );
            read_offset_ = 0;
        }
    }

    std::vector<std::uint8_t> buffer_;
    std::size_t read_offset_ = 0;
};

// ============================================================
// LCB1 Compact String-Deduplicated Binary Value & File Codec
// ============================================================

class LcbCodec final {
public:
    enum class Tag : std::uint8_t {
        Null = 0,
        False = 1,
        True = 2,
        Int64 = 3,
        UInt64 = 4,
        Float64 = 5,
        String = 6,
        Bytes = 7,
        Array = 8,
        Object = 9
    };

    [[nodiscard]]
    static std::vector<std::uint8_t> EncodeRpcRequest(
        std::string_view operation_id,
        const serialization::Value& input,
        const std::vector<LampFilePayload>& files = {}
    ) {
        StringTable dict;
        const std::uint64_t op_idx = dict.Intern(operation_id);

        std::vector<std::uint8_t> body;
        detail::WriteVarint(body, op_idx);
        WriteValue(input, dict, body);
        detail::WriteVarint(body, files.size());
        for (const auto& f : files) {
            detail::WriteVarint(body, dict.Intern(f.name));
            detail::WriteVarint(body, dict.Intern(f.mime_type));
            detail::WriteVarint(body, f.data.size());
            body.insert(body.end(), f.data.begin(), f.data.end());
        }

        return FinalizeContainer(dict, body);
    }

    [[nodiscard]]
    static bool DecodeRpcRequest(
        std::span<const std::uint8_t> bytes,
        DecodedRpcInvocation& out
    ) {
        std::vector<std::string> dict;
        std::size_t pos = 0;
        if (!ParseDictionary(bytes, pos, dict)) {
            return false;
        }

        std::uint64_t op_idx = 0;
        if (!detail::ReadVarint(bytes, pos, op_idx) || op_idx >= dict.size()) {
            return false;
        }
        out.operation_id = dict[static_cast<std::size_t>(op_idx)];

        if (!ReadValue(bytes, pos, dict, out.input, 0)) {
            return false;
        }

        std::uint64_t file_count = 0;
        if (!detail::ReadVarint(bytes, pos, file_count) || file_count > 1024) {
            return false;
        }
        out.files.clear();
        out.files.reserve(static_cast<std::size_t>(file_count));
        for (std::uint64_t i = 0; i < file_count; ++i) {
            std::uint64_t name_idx = 0;
            std::uint64_t mime_idx = 0;
            std::uint64_t data_len = 0;
            if (!detail::ReadVarint(bytes, pos, name_idx) || name_idx >= dict.size() ||
                !detail::ReadVarint(bytes, pos, mime_idx) || mime_idx >= dict.size() ||
                !detail::ReadVarint(bytes, pos, data_len) ||
                data_len > bytes.size() - pos) {
                return false;
            }
            LampFilePayload file;
            file.name = dict[static_cast<std::size_t>(name_idx)];
            file.mime_type = dict[static_cast<std::size_t>(mime_idx)];
            file.data.assign(
                bytes.begin() + static_cast<std::ptrdiff_t>(pos),
                bytes.begin() + static_cast<std::ptrdiff_t>(pos + data_len)
            );
            pos += static_cast<std::size_t>(data_len);
            out.files.push_back(std::move(file));
        }

        return pos == bytes.size();
    }

    [[nodiscard]]
    static std::vector<std::uint8_t> EncodeRpcResponse(
        bool ok,
        const serialization::Value& output,
        std::string_view error_code = {},
        std::string_view error_identifier = {},
        std::string_view error_message = {},
        bool retryable = false,
        const std::vector<LampFilePayload>& files = {}
    ) {
        StringTable dict;
        std::vector<std::uint8_t> body;

        body.push_back(ok ? 1U : 0U);
        if (ok) {
            WriteValue(output, dict, body);
            detail::WriteVarint(body, files.size());
            for (const auto& f : files) {
                detail::WriteVarint(body, dict.Intern(f.name));
                detail::WriteVarint(body, dict.Intern(f.mime_type));
                detail::WriteVarint(body, f.data.size());
                body.insert(body.end(), f.data.begin(), f.data.end());
            }
        } else {
            detail::WriteVarint(body, dict.Intern(error_code));
            detail::WriteVarint(body, dict.Intern(error_identifier));
            detail::WriteVarint(body, dict.Intern(error_message));
            body.push_back(retryable ? 1U : 0U);
        }

        return FinalizeContainer(dict, body);
    }

    [[nodiscard]]
    static std::vector<std::uint8_t> EncodeRpcResponse(
        const DecodedRpcResult& r
    ) {
        return EncodeRpcResponse(
            r.ok,
            r.output,
            r.error_code,
            r.error_identifier,
            r.error_message,
            r.retryable,
            r.files
        );
    }

    [[nodiscard]]
    static bool DecodeRpcResponse(
        std::span<const std::uint8_t> bytes,
        DecodedRpcResult& out
    ) {
        std::vector<std::string> dict;
        std::size_t pos = 0;
        if (!ParseDictionary(bytes, pos, dict) || pos >= bytes.size()) {
            return false;
        }

        out.ok = bytes[pos++] != 0;
        if (out.ok) {
            if (!ReadValue(bytes, pos, dict, out.output, 0)) {
                return false;
            }
            std::uint64_t file_count = 0;
            if (!detail::ReadVarint(bytes, pos, file_count) || file_count > 1024) {
                return false;
            }
            out.files.clear();
            for (std::uint64_t i = 0; i < file_count; ++i) {
                std::uint64_t name_idx = 0, mime_idx = 0, data_len = 0;
                if (!detail::ReadVarint(bytes, pos, name_idx) || name_idx >= dict.size() ||
                    !detail::ReadVarint(bytes, pos, mime_idx) || mime_idx >= dict.size() ||
                    !detail::ReadVarint(bytes, pos, data_len) ||
                    data_len > bytes.size() - pos) {
                    return false;
                }
                LampFilePayload file;
                file.name = dict[static_cast<std::size_t>(name_idx)];
                file.mime_type = dict[static_cast<std::size_t>(mime_idx)];
                file.data.assign(
                    bytes.begin() + static_cast<std::ptrdiff_t>(pos),
                    bytes.begin() + static_cast<std::ptrdiff_t>(pos + data_len)
                );
                pos += static_cast<std::size_t>(data_len);
                out.files.push_back(std::move(file));
            }
        } else {
            std::uint64_t code_idx = 0, id_idx = 0, msg_idx = 0;
            if (!detail::ReadVarint(bytes, pos, code_idx) || code_idx >= dict.size() ||
                !detail::ReadVarint(bytes, pos, id_idx) || id_idx >= dict.size() ||
                !detail::ReadVarint(bytes, pos, msg_idx) || msg_idx >= dict.size() ||
                pos >= bytes.size()) {
                return false;
            }
            out.error_code = dict[static_cast<std::size_t>(code_idx)];
            out.error_identifier = dict[static_cast<std::size_t>(id_idx)];
            out.error_message = dict[static_cast<std::size_t>(msg_idx)];
            out.retryable = bytes[pos++] != 0;
        }
        return pos == bytes.size();
    }

    [[nodiscard]]
    static std::vector<std::uint8_t> EncodeStandaloneValue(
        const serialization::Value& value
    ) {
        StringTable dict;
        std::vector<std::uint8_t> body;
        WriteValue(value, dict, body);
        return FinalizeContainer(dict, body);
    }

    [[nodiscard]]
    static bool DecodeStandaloneValue(
        std::span<const std::uint8_t> bytes,
        serialization::Value& out
    ) {
        std::vector<std::string> dict;
        std::size_t pos = 0;
        if (!ParseDictionary(bytes, pos, dict)) {
            return false;
        }
        return ReadValue(bytes, pos, dict, out, 0) && pos == bytes.size();
    }

private:
    class StringTable final {
    public:
        std::uint64_t Intern(std::string_view s) {
            if (const auto it = lookup_.find(s); it != lookup_.end()) {
                return it->second;
            }
            const std::uint64_t idx = strings_.size();
            strings_.emplace_back(s);
            lookup_.emplace(strings_.back(), idx);
            return idx;
        }

        const std::vector<std::string>& Strings() const noexcept {
            return strings_;
        }

    private:
        struct StringHash {
            using is_transparent = void;
            std::size_t operator()(std::string_view sv) const noexcept {
                return std::hash<std::string_view>{}(sv);
            }
        };
        std::vector<std::string> strings_;
        std::unordered_map<std::string_view, std::uint64_t, StringHash, std::equal_to<>> lookup_;
    };

    static std::vector<std::uint8_t> FinalizeContainer(
        const StringTable& dict,
        const std::vector<std::uint8_t>& body
    ) {
        std::vector<std::uint8_t> out;
        out.reserve(8 + body.size() + dict.Strings().size() * 8U);
        out.push_back('L');
        out.push_back(1);
        detail::WriteVarint(out, dict.Strings().size());
        for (const auto& s : dict.Strings()) {
            detail::WriteVarint(out, s.size());
            out.insert(out.end(), s.begin(), s.end());
        }
        out.insert(out.end(), body.begin(), body.end());
        return out;
    }

    static bool ParseDictionary(
        std::span<const std::uint8_t> bytes,
        std::size_t& pos,
        std::vector<std::string>& dict
    ) {
        if (bytes.size() < 3 || bytes[0] != 'L' || bytes[1] != 1) {
            return false;
        }
        pos = 2;
        std::uint64_t count = 0;
        if (!detail::ReadVarint(bytes, pos, count) || count > 65536U) {
            return false;
        }
        dict.reserve(static_cast<std::size_t>(count));
        for (std::uint64_t i = 0; i < count; ++i) {
            std::uint64_t len = 0;
            if (!detail::ReadVarint(bytes, pos, len) || len > bytes.size() - pos) {
                return false;
            }
            dict.emplace_back(
                reinterpret_cast<const char*>(bytes.data() + pos),
                static_cast<std::size_t>(len)
            );
            pos += static_cast<std::size_t>(len);
        }
        return true;
    }

    static void WriteValue(
        const serialization::Value& v,
        StringTable& dict,
        std::vector<std::uint8_t>& out
    ) {
        using K = serialization::ValueKind;
        switch (v.Kind()) {
            case K::Null:
                out.push_back(static_cast<std::uint8_t>(Tag::Null));
                break;
            case K::Bool:
                out.push_back(static_cast<std::uint8_t>(v.AsBool().value_or(false) ? Tag::True : Tag::False));
                break;
            case K::Int64:
                out.push_back(static_cast<std::uint8_t>(Tag::Int64));
                detail::WriteVarint(out, detail::ZigZagEncode(*v.AsInt64()));
                break;
            case K::UInt64:
                out.push_back(static_cast<std::uint8_t>(Tag::UInt64));
                detail::WriteVarint(out, *v.AsUInt64());
                break;
            case K::Double: {
                out.push_back(static_cast<std::uint8_t>(Tag::Float64));
                std::uint64_t bits = 0;
                const double d = *v.AsDouble();
                std::memcpy(&bits, &d, sizeof(bits));
                detail::WriteU64BE(out, bits);
                break;
            }
            case K::String:
                out.push_back(static_cast<std::uint8_t>(Tag::String));
                detail::WriteVarint(out, dict.Intern(*v.AsString()));
                break;
            case K::Bytes: {
                out.push_back(static_cast<std::uint8_t>(Tag::Bytes));
                const auto* b = v.AsBytes();
                detail::WriteVarint(out, b->size());
                out.insert(out.end(), b->begin(), b->end());
                break;
            }
            case K::Array: {
                out.push_back(static_cast<std::uint8_t>(Tag::Array));
                const auto* arr = v.AsArray();
                detail::WriteVarint(out, arr->size());
                for (const auto& item : *arr) {
                    WriteValue(item, dict, out);
                }
                break;
            }
            case K::Object: {
                out.push_back(static_cast<std::uint8_t>(Tag::Object));
                const auto* obj = v.AsObject();
                detail::WriteVarint(out, obj->size());
                for (const auto& [key, val] : *obj) {
                    detail::WriteVarint(out, dict.Intern(key));
                    WriteValue(val, dict, out);
                }
                break;
            }
        }
    }

    static bool ReadValue(
        std::span<const std::uint8_t> bytes,
        std::size_t& pos,
        const std::vector<std::string>& dict,
        serialization::Value& out,
        std::size_t depth
    ) {
        if (depth > 64 || pos >= bytes.size()) {
            return false;
        }
        const auto tag = static_cast<Tag>(bytes[pos++]);
        switch (tag) {
            case Tag::Null:
                out = serialization::Value::MakeNull();
                return true;
            case Tag::False:
                out = serialization::Value(false);
                return true;
            case Tag::True:
                out = serialization::Value(true);
                return true;
            case Tag::Int64: {
                std::uint64_t zz = 0;
                if (!detail::ReadVarint(bytes, pos, zz)) return false;
                out = serialization::Value(detail::ZigZagDecode(zz));
                return true;
            }
            case Tag::UInt64: {
                std::uint64_t u = 0;
                if (!detail::ReadVarint(bytes, pos, u)) return false;
                out = serialization::Value(u);
                return true;
            }
            case Tag::Float64: {
                if (bytes.size() - pos < 8U) return false;
                const std::uint64_t bits = detail::ReadU64BE(bytes.data() + pos);
                pos += 8U;
                double d = 0.0;
                std::memcpy(&d, &bits, sizeof(d));
                out = serialization::Value(d);
                return true;
            }
            case Tag::String: {
                std::uint64_t idx = 0;
                if (!detail::ReadVarint(bytes, pos, idx) || idx >= dict.size()) return false;
                out = serialization::Value(dict[static_cast<std::size_t>(idx)]);
                return true;
            }
            case Tag::Bytes: {
                std::uint64_t len = 0;
                if (!detail::ReadVarint(bytes, pos, len) || len > bytes.size() - pos) return false;
                serialization::Value::Bytes b(
                    bytes.begin() + static_cast<std::ptrdiff_t>(pos),
                    bytes.begin() + static_cast<std::ptrdiff_t>(pos + len)
                );
                pos += static_cast<std::size_t>(len);
                out = serialization::Value(std::move(b));
                return true;
            }
            case Tag::Array: {
                std::uint64_t count = 0;
                if (!detail::ReadVarint(bytes, pos, count) || count > 1000000U) return false;
                serialization::Value::Array arr;
                arr.reserve(static_cast<std::size_t>(count));
                for (std::uint64_t i = 0; i < count; ++i) {
                    serialization::Value item;
                    if (!ReadValue(bytes, pos, dict, item, depth + 1)) return false;
                    arr.push_back(std::move(item));
                }
                out = serialization::Value(std::move(arr));
                return true;
            }
            case Tag::Object: {
                std::uint64_t count = 0;
                if (!detail::ReadVarint(bytes, pos, count) || count > 1000000U) return false;
                serialization::Value obj = serialization::Value::MakeObject();
                for (std::uint64_t i = 0; i < count; ++i) {
                    std::uint64_t key_idx = 0;
                    if (!detail::ReadVarint(bytes, pos, key_idx) || key_idx >= dict.size()) return false;
                    serialization::Value val;
                    if (!ReadValue(bytes, pos, dict, val, depth + 1)) return false;
                    obj.Set(dict[static_cast<std::size_t>(key_idx)], std::move(val));
                }
                out = std::move(obj);
                return true;
            }
        }
        return false;
    }
};

// ============================================================
// RFC 6455 WebSocket Binary Frame Wrapper (Browser <-> C++ LMP1)
// ============================================================

[[nodiscard]]
inline std::string ComputeWebSocketAcceptKey(std::string_view client_key) {
    static constexpr std::string_view kWsGuid =
        "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string combined;
    combined.reserve(client_key.size() + kWsGuid.size());
    combined.append(client_key).append(kWsGuid);
    const auto sha1 = detail::Sha1(combined);
    return serialization::Base64::Encode(sha1);
}

[[nodiscard]]
inline std::vector<std::uint8_t> EncodeWebSocketBinaryFrame(
    std::span<const std::uint8_t> payload
) {
    std::vector<std::uint8_t> out;
    out.reserve(10 + payload.size());
    out.push_back(0x82U); // FIN=1, opcode=0x2 (binary)
    if (payload.size() <= 125U) {
        out.push_back(static_cast<std::uint8_t>(payload.size()));
    } else if (payload.size() <= 65535U) {
        out.push_back(126U);
        detail::WriteU16BE(out, static_cast<std::uint16_t>(payload.size()));
    } else {
        out.push_back(127U);
        detail::WriteU64BE(out, static_cast<std::uint64_t>(payload.size()));
    }
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

} // namespace lamp::transport
