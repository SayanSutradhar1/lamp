
#pragma once

#include <algorithm>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lamp::http {

// ============================================================
// HTTP Header Errors
// ============================================================

class HttpHeaderError final : public std::invalid_argument {
public:
    explicit HttpHeaderError(const std::string& message)
        : std::invalid_argument(message) {}
};

// ============================================================
// HTTP Header Validation
// ============================================================
//
// Header field names use HTTP token syntax.
//
// Header values may contain:
//   HTAB, SP, visible ASCII, obs-text (0x80-0xFF)
//
// CR, LF, NUL, DEL and other control characters are rejected.
// Leading/trailing whitespace is allowed here so that a parser
// can explicitly normalize optional whitespace.
//
// ============================================================

[[nodiscard]]
constexpr bool isHttpHeaderNameCharacter(
    unsigned char character
) noexcept {
    if ((character >= 'A' && character <= 'Z') ||
        (character >= 'a' && character <= 'z') ||
        (character >= '0' && character <= '9')) {
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
constexpr bool isValidHttpHeaderName(
    std::string_view name
) noexcept {
    if (name.empty()) {
        return false;
    }

    for (const char character : name) {
        if (!isHttpHeaderNameCharacter(
                static_cast<unsigned char>(character)
            )) {
            return false;
        }
    }

    return true;
}

[[nodiscard]]
constexpr bool isValidHttpHeaderValue(
    std::string_view value
) noexcept {
    for (const char character : value) {
        const auto byte =
            static_cast<unsigned char>(character);

        if (byte == '\t' ||
            (byte >= 0x20 && byte <= 0x7E) ||
            byte >= 0x80) {
            continue;
        }

        return false;
    }

    return true;
}

// ============================================================
// ASCII Case-Insensitive Comparison
// ============================================================
//
// HTTP header names are ASCII case-insensitive.
//
// This deliberately avoids locale-sensitive transformations.
//
// ============================================================

[[nodiscard]]
constexpr unsigned char toLowerHttpAscii(
    unsigned char character
) noexcept {
    if (character >= 'A' && character <= 'Z') {
        return static_cast<unsigned char>(
            character - 'A' + 'a'
        );
    }

    return character;
}

[[nodiscard]]
constexpr bool httpHeaderNameEquals(
    std::string_view left,
    std::string_view right
) noexcept {
    if (left.size() != right.size()) {
        return false;
    }

    for (std::size_t i = 0; i < left.size(); ++i) {
        if (toLowerHttpAscii(
                static_cast<unsigned char>(left[i])
            ) !=
            toLowerHttpAscii(
                static_cast<unsigned char>(right[i])
            )) {
            return false;
        }
    }

    return true;
}

// ============================================================
// HTTP Header Entry
// ============================================================

struct HttpHeader {
    std::string name;
    std::string value;
};

// ============================================================
// HTTP Headers Collection
// ============================================================
//
// Properties:
//
// - Case-insensitive field-name comparison
// - Case-preserving storage
// - Duplicate fields are supported
// - Insertion order is preserved
// - Header values are not implicitly combined
// - Copyable and movable
//
// Thread safety:
//
// HttpHeaders is not internally synchronized.
// Concurrent access requires external synchronization when
// any participating operation mutates the collection.
//
// Reference lifetime:
//
// Views returned by get(), values(), and entries() remain
// valid only while the referenced strings are alive and
// unmodified. Mutating the collection may invalidate views.
//
// ============================================================

class HttpHeaders final {
public:
    using Container = std::vector<HttpHeader>;
    using const_iterator = Container::const_iterator;

    HttpHeaders() = default;

    explicit HttpHeaders(Container headers) {
        for (auto& header : headers) {
            append(
                std::move(header.name),
                std::move(header.value)
            );
        }
    }

    // ========================================================
    // Append
    // ========================================================
    //
    // Adds a new field without replacing existing entries.
    //
    // Multiple Set-Cookie fields, for example, must remain
    // separate rather than being joined into one value.
    //
    // ========================================================

    void append(
        std::string name,
        std::string value
    ) {
        validate(name, value);

        headers_.push_back({
            std::move(name),
            std::move(value)
        });
    }

    // ========================================================
    // Set
    // ========================================================
    //
    // Replaces all existing entries with the same name.
    //
    // The new field is appended at the end.
    //
    // ========================================================

    void set(
        std::string name,
        std::string value
    ) {
        validate(name, value);

        // Validate before modifying existing entries.
        eraseValidated(name);

        headers_.push_back({
            std::move(name),
            std::move(value)
        });
    }

    // ========================================================
    // Get First Value
    // ========================================================
    //
    // Returns the first matching header value.
    //
    // No allocation occurs during lookup.
    //
    // ========================================================

    [[nodiscard]]
    std::optional<std::string_view> get(
        std::string_view name
    ) const noexcept {
        for (const auto& header : headers_) {
            if (httpHeaderNameEquals(header.name, name)) {
                return std::string_view(header.value);
            }
        }

        return std::nullopt;
    }

    // ========================================================
    // Get All Values
    // ========================================================
    //
    // Returns all matching values in insertion order.
    //
    // Values are not joined because comma-combining is not
    // valid for every HTTP field.
    //
    // ========================================================

    [[nodiscard]]
    std::vector<std::string_view> values(
        std::string_view name
    ) const {
        std::vector<std::string_view> result;

        for (const auto& header : headers_) {
            if (httpHeaderNameEquals(header.name, name)) {
                result.emplace_back(header.value);
            }
        }

        return result;
    }

    // ========================================================
    // Contains
    // ========================================================

    [[nodiscard]]
    bool contains(
        std::string_view name
    ) const noexcept {
        return get(name).has_value();
    }

    // ========================================================
    // Count Matching Fields
    // ========================================================

    [[nodiscard]]
    std::size_t count(
        std::string_view name
    ) const noexcept {
        std::size_t result = 0;

        for (const auto& header : headers_) {
            if (httpHeaderNameEquals(header.name, name)) {
                ++result;
            }
        }

        return result;
    }

    // ========================================================
    // Erase
    // ========================================================
    //
    // Removes every field matching the supplied name.
    //
    // Returns the number of removed fields.
    //
    // ========================================================

    std::size_t erase(
        std::string_view name
    ) {
        return eraseValidated(name);
    }

    // ========================================================
    // Clear
    // ========================================================

    void clear() noexcept {
        headers_.clear();
    }

    // ========================================================
    // Collection Inspection
    // ========================================================

    [[nodiscard]]
    bool empty() const noexcept {
        return headers_.empty();
    }

    [[nodiscard]]
    std::size_t size() const noexcept {
        return headers_.size();
    }

    [[nodiscard]]
    const Container& entries() const noexcept {
        return headers_;
    }

    [[nodiscard]]
    const_iterator begin() const noexcept {
        return headers_.cbegin();
    }

    [[nodiscard]]
    const_iterator end() const noexcept {
        return headers_.cend();
    }

    [[nodiscard]]
    const_iterator cbegin() const noexcept {
        return headers_.cbegin();
    }

    [[nodiscard]]
    const_iterator cend() const noexcept {
        return headers_.cend();
    }

    // ========================================================
    // Reserve
    // ========================================================

    void reserve(std::size_t capacity) {
        headers_.reserve(capacity);
    }

    // ========================================================
    // Swap
    // ========================================================

    void swap(HttpHeaders& other) noexcept {
        headers_.swap(other.headers_);
    }

private:
    // ========================================================
    // Validation
    // ========================================================

    static void validate(
        std::string_view name,
        std::string_view value
    ) {
        if (!isValidHttpHeaderName(name)) {
            throw HttpHeaderError(
                "Invalid HTTP header field name"
            );
        }

        if (!isValidHttpHeaderValue(value)) {
            throw HttpHeaderError(
                "Invalid HTTP header field value"
            );
        }
    }

    // ========================================================
    // Internal Erase
    // ========================================================

    std::size_t eraseValidated(
        std::string_view name
    ) {
        const auto originalSize = headers_.size();

        std::erase_if(
            headers_,
            [name](const HttpHeader& header) {
                return httpHeaderNameEquals(
                    header.name,
                    name
                );
            }
        );

        return originalSize - headers_.size();
    }

    Container headers_;
};

// ============================================================
// Compile-Time Validation
// ============================================================

static_assert(
    isValidHttpHeaderName("Content-Type")
);

static_assert(
    isValidHttpHeaderName("X-Custom-Header")
);

static_assert(
    !isValidHttpHeaderName("")
);

static_assert(
    !isValidHttpHeaderName("Bad Header")
);

static_assert(
    !isValidHttpHeaderName("Bad:Header")
);

static_assert(
    isValidHttpHeaderValue("application/json")
);

static_assert(
    isValidHttpHeaderValue("")
);

static_assert(
    !isValidHttpHeaderValue("value\r\nInjected: true")
);

static_assert(
    httpHeaderNameEquals(
        "Content-Type",
        "content-type"
    )
);

static_assert(
    !httpHeaderNameEquals(
        "Content-Type",
        "Content-Length"
    )
);

} // namespace lamp::http
