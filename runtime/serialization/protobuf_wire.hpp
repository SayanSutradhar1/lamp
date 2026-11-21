#pragma once

// ============================================================
// Lamp Framework
// Phase 7 - Serialization
//
// File: runtime/serialization/protobuf_wire.hpp
//
// Purpose:
//   Zero-dependency, bounds-checked Protobuf binary wire
//   format reader and writer (varint, ZigZag, fixed32/64,
//   length-delimited).
// ============================================================

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lamp::serialization {

enum class WireType : std::uint8_t {
    Varint = 0,
    Fixed64 = 1,
    LengthDelimited = 2,
    StartGroup = 3,
    EndGroup = 4,
    Fixed32 = 5
};

[[nodiscard]]
constexpr bool IsSupportedWireType(std::uint8_t raw) noexcept {
    return raw == 0 || raw == 1 || raw == 2 || raw == 5;
}

[[nodiscard]]
constexpr std::uint64_t MakeTag(
    std::uint32_t fieldNumber,
    WireType wireType
) noexcept {
    return (static_cast<std::uint64_t>(fieldNumber) << 3U) |
           static_cast<std::uint64_t>(wireType);
}

[[nodiscard]]
constexpr std::uint64_t ZigZagEncode64(std::int64_t value) noexcept {
    const auto u = static_cast<std::uint64_t>(value);
    const std::uint64_t signMask =
        value < 0 ? ~std::uint64_t{0} : std::uint64_t{0};
    return (u << 1U) ^ signMask;
}

[[nodiscard]]
constexpr std::int64_t ZigZagDecode64(std::uint64_t value) noexcept {
    return static_cast<std::int64_t>(
        (value >> 1U) ^ (~(value & 1ULL) + 1ULL)
    );
}

class ProtobufWriter final {
public:
    void Reserve(std::size_t bytes) {
        buffer_.reserve(buffer_.size() + bytes);
    }

    void WriteVarint(std::uint64_t value) {
        while (value >= 0x80ULL) {
            buffer_.push_back(
                static_cast<std::uint8_t>((value & 0x7FULL) | 0x80ULL)
            );
            value >>= 7U;
        }
        buffer_.push_back(static_cast<std::uint8_t>(value));
    }

    void WriteSignedVarint(std::int64_t value) {
        WriteVarint(static_cast<std::uint64_t>(value));
    }

    void WriteZigZagVarint(std::int64_t value) {
        WriteVarint(ZigZagEncode64(value));
    }

    void WriteTag(std::uint32_t fieldNumber, WireType wireType) {
        WriteVarint(MakeTag(fieldNumber, wireType));
    }

    void WriteFixed32(std::uint32_t value) {
        for (int i = 0; i < 4; ++i) {
            buffer_.push_back(
                static_cast<std::uint8_t>((value >> (i * 8)) & 0xFFU)
            );
        }
    }

    void WriteFixed64(std::uint64_t value) {
        for (int i = 0; i < 8; ++i) {
            buffer_.push_back(
                static_cast<std::uint8_t>((value >> (i * 8)) & 0xFFULL)
            );
        }
    }

    void WriteDouble(double value) {
        WriteFixed64(std::bit_cast<std::uint64_t>(value));
    }

    void WriteBytes(std::span<const std::uint8_t> bytes) {
        WriteVarint(static_cast<std::uint64_t>(bytes.size()));
        buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
    }

    void WriteString(std::string_view text) {
        WriteVarint(static_cast<std::uint64_t>(text.size()));
        buffer_.reserve(buffer_.size() + text.size());
        for (const unsigned char ch : text) {
            buffer_.push_back(static_cast<std::uint8_t>(ch));
        }
    }

    [[nodiscard]]
    const std::vector<std::uint8_t>& Data() const noexcept {
        return buffer_;
    }

    [[nodiscard]]
    std::vector<std::uint8_t> Take() noexcept {
        return std::move(buffer_);
    }

private:
    std::vector<std::uint8_t> buffer_;
};

class ProtobufReader final {
public:
    explicit ProtobufReader(
        std::span<const std::uint8_t> input
    ) noexcept
        : input_(input) {}

    [[nodiscard]]
    bool AtEnd() const noexcept {
        return offset_ >= input_.size();
    }

    [[nodiscard]]
    std::size_t Remaining() const noexcept {
        return input_.size() - offset_;
    }

    [[nodiscard]]
    bool ReadVarint(std::uint64_t& out) noexcept {
        std::uint64_t result = 0;
        std::uint32_t shift = 0;

        for (int i = 0; i < 10; ++i) {
            if (offset_ >= input_.size()) {
                return false;
            }

            const std::uint8_t byte = input_[offset_++];
            if (i == 9 && (byte & 0xFEU) != 0) {
                return false;
            }

            result |=
                static_cast<std::uint64_t>(byte & 0x7FU) << shift;

            if ((byte & 0x80U) == 0) {
                out = result;
                return true;
            }

            shift += 7U;
        }

        return false;
    }

    [[nodiscard]]
    bool ReadTag(
        std::uint32_t& fieldNumber,
        WireType& wireType
    ) noexcept {
        std::uint64_t tag = 0;
        if (!ReadVarint(tag)) {
            return false;
        }

        const auto rawWire = static_cast<std::uint8_t>(tag & 0x07ULL);
        if (!IsSupportedWireType(rawWire)) {
            return false;
        }

        const std::uint64_t num = tag >> 3U;
        if (num == 0 || num > 536870911ULL) {
            return false;
        }

        fieldNumber = static_cast<std::uint32_t>(num);
        wireType = static_cast<WireType>(rawWire);
        return true;
    }

    [[nodiscard]]
    bool ReadFixed32(std::uint32_t& out) noexcept {
        if (Remaining() < 4) {
            return false;
        }

        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            value |= static_cast<std::uint32_t>(input_[offset_++])
                     << (i * 8);
        }
        out = value;
        return true;
    }

    [[nodiscard]]
    bool ReadFixed64(std::uint64_t& out) noexcept {
        if (Remaining() < 8) {
            return false;
        }

        std::uint64_t value = 0;
        for (int i = 0; i < 8; ++i) {
            value |= static_cast<std::uint64_t>(input_[offset_++])
                     << (i * 8);
        }
        out = value;
        return true;
    }

    [[nodiscard]]
    bool ReadDouble(double& out) noexcept {
        std::uint64_t bits = 0;
        if (!ReadFixed64(bits)) {
            return false;
        }
        out = std::bit_cast<double>(bits);
        return true;
    }

    [[nodiscard]]
    bool ReadLengthDelimited(
        std::span<const std::uint8_t>& out
    ) noexcept {
        std::uint64_t length = 0;
        if (!ReadVarint(length)) {
            return false;
        }

        if (length > Remaining()) {
            return false;
        }

        const auto size = static_cast<std::size_t>(length);
        out = input_.subspan(offset_, size);
        offset_ += size;
        return true;
    }

    [[nodiscard]]
    bool SkipField(WireType wireType) noexcept {
        switch (wireType) {
            case WireType::Varint: {
                std::uint64_t dummy = 0;
                return ReadVarint(dummy);
            }
            case WireType::Fixed64: {
                std::uint64_t dummy = 0;
                return ReadFixed64(dummy);
            }
            case WireType::LengthDelimited: {
                std::span<const std::uint8_t> dummy;
                return ReadLengthDelimited(dummy);
            }
            case WireType::Fixed32: {
                std::uint32_t dummy = 0;
                return ReadFixed32(dummy);
            }
            case WireType::StartGroup:
            case WireType::EndGroup:
                return false;
        }
        return false;
    }

private:
    std::span<const std::uint8_t> input_;
    std::size_t offset_ = 0;
};

} // namespace lamp::serialization
