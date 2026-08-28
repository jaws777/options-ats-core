#pragma once
/// safe_numeric.hpp — Checked integer arithmetic for hot-path primitives.
///
/// Rationale:
/// - Signed overflow is UB. Trading code cannot rely on wraparound.
/// - Narrowing conversions must be explicit and range-checked.
/// - The helpers below are constexpr-capable for unit tests and compile-time
///   constants, while using compiler builtins at runtime when available.

#include "core/expected.hpp"

#include <concepts>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace options_ats_core::core {

enum class NumericError : uint8_t {
    Overflow,
    Underflow,
    Narrowing,
};

namespace detail {

template <typename T>
[[nodiscard]] constexpr bool add_overflow_constexpr(T lhs, T rhs, T& out) noexcept {
    if constexpr (std::is_signed_v<T>) {
        if ((rhs > 0 && lhs > std::numeric_limits<T>::max() - rhs) ||
            (rhs < 0 && lhs < std::numeric_limits<T>::min() - rhs)) {
            return true;
        }
    } else if (lhs > std::numeric_limits<T>::max() - rhs) {
        return true;
    }

    out = static_cast<T>(lhs + rhs);
    return false;
}

template <typename T>
[[nodiscard]] constexpr bool sub_overflow_constexpr(T lhs, T rhs, T& out) noexcept {
    if constexpr (std::is_signed_v<T>) {
        if ((rhs > 0 && lhs < std::numeric_limits<T>::min() + rhs) ||
            (rhs < 0 && lhs > std::numeric_limits<T>::max() + rhs)) {
            return true;
        }
    } else if (lhs < rhs) {
        return true;
    }

    out = static_cast<T>(lhs - rhs);
    return false;
}

template <typename T>
[[nodiscard]] constexpr bool mul_overflow_constexpr(T lhs, T rhs, T& out) noexcept {
    if (lhs == 0 || rhs == 0) {
        out = 0;
        return false;
    }

    if constexpr (std::is_signed_v<T>) {
        if (lhs == -1 && rhs == std::numeric_limits<T>::min()) {
            return true;
        }
        if (rhs == -1 && lhs == std::numeric_limits<T>::min()) {
            return true;
        }
        if (lhs > 0) {
            if ((rhs > 0 && lhs > std::numeric_limits<T>::max() / rhs) ||
                (rhs < 0 && rhs < std::numeric_limits<T>::min() / lhs)) {
                return true;
            }
        } else {
            if ((rhs > 0 && lhs < std::numeric_limits<T>::min() / rhs) ||
                (rhs < 0 && lhs != 0 &&
                 rhs < std::numeric_limits<T>::max() / lhs)) {
                return true;
            }
        }
    } else if (lhs > std::numeric_limits<T>::max() / rhs) {
        return true;
    }

    out = static_cast<T>(lhs * rhs);
    return false;
}

} // namespace detail

template <std::integral T>
[[nodiscard]] constexpr expected<T, NumericError>
checked_add(T lhs, T rhs) noexcept {
    T out{};
    const bool overflow = [] (T left, T right, T& result) constexpr noexcept {
        if (std::is_constant_evaluated()) {
            return detail::add_overflow_constexpr(left, right, result);
        }
#if defined(__has_builtin)
#if __has_builtin(__builtin_add_overflow)
        return __builtin_add_overflow(left, right, &result);
#else
        return detail::add_overflow_constexpr(left, right, result);
#endif
#else
        return detail::add_overflow_constexpr(left, right, result);
#endif
    }(lhs, rhs, out);

    if (overflow) {
        return unexpected(rhs < 0 ? NumericError::Underflow : NumericError::Overflow);
    }
    return out;
}

template <std::integral T>
[[nodiscard]] constexpr expected<T, NumericError>
checked_sub(T lhs, T rhs) noexcept {
    T out{};
    const bool overflow = [] (T left, T right, T& result) constexpr noexcept {
        if (std::is_constant_evaluated()) {
            return detail::sub_overflow_constexpr(left, right, result);
        }
#if defined(__has_builtin)
#if __has_builtin(__builtin_sub_overflow)
        return __builtin_sub_overflow(left, right, &result);
#else
        return detail::sub_overflow_constexpr(left, right, result);
#endif
#else
        return detail::sub_overflow_constexpr(left, right, result);
#endif
    }(lhs, rhs, out);

    if (overflow) {
        return unexpected(rhs < 0 ? NumericError::Overflow : NumericError::Underflow);
    }
    return out;
}

template <std::integral T>
[[nodiscard]] constexpr expected<T, NumericError>
checked_mul(T lhs, T rhs) noexcept {
    T out{};
    const bool overflow = [] (T left, T right, T& result) constexpr noexcept {
        if (std::is_constant_evaluated()) {
            return detail::mul_overflow_constexpr(left, right, result);
        }
#if defined(__has_builtin)
#if __has_builtin(__builtin_mul_overflow)
        return __builtin_mul_overflow(left, right, &result);
#else
        return detail::mul_overflow_constexpr(left, right, result);
#endif
#else
        return detail::mul_overflow_constexpr(left, right, result);
#endif
    }(lhs, rhs, out);

    if (overflow) {
        return unexpected(((lhs < 0) ^ (rhs < 0)) ? NumericError::Underflow
                                                       : NumericError::Overflow);
    }
    return out;
}

template <std::integral To, std::integral From>
[[nodiscard]] constexpr expected<To, NumericError>
checked_narrow_cast(From value) noexcept {
    using ToLimits = std::numeric_limits<To>;
    if constexpr (std::is_same_v<To, From>) {
        return value;
    } else {
        if constexpr (std::is_signed_v<From> == std::is_signed_v<To>) {
            if (value < static_cast<From>(ToLimits::min()) ||
                value > static_cast<From>(ToLimits::max())) {
                return unexpected(NumericError::Narrowing);
            }
        } else if constexpr (std::is_signed_v<From>) {
            if (value < 0 ||
                value > static_cast<From>(ToLimits::max())) {
                return unexpected(NumericError::Narrowing);
            }
        } else {
            if (value > static_cast<std::make_unsigned_t<From>>(ToLimits::max())) {
                return unexpected(NumericError::Narrowing);
            }
        }

        return static_cast<To>(value);
    }
}

} // namespace options_ats_core::core
