#pragma once
/// expected.hpp — Minimal expected<T, E> polyfill for AppleClang.
///
/// AppleClang (as of version 21) does not ship <expected>. This provides
/// the subset used by our codebase: construction, value/error access,
/// has_value(), operator*, and unexpected.
///
/// When the toolchain supports C++23 <expected>, delete this file and
/// switch to the standard type.

// Use the standard library implementation where available (Linux/GCC/Clang).
// Keep the local polyfill path for Apple toolchains that may lack <expected>.
#if defined(__cpp_lib_expected) && __cpp_lib_expected >= 202202L
#include <expected>

namespace options_ats_core::core {

using std::expected;
using std::unexpected;

} // namespace options_ats_core::core
#else

#include <cassert>
#include <type_traits>
#include <utility>
#include <variant>

namespace options_ats_core::core {

template <typename E>
class unexpected {
public:
    constexpr explicit unexpected(const E& e) : error_(e) {}
    constexpr explicit unexpected(E&& e) : error_(std::move(e)) {}
    [[nodiscard]] constexpr const E& error() const& noexcept { return error_; }
    [[nodiscard]] constexpr E& error() & noexcept { return error_; }
    [[nodiscard]] constexpr E&& error() && noexcept { return std::move(error_); }
private:
    E error_;
};

template <typename E>
unexpected(E) -> unexpected<E>;

template <typename T, typename E>
class expected {
public:
    // --- Constructors ---
    constexpr expected() : data_(T{}) {}
    constexpr expected(const T& val) : data_(val) {}          // NOLINT implicit
    constexpr expected(T&& val) : data_(std::move(val)) {}    // NOLINT implicit
    constexpr expected(const unexpected<E>& u) : data_(u.error()) {} // NOLINT
    constexpr expected(unexpected<E>&& u) : data_(std::move(u).error()) {} // NOLINT

    // --- Observers ---
    [[nodiscard]] constexpr bool has_value() const noexcept {
        return std::holds_alternative<T>(data_);
    }
    constexpr explicit operator bool() const noexcept { return has_value(); }

    [[nodiscard]] constexpr const T& value() const& {
        assert(has_value());
        return std::get<T>(data_);
    }
    [[nodiscard]] constexpr T& value() & {
        assert(has_value());
        return std::get<T>(data_);
    }
    [[nodiscard]] constexpr T&& value() && {
        assert(has_value());
        return std::get<T>(std::move(data_));
    }

    [[nodiscard]] constexpr const T& operator*() const& noexcept {
        return std::get<T>(data_);
    }
    [[nodiscard]] constexpr T& operator*() & noexcept {
        return std::get<T>(data_);
    }

    [[nodiscard]] constexpr const T* operator->() const noexcept {
        return &std::get<T>(data_);
    }
    [[nodiscard]] constexpr T* operator->() noexcept {
        return &std::get<T>(data_);
    }

    [[nodiscard]] constexpr const E& error() const& {
        assert(!has_value());
        return std::get<E>(data_);
    }
    [[nodiscard]] constexpr E& error() & {
        assert(!has_value());
        return std::get<E>(data_);
    }

private:
    std::variant<T, E> data_;
};

} // namespace options_ats_core::core

#endif
