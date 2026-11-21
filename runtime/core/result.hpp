
#pragma once

#include "core/error.hpp"

#include <cassert>
#include <concepts>
#include <functional>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

namespace lamp::core {

// ============================================================
// Result<T>
// ============================================================

template <typename T>
class [[nodiscard]] Result final {
    static_assert(
        !std::is_void_v<T>,
        "Use Result<void> for operations without a value."
    );

    static_assert(
        std::is_object_v<T> &&
        !std::is_array_v<T> &&
        !std::is_const_v<T> &&
        !std::is_volatile_v<T>,
        "Result<T> requires a non-cv-qualified object type."
    );

    static_assert(
        !std::is_same_v<T, LampError>,
        "Result<LampError> is ambiguous."
    );

public:
    using ValueType = T;
    using ErrorType = LampError;

    // --------------------------------------------------------
    // Construction
    // --------------------------------------------------------

    template <typename U = T>
        requires std::constructible_from<T, U&&>
    static Result success(U&& value) {
        return Result(
            std::in_place_index<0>,
            std::forward<U>(value)
        );
    }

    template <typename... Args>
        requires std::constructible_from<T, Args&&...>
    static Result emplace(Args&&... args) {
        return Result(
            std::in_place_index<0>,
            std::forward<Args>(args)...
        );
    }

    static Result failure(LampError error) {
        return Result(
            std::in_place_index<1>,
            std::move(error)
        );
    }

    static Result failure(
        ErrorCode code,
        std::string message
    ) {
        return failure(
            makeError(code, std::move(message))
        );
    }

    Result(const Result&) = default;
    Result(Result&&) = default;

    Result& operator=(const Result&) = default;
    Result& operator=(Result&&) = default;

    ~Result() = default;

    // --------------------------------------------------------
    // State Inspection
    // --------------------------------------------------------

    [[nodiscard]]
    bool hasValue() const noexcept {
        return storage_.index() == 0;
    }

    [[nodiscard]]
    bool hasError() const noexcept {
        return storage_.index() == 1;
    }

    [[nodiscard]]
    explicit operator bool() const noexcept {
        return hasValue();
    }

    // --------------------------------------------------------
    // Value Access
    // --------------------------------------------------------

    [[nodiscard]]
    T& value() & {
        ensureValue();
        return std::get<0>(storage_);
    }

    [[nodiscard]]
    const T& value() const & {
        ensureValue();
        return std::get<0>(storage_);
    }

    [[nodiscard]]
    T&& value() && {
        ensureValue();
        return std::get<0>(std::move(storage_));
    }

    // --------------------------------------------------------
    // Error Access
    // --------------------------------------------------------

    [[nodiscard]]
    LampError& error() & {
        ensureError();
        return std::get<1>(storage_);
    }

    [[nodiscard]]
    const LampError& error() const & {
        ensureError();
        return std::get<1>(storage_);
    }

    [[nodiscard]]
    LampError&& error() && {
        ensureError();
        return std::get<1>(std::move(storage_));
    }

    // --------------------------------------------------------
    // Non-throwing Pointer Access
    // --------------------------------------------------------

    [[nodiscard]]
    T* valueIf() noexcept {
        return std::get_if<0>(&storage_);
    }

    [[nodiscard]]
    const T* valueIf() const noexcept {
        return std::get_if<0>(&storage_);
    }

    [[nodiscard]]
    LampError* errorIf() noexcept {
        return std::get_if<1>(&storage_);
    }

    [[nodiscard]]
    const LampError* errorIf() const noexcept {
        return std::get_if<1>(&storage_);
    }

    // --------------------------------------------------------
    // Fallback Value
    // --------------------------------------------------------

    template <typename U>
        requires std::constructible_from<T, U&&> &&
                 std::copy_constructible<T>
    [[nodiscard]]
    T valueOr(U&& fallback) const & {
        if (hasValue()) {
            return std::get<0>(storage_);
        }

        return T(std::forward<U>(fallback));
    }

    template <typename U>
        requires std::constructible_from<T, U&&> &&
                 std::move_constructible<T>
    [[nodiscard]]
    T valueOr(U&& fallback) && {
        if (hasValue()) {
            return std::get<0>(std::move(storage_));
        }

        return T(std::forward<U>(fallback));
    }

    // --------------------------------------------------------
    // Transform Successful Value
    // --------------------------------------------------------

    template <typename F>
        requires std::invocable<F, const T&> &&
                 (!std::is_void_v<
                     std::invoke_result_t<F, const T&>
                 >)
    [[nodiscard]]
    auto map(F&& function) const &
        -> Result<
            std::remove_cvref_t<
                std::invoke_result_t<F, const T&>
            >
        >
    {
        using U = std::remove_cvref_t<
            std::invoke_result_t<F, const T&>
        >;

        if (hasError()) {
            return Result<U>::failure(error());
        }

        return Result<U>::success(
            std::invoke(
                std::forward<F>(function),
                value()
            )
        );
    }

    // --------------------------------------------------------
    // Chain Result-returning Operations
    // --------------------------------------------------------

    template <typename F>
        requires std::invocable<F, const T&>
    [[nodiscard]]
    auto andThen(F&& function) const &
        -> std::invoke_result_t<F, const T&>
    {
        using NextResult =
            std::invoke_result_t<F, const T&>;

        static_assert(
            requires(LampError error) {
                NextResult::failure(std::move(error));
            },
            "andThen callback must return a Result type."
        );

        if (hasError()) {
            return NextResult::failure(error());
        }

        return std::invoke(
            std::forward<F>(function),
            value()
        );
    }

private:
    // --------------------------------------------------------
    // Internal Storage
    // --------------------------------------------------------

    std::variant<T, LampError> storage_;

    template <std::size_t Index, typename... Args>
    explicit Result(
        std::in_place_index_t<Index>,
        Args&&... args
    )
        : storage_(
            std::in_place_index<Index>,
            std::forward<Args>(args)...
        ) {}

    void ensureValue() const {
        if (!hasValue()) {
            throw std::logic_error(
                "Lamp Result: attempted to access "
                "a value from a failed result."
            );
        }
    }

    void ensureError() const {
        if (!hasError()) {
            throw std::logic_error(
                "Lamp Result: attempted to access "
                "an error from a successful result."
            );
        }
    }
};

// ============================================================
// Result<void>
// ============================================================

template <>
class [[nodiscard]] Result<void> final {
public:
    using ValueType = void;
    using ErrorType = LampError;

    // --------------------------------------------------------
    // Construction
    // --------------------------------------------------------

    static Result success() {
        return Result();
    }

    static Result failure(LampError error) {
        return Result(std::move(error));
    }

    static Result failure(
        ErrorCode code,
        std::string message
    ) {
        return failure(
            makeError(code, std::move(message))
        );
    }

    // --------------------------------------------------------
    // State Inspection
    // --------------------------------------------------------

    [[nodiscard]]
    bool hasValue() const noexcept {
        return !error_.has_value();
    }

    [[nodiscard]]
    bool hasError() const noexcept {
        return error_.has_value();
    }

    [[nodiscard]]
    explicit operator bool() const noexcept {
        return hasValue();
    }

    // --------------------------------------------------------
    // Success Validation
    // --------------------------------------------------------

    void value() const {
        if (hasError()) {
            throw std::logic_error(
                "Lamp Result<void>: operation failed."
            );
        }
    }

    // --------------------------------------------------------
    // Error Access
    // --------------------------------------------------------

    [[nodiscard]]
    LampError& error() & {
        ensureError();
        return *error_;
    }

    [[nodiscard]]
    const LampError& error() const & {
        ensureError();
        return *error_;
    }

    [[nodiscard]]
    LampError&& error() && {
        ensureError();
        return std::move(*error_);
    }

    [[nodiscard]]
    LampError* errorIf() noexcept {
        return error_ ? &*error_ : nullptr;
    }

    [[nodiscard]]
    const LampError* errorIf() const noexcept {
        return error_ ? &*error_ : nullptr;
    }

    // --------------------------------------------------------
    // Chain Operations
    // --------------------------------------------------------

    template <typename F>
        requires std::invocable<F>
    [[nodiscard]]
    auto andThen(F&& function) const &
        -> std::invoke_result_t<F>
    {
        using NextResult =
            std::invoke_result_t<F>;

        static_assert(
            requires(LampError error) {
                NextResult::failure(std::move(error));
            },
            "andThen callback must return a Result type."
        );

        if (hasError()) {
            return NextResult::failure(error());
        }

        return std::invoke(
            std::forward<F>(function)
        );
    }

private:
    std::optional<LampError> error_;

    Result() = default;

    explicit Result(LampError error)
        : error_(std::move(error)) {}

    void ensureError() const {
        if (!hasError()) {
            throw std::logic_error(
                "Lamp Result<void>: no error available."
            );
        }
    }
};

// ============================================================
// Convenience Factories
// ============================================================

template <typename T>
    requires (
        !std::is_void_v<std::remove_cvref_t<T>>
    )
[[nodiscard]]
auto Ok(T&& value)
    -> Result<std::remove_cvref_t<T>>
{
    using Value = std::remove_cvref_t<T>;

    return Result<Value>::success(
        std::forward<T>(value)
    );
}

[[nodiscard]]
inline Result<void> Ok() {
    return Result<void>::success();
}

template <typename T>
[[nodiscard]]
Result<T> Err(
    ErrorCode code,
    std::string message
) {
    return Result<T>::failure(
        makeError(code, std::move(message))
    );
}

} // namespace lamp::core
