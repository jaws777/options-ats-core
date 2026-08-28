#pragma once
/// fixed_price.hpp — Fixed-point price representation for options trading.
///
/// Internal representation: int64_t scaled by 10^8.
/// Rationale: options tick in pennies ($0.01) but market makers compute
/// sub-penny edge. 10^8 gives 8 decimal digits of precision while keeping
/// max representable price at ~92 billion — well within any realistic range.
/// Never use double for price storage or comparison.

#include <array>
#include <charconv>
#include <compare>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "core/safe_numeric.hpp"

namespace options_ats_core::core {

enum class FixedPriceError : uint8_t {
    InvalidCents,
    NumericOverflow,
};

class FixedPrice {
public:
    static constexpr int64_t kScale = 100'000'000;  // 10^8
    // Two decimal places are mandated by listed US options display conventions.
    static constexpr int64_t kCentScale = kScale / 100;
    static constexpr int kDecimalDigits = 8;
    // Sign + 19 digits + decimal point + trailing NUL.
    static constexpr std::size_t kCharsCapacity = 24;
    static constexpr std::size_t kWholeCharsCapacity = 20;

    constexpr FixedPrice() noexcept : raw_{0} {}

    /// Construct from raw wire value (already scaled).
    [[nodiscard]] static constexpr FixedPrice from_raw(int64_t raw) noexcept {
        FixedPrice p;
        p.raw_ = raw;
        return p;
    }

    /// Construct from integer dollars and cents. No floating point.
    [[nodiscard]] static constexpr FixedPrice from_dollars_cents(
        int64_t dollars, int32_t cents) noexcept {
        auto result = try_from_dollars_cents(dollars, cents);
        return result.has_value() ? *result : FixedPrice{};
    }

    /// Fallible construction for production code paths where overflow matters.
    [[nodiscard]] static constexpr expected<FixedPrice, FixedPriceError>
    try_from_dollars_cents(int64_t dollars, int32_t cents) noexcept {
        if (cents <= -100 || cents >= 100) {
            return unexpected(FixedPriceError::InvalidCents);
        }

        const auto dollars_scaled = checked_mul(dollars, kScale);
        if (!dollars_scaled.has_value()) {
            return unexpected(FixedPriceError::NumericOverflow);
        }

        const auto cents_scaled = checked_mul(static_cast<int64_t>(cents), kCentScale);
        if (!cents_scaled.has_value()) {
            return unexpected(FixedPriceError::NumericOverflow);
        }

        const auto raw = checked_add(*dollars_scaled, *cents_scaled);
        if (!raw.has_value()) {
            return unexpected(FixedPriceError::NumericOverflow);
        }

        return from_raw(*raw);
    }

    /// Compile-time only: construct from double literal.
    /// Do NOT call at runtime — exists for constexpr test constants.
    [[nodiscard]] static consteval FixedPrice from_double(double d) noexcept {
        FixedPrice p;
        p.raw_ = static_cast<int64_t>(d * static_cast<double>(kScale) +
                                       (d >= 0.0 ? 0.5 : -0.5));
        return p;
    }

    /// Wire serialization.
    [[nodiscard]] constexpr int64_t to_raw() const noexcept { return raw_; }

    /// Stack-allocated string representation. No heap allocation.
    [[nodiscard]] std::array<char, kCharsCapacity> to_chars() const noexcept {
        std::array<char, kCharsCapacity> buf{};
        const uint64_t abs_raw = raw_ < 0
            ? static_cast<uint64_t>(-(raw_ + 1)) + 1U
            : static_cast<uint64_t>(raw_);
        int64_t whole = abs_raw / kScale;
        int64_t frac = abs_raw % kScale;

        char* ptr = buf.data();
        if (raw_ < 0) *ptr++ = '-';

        auto [wend, _w] = std::to_chars(ptr, buf.data() + kWholeCharsCapacity, whole);
        *wend++ = '.';

        // Pad fractional part to kDecimalDigits digits, then strip trailing zeros.
        char frac_buf[kDecimalDigits + 1]{};
        auto [fend, _f] = std::to_chars(frac_buf, frac_buf + kDecimalDigits, frac);
        int frac_len = static_cast<int>(fend - frac_buf);
        int pad = kDecimalDigits - frac_len;

        for (int i = 0; i < pad; ++i) *wend++ = '0';
        std::memcpy(wend, frac_buf, static_cast<size_t>(frac_len));
        wend += frac_len;

        // Strip trailing zeros but keep at least 2 decimal places.
        char* decimal_point = wend - kDecimalDigits - 1;
        while (wend > decimal_point + 3 && *(wend - 1) == '0') --wend;

        *wend = '\0';
        return buf;
    }

    [[nodiscard]] std::string_view to_string_view(
        std::array<char, kCharsCapacity>& buf) const noexcept {
        buf = to_chars();
        return {buf.data()};
    }

    // --- Arithmetic ---

    [[nodiscard]] constexpr FixedPrice operator+(FixedPrice rhs) const noexcept {
        auto result = try_add(rhs);
        return result.has_value() ? *result : FixedPrice{};
    }

    [[nodiscard]] constexpr FixedPrice operator-(FixedPrice rhs) const noexcept {
        auto result = try_sub(rhs);
        return result.has_value() ? *result : FixedPrice{};
    }

    [[nodiscard]] constexpr FixedPrice operator-() const noexcept {
        auto result = try_negate();
        return result.has_value() ? *result : FixedPrice{};
    }

    /// Multiply price by integer quantity. Result is in price-space (for notional).
    [[nodiscard]] constexpr FixedPrice operator*(int64_t qty) const noexcept {
        auto result = try_mul(qty);
        return result.has_value() ? *result : FixedPrice{};
    }

    constexpr FixedPrice& operator+=(FixedPrice rhs) noexcept {
        auto result = try_add(rhs);
        raw_ = result.has_value() ? result->raw_ : 0;
        return *this;
    }

    constexpr FixedPrice& operator-=(FixedPrice rhs) noexcept {
        auto result = try_sub(rhs);
        raw_ = result.has_value() ? result->raw_ : 0;
        return *this;
    }

    [[nodiscard]] constexpr expected<FixedPrice, FixedPriceError>
    try_add(FixedPrice rhs) const noexcept {
        const auto result = checked_add(raw_, rhs.raw_);
        if (!result.has_value()) {
            return unexpected(FixedPriceError::NumericOverflow);
        }
        return from_raw(*result);
    }

    [[nodiscard]] constexpr expected<FixedPrice, FixedPriceError>
    try_sub(FixedPrice rhs) const noexcept {
        const auto result = checked_sub(raw_, rhs.raw_);
        if (!result.has_value()) {
            return unexpected(FixedPriceError::NumericOverflow);
        }
        return from_raw(*result);
    }

    [[nodiscard]] constexpr expected<FixedPrice, FixedPriceError>
    try_negate() const noexcept {
        const auto result = checked_sub<int64_t>(0, raw_);
        if (!result.has_value()) {
            return unexpected(FixedPriceError::NumericOverflow);
        }
        return from_raw(*result);
    }

    [[nodiscard]] constexpr expected<FixedPrice, FixedPriceError>
    try_mul(int64_t qty) const noexcept {
        const auto result = checked_mul(raw_, qty);
        if (!result.has_value()) {
            return unexpected(FixedPriceError::NumericOverflow);
        }
        return from_raw(*result);
    }

    // --- Comparison (C++20 three-way) ---

    [[nodiscard]] constexpr auto operator<=>(const FixedPrice&) const noexcept = default;
    [[nodiscard]] constexpr bool operator==(const FixedPrice&) const noexcept = default;

    // --- Utility ---

    [[nodiscard]] constexpr bool is_zero() const noexcept { return raw_ == 0; }

    /// Penny tick: $0.01
    [[nodiscard]] static constexpr FixedPrice penny_tick() noexcept {
        return from_raw(kScale / 100);
    }

    /// Nickel tick: $0.05
    [[nodiscard]] static constexpr FixedPrice nickel_tick() noexcept {
        return from_raw(kScale / 20);
    }

private:
    int64_t raw_;
};

} // namespace options_ats_core::core
