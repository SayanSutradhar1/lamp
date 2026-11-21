#pragma once

// ============================================================
// Lamp Framework
// Phase 10 - Security and Reliability
//
// File: runtime/security/crypto_utils.hpp
//
// Purpose:
//   Self-contained cryptographic primitives (constant-time
//   comparison, SHA-256, HMAC-SHA256, Base64URL) with zero
//   external dependencies.
// ============================================================

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lamp::security {

// ============================================================
// Constant-Time Comparison
// ============================================================

[[nodiscard]]
inline bool ConstantTimeEquals(
    std::span<const std::uint8_t> a,
    std::span<const std::uint8_t> b
) noexcept {
    const std::size_t max_len = a.size() > b.size() ? a.size() : b.size();
    volatile std::uint8_t diff = static_cast<std::uint8_t>(a.size() != b.size());

    for (std::size_t i = 0; i < max_len; ++i) {
        const std::uint8_t av = i < a.size() ? a[i] : 0;
        const std::uint8_t bv = i < b.size() ? b[i] : 0;
        diff = static_cast<std::uint8_t>(diff | static_cast<std::uint8_t>(av ^ bv));
    }

    return diff == 0;
}

[[nodiscard]]
inline bool ConstantTimeEquals(
    std::string_view a,
    std::string_view b
) noexcept {
    const auto a_bytes = std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(a.data()),
        a.size()
    );
    const auto b_bytes = std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(b.data()),
        b.size()
    );
    return ConstantTimeEquals(a_bytes, b_bytes);
}

// ============================================================
// SHA-256 (FIPS 180-4)
// ============================================================

namespace detail {

[[nodiscard]]
constexpr std::uint32_t RotR32(std::uint32_t x, unsigned n) noexcept {
    return (x >> n) | (x << (32U - n));
}

inline void Sha256Transform(
    std::array<std::uint32_t, 8>& state,
    const std::uint8_t block[64]
) noexcept {
    static constexpr std::array<std::uint32_t, 64> K = {
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
        0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
        0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
        0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
        0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
        0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
        0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
        0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
        0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
        0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
        0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
        0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
        0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
        0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
        0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
        0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
    };

    std::uint32_t w[64]{};
    for (std::size_t i = 0; i < 16; ++i) {
        w[i] = (static_cast<std::uint32_t>(block[i * 4 + 0]) << 24U) |
               (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16U) |
               (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8U) |
               static_cast<std::uint32_t>(block[i * 4 + 3]);
    }
    for (std::size_t i = 16; i < 64; ++i) {
        const std::uint32_t s0 =
            RotR32(w[i - 15], 7) ^ RotR32(w[i - 15], 18) ^ (w[i - 15] >> 3U);
        const std::uint32_t s1 =
            RotR32(w[i - 2], 17) ^ RotR32(w[i - 2], 19) ^ (w[i - 2] >> 10U);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    std::uint32_t a = state[0];
    std::uint32_t b = state[1];
    std::uint32_t c = state[2];
    std::uint32_t d = state[3];
    std::uint32_t e = state[4];
    std::uint32_t f = state[5];
    std::uint32_t g = state[6];
    std::uint32_t h = state[7];

    for (std::size_t i = 0; i < 64; ++i) {
        const std::uint32_t s1 = RotR32(e, 6) ^ RotR32(e, 11) ^ RotR32(e, 25);
        const std::uint32_t ch = (e & f) ^ ((~e) & g);
        const std::uint32_t temp1 = h + s1 + ch + K[i] + w[i];
        const std::uint32_t s0 = RotR32(a, 2) ^ RotR32(a, 13) ^ RotR32(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temp2 = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

} // namespace detail

using Sha256Digest = std::array<std::uint8_t, 32>;

[[nodiscard]]
inline Sha256Digest Sha256(std::span<const std::uint8_t> data) noexcept {
    std::array<std::uint32_t, 8> state = {
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U
    };

    std::uint8_t buffer[64]{};
    std::size_t offset = 0;
    while (offset + 64 <= data.size()) {
        detail::Sha256Transform(state, data.data() + offset);
        offset += 64;
    }

    const std::size_t rem = data.size() - offset;
    if (rem > 0) {
        std::memcpy(buffer, data.data() + offset, rem);
    }
    buffer[rem] = 0x80U;

    if (rem >= 56) {
        detail::Sha256Transform(state, buffer);
        std::memset(buffer, 0, sizeof(buffer));
    }

    const std::uint64_t bit_len = static_cast<std::uint64_t>(data.size()) * 8ULL;
    for (std::size_t i = 0; i < 8; ++i) {
        buffer[56 + i] = static_cast<std::uint8_t>(
            (bit_len >> ((7U - i) * 8U)) & 0xFFU
        );
    }
    detail::Sha256Transform(state, buffer);

    Sha256Digest digest{};
    for (std::size_t i = 0; i < 8; ++i) {
        digest[i * 4 + 0] = static_cast<std::uint8_t>((state[i] >> 24U) & 0xFFU);
        digest[i * 4 + 1] = static_cast<std::uint8_t>((state[i] >> 16U) & 0xFFU);
        digest[i * 4 + 2] = static_cast<std::uint8_t>((state[i] >> 8U) & 0xFFU);
        digest[i * 4 + 3] = static_cast<std::uint8_t>(state[i] & 0xFFU);
    }
    return digest;
}

[[nodiscard]]
inline Sha256Digest Sha256(std::string_view text) noexcept {
    return Sha256(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(text.data()),
        text.size()
    ));
}

[[nodiscard]]
inline std::string ToHexLower(std::span<const std::uint8_t> bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2U);
    for (const std::uint8_t b : bytes) {
        out.push_back(kHex[(b >> 4U) & 0x0FU]);
        out.push_back(kHex[b & 0x0FU]);
    }
    return out;
}

[[nodiscard]]
inline std::string Sha256Hex(std::string_view text) {
    const auto digest = Sha256(text);
    return ToHexLower(digest);
}

// ============================================================
// HMAC-SHA256 (RFC 2104)
// ============================================================

[[nodiscard]]
inline Sha256Digest HmacSha256(
    std::span<const std::uint8_t> key,
    std::span<const std::uint8_t> message
) {
    std::array<std::uint8_t, 64> padded_key{};
    if (key.size() > 64) {
        const auto hashed = Sha256(key);
        std::memcpy(padded_key.data(), hashed.data(), hashed.size());
    } else if (!key.empty()) {
        std::memcpy(padded_key.data(), key.data(), key.size());
    }

    std::vector<std::uint8_t> inner;
    inner.resize(64 + message.size());
    for (std::size_t i = 0; i < 64; ++i) {
        inner[i] = static_cast<std::uint8_t>(padded_key[i] ^ 0x36U);
    }
    if (!message.empty()) {
        std::memcpy(inner.data() + 64, message.data(), message.size());
    }
    const auto inner_hash = Sha256(inner);

    std::array<std::uint8_t, 64 + 32> outer{};
    for (std::size_t i = 0; i < 64; ++i) {
        outer[i] = static_cast<std::uint8_t>(padded_key[i] ^ 0x5CU);
    }
    std::memcpy(outer.data() + 64, inner_hash.data(), inner_hash.size());

    return Sha256(outer);
}

[[nodiscard]]
inline Sha256Digest HmacSha256(
    std::string_view key,
    std::string_view message
) {
    return HmacSha256(
        std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(key.data()),
            key.size()
        ),
        std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(message.data()),
            message.size()
        )
    );
}

[[nodiscard]]
inline bool HmacSha256Verify(
    std::string_view key,
    std::string_view message,
    std::span<const std::uint8_t> expected_mac
) {
    const auto computed = HmacSha256(key, message);
    return ConstantTimeEquals(computed, expected_mac);
}

// ============================================================
// Base64URL (RFC 4648 Section 5, unpadded)
// ============================================================

[[nodiscard]]
inline std::string Base64UrlEncode(std::span<const std::uint8_t> data) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

    std::string out;
    out.reserve(((data.size() + 2U) / 3U) * 4U);

    std::size_t i = 0;
    while (i + 3U <= data.size()) {
        const std::uint32_t v =
            (static_cast<std::uint32_t>(data[i]) << 16U) |
            (static_cast<std::uint32_t>(data[i + 1U]) << 8U) |
            static_cast<std::uint32_t>(data[i + 2U]);
        out.push_back(kAlphabet[(v >> 18U) & 0x3FU]);
        out.push_back(kAlphabet[(v >> 12U) & 0x3FU]);
        out.push_back(kAlphabet[(v >> 6U) & 0x3FU]);
        out.push_back(kAlphabet[v & 0x3FU]);
        i += 3U;
    }

    const std::size_t rem = data.size() - i;
    if (rem == 1U) {
        const std::uint32_t v = static_cast<std::uint32_t>(data[i]) << 16U;
        out.push_back(kAlphabet[(v >> 18U) & 0x3FU]);
        out.push_back(kAlphabet[(v >> 12U) & 0x3FU]);
    } else if (rem == 2U) {
        const std::uint32_t v =
            (static_cast<std::uint32_t>(data[i]) << 16U) |
            (static_cast<std::uint32_t>(data[i + 1U]) << 8U);
        out.push_back(kAlphabet[(v >> 18U) & 0x3FU]);
        out.push_back(kAlphabet[(v >> 12U) & 0x3FU]);
        out.push_back(kAlphabet[(v >> 6U) & 0x3FU]);
    }

    return out;
}

[[nodiscard]]
inline std::string Base64UrlEncode(std::string_view text) {
    return Base64UrlEncode(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(text.data()),
        text.size()
    ));
}

[[nodiscard]]
inline std::optional<std::vector<std::uint8_t>>
Base64UrlDecode(std::string_view input) {
    auto decode_char = [](unsigned char c) noexcept -> int {
        if (c >= 'A' && c <= 'Z') return static_cast<int>(c - 'A');
        if (c >= 'a' && c <= 'z') return static_cast<int>(c - 'a') + 26;
        if (c >= '0' && c <= '9') return static_cast<int>(c - '0') + 52;
        if (c == '-') return 62;
        if (c == '_') return 63;
        return -1;
    };

    // Strip optional trailing '=' padding if present, up to 2.
    while (!input.empty() && input.back() == '=') {
        input.remove_suffix(1);
    }

    if (input.size() % 4U == 1U) {
        return std::nullopt;
    }

    std::vector<std::uint8_t> out;
    out.reserve((input.size() * 3U) / 4U);

    std::size_t i = 0;
    while (i + 4U <= input.size()) {
        const int a = decode_char(static_cast<unsigned char>(input[i]));
        const int b = decode_char(static_cast<unsigned char>(input[i + 1U]));
        const int c = decode_char(static_cast<unsigned char>(input[i + 2U]));
        const int d = decode_char(static_cast<unsigned char>(input[i + 3U]));
        if (a < 0 || b < 0 || c < 0 || d < 0) {
            return std::nullopt;
        }
        const std::uint32_t v =
            (static_cast<std::uint32_t>(a) << 18U) |
            (static_cast<std::uint32_t>(b) << 12U) |
            (static_cast<std::uint32_t>(c) << 6U) |
            static_cast<std::uint32_t>(d);
        out.push_back(static_cast<std::uint8_t>((v >> 16U) & 0xFFU));
        out.push_back(static_cast<std::uint8_t>((v >> 8U) & 0xFFU));
        out.push_back(static_cast<std::uint8_t>(v & 0xFFU));
        i += 4U;
    }

    const std::size_t rem = input.size() - i;
    if (rem == 2U) {
        const int a = decode_char(static_cast<unsigned char>(input[i]));
        const int b = decode_char(static_cast<unsigned char>(input[i + 1U]));
        if (a < 0 || b < 0 || (b & 0x0F) != 0) {
            return std::nullopt;
        }
        const std::uint32_t v =
            (static_cast<std::uint32_t>(a) << 18U) |
            (static_cast<std::uint32_t>(b) << 12U);
        out.push_back(static_cast<std::uint8_t>((v >> 16U) & 0xFFU));
    } else if (rem == 3U) {
        const int a = decode_char(static_cast<unsigned char>(input[i]));
        const int b = decode_char(static_cast<unsigned char>(input[i + 1U]));
        const int c = decode_char(static_cast<unsigned char>(input[i + 2U]));
        if (a < 0 || b < 0 || c < 0 || (c & 0x03) != 0) {
            return std::nullopt;
        }
        const std::uint32_t v =
            (static_cast<std::uint32_t>(a) << 18U) |
            (static_cast<std::uint32_t>(b) << 12U) |
            (static_cast<std::uint32_t>(c) << 6U);
        out.push_back(static_cast<std::uint8_t>((v >> 16U) & 0xFFU));
        out.push_back(static_cast<std::uint8_t>((v >> 8U) & 0xFFU));
    }

    return out;
}

} // namespace lamp::security
