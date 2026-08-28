#pragma once
/// opra.hpp — OPRA symbol parsing and exchange text ingestion helpers.
///
/// Assumptions:
/// - OPRA symbols arrive in the canonical 21-character OCC format:
///   root[6] + yymmdd[6] + C/P[1] + strike_millis[8]
/// - Example: "AAPL  240621C00195000"
/// - Exchange text payload is a compact CSV line:
///   opra_code,timestamp_ns,bid_price,ask_price,bid_qty,ask_qty,exchange
/// - Price strings are decimal ASCII with up to 8 fractional digits.

#include "core/expected.hpp"
#include "core/fixed_price.hpp"
#include "core/safe_numeric.hpp"

#include <array>
#include <charconv>
#include <cstdint>
#include <string_view>

namespace options_ats_core::market_data {

using core::FixedPrice;

enum class OptionRight : uint8_t {
    Call,
    Put,
};

struct OpraCode {
    static constexpr std::size_t kRootBytes = 6;
    static constexpr std::size_t kCodeBytes = 21;
    // OCC encodes strike with 3 decimal places in the last 8 digits.
    static constexpr int64_t kStrikePriceScaleDivisor = 1'000;

    std::array<char, kRootBytes> root{};
    uint8_t expiry_year{0};
    uint8_t expiry_month{0};
    uint8_t expiry_day{0};
    OptionRight right{OptionRight::Call};
    FixedPrice strike{};

    [[nodiscard]] std::string_view root_view() const noexcept {
        std::size_t len = 0;
        while (len < kRootBytes && root[len] != '\0') {
            ++len;
        }
        return {root.data(), len};
    }
};

struct ExchangeQuote {
    OpraCode symbol;
    uint64_t timestamp_ns{0};
    FixedPrice bid{};
    FixedPrice ask{};
    int32_t bid_qty{0};
    int32_t ask_qty{0};
    std::string_view exchange{};
};

enum class OpraParseError : uint8_t {
    InvalidLength,
    InvalidRoot,
    InvalidExpiry,
    InvalidRight,
    InvalidStrike,
    InvalidTimestamp,
    InvalidPrice,
    InvalidQuantity,
    MissingField,
};

[[nodiscard]] constexpr std::string_view to_string(OpraParseError error) noexcept {
    switch (error) {
        case OpraParseError::InvalidLength:    return "InvalidLength";
        case OpraParseError::InvalidRoot:      return "InvalidRoot";
        case OpraParseError::InvalidExpiry:    return "InvalidExpiry";
        case OpraParseError::InvalidRight:     return "InvalidRight";
        case OpraParseError::InvalidStrike:    return "InvalidStrike";
        case OpraParseError::InvalidTimestamp: return "InvalidTimestamp";
        case OpraParseError::InvalidPrice:     return "InvalidPrice";
        case OpraParseError::InvalidQuantity:  return "InvalidQuantity";
        case OpraParseError::MissingField:     return "MissingField";
    }
    return "Unknown";
}

namespace detail {

[[nodiscard]] constexpr bool is_ascii_alnum(char ch) noexcept {
    return (ch >= 'A' && ch <= 'Z') ||
           (ch >= 'a' && ch <= 'z') ||
           (ch >= '0' && ch <= '9');
}

template <typename Integer>
[[nodiscard]] constexpr core::expected<Integer, OpraParseError>
parse_unsigned_digits(std::string_view text) noexcept {
    if (text.empty()) {
        return core::unexpected(OpraParseError::MissingField);
    }

    Integer value = 0;
    const char* const begin = text.data();
    const char* const end = begin + text.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{} || ptr != end) {
        return core::unexpected(OpraParseError::InvalidStrike);
    }
    return value;
}

[[nodiscard]] constexpr core::expected<uint64_t, OpraParseError>
parse_timestamp_ns(std::string_view text) noexcept {
    if (text.empty()) {
        return core::unexpected(OpraParseError::MissingField);
    }

    uint64_t value = 0;
    const char* const begin = text.data();
    const char* const end = begin + text.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{} || ptr != end) {
        return core::unexpected(OpraParseError::InvalidTimestamp);
    }
    return value;
}

[[nodiscard]] constexpr core::expected<int32_t, OpraParseError>
parse_quantity(std::string_view text) noexcept {
    if (text.empty()) {
        return core::unexpected(OpraParseError::MissingField);
    }

    int64_t value = 0;
    const char* const begin = text.data();
    const char* const end = begin + text.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{} || ptr != end || value <= 0) {
        return core::unexpected(OpraParseError::InvalidQuantity);
    }

    const auto narrowed = core::checked_narrow_cast<int32_t>(value);
    if (!narrowed.has_value()) {
        return core::unexpected(OpraParseError::InvalidQuantity);
    }
    return *narrowed;
}

[[nodiscard]] constexpr core::expected<FixedPrice, OpraParseError>
parse_decimal_price(std::string_view text) noexcept {
    if (text.empty()) {
        return core::unexpected(OpraParseError::MissingField);
    }

    constexpr char kDecimalPoint = '.';
    const std::size_t decimal_pos = text.find(kDecimalPoint);
    const std::string_view whole_text =
        (decimal_pos == std::string_view::npos) ? text : text.substr(0, decimal_pos);
    const std::string_view frac_text =
        (decimal_pos == std::string_view::npos) ? std::string_view{} : text.substr(decimal_pos + 1);

    if (whole_text.empty()) {
        return core::unexpected(OpraParseError::InvalidPrice);
    }
    if (frac_text.size() > static_cast<std::size_t>(FixedPrice::kDecimalDigits)) {
        return core::unexpected(OpraParseError::InvalidPrice);
    }

    bool negative = false;
    std::string_view whole_digits = whole_text;
    if (whole_text.front() == '-') {
        negative = true;
        whole_digits.remove_prefix(1);
    }
    if (whole_digits.empty()) {
        return core::unexpected(OpraParseError::InvalidPrice);
    }

    int64_t whole = 0;
    const auto [whole_ptr, whole_ec] = std::from_chars(
        whole_digits.data(), whole_digits.data() + whole_digits.size(), whole);
    if (whole_ec != std::errc{} || whole_ptr != whole_digits.data() + whole_digits.size()) {
        return core::unexpected(OpraParseError::InvalidPrice);
    }

    int64_t frac = 0;
    if (!frac_text.empty()) {
        const auto [frac_ptr, frac_ec] = std::from_chars(
            frac_text.data(), frac_text.data() + frac_text.size(), frac);
        if (frac_ec != std::errc{} || frac_ptr != frac_text.data() + frac_text.size()) {
            return core::unexpected(OpraParseError::InvalidPrice);
        }
    }

    int64_t frac_scale = 1;
    for (std::size_t i = 0; i < frac_text.size(); ++i) {
        const auto next = core::checked_mul(frac_scale, int64_t{10});
        if (!next.has_value()) {
            return core::unexpected(OpraParseError::InvalidPrice);
        }
        frac_scale = *next;
    }

    const auto whole_raw = core::checked_mul(whole, FixedPrice::kScale);
    if (!whole_raw.has_value()) {
        return core::unexpected(OpraParseError::InvalidPrice);
    }

    const auto frac_multiplier = FixedPrice::kScale / frac_scale;
    const auto frac_raw = core::checked_mul(frac, frac_multiplier);
    if (!frac_raw.has_value()) {
        return core::unexpected(OpraParseError::InvalidPrice);
    }

    const auto combined = core::checked_add(*whole_raw, *frac_raw);
    if (!combined.has_value()) {
        return core::unexpected(OpraParseError::InvalidPrice);
    }

    return FixedPrice::from_raw(negative ? -*combined : *combined);
}

[[nodiscard]] constexpr std::array<std::string_view, 7>
split_csv_fields(std::string_view line) noexcept {
    std::array<std::string_view, 7> fields{};
    std::size_t field_index = 0;
    std::size_t start = 0;

    for (std::size_t i = 0; i <= line.size() && field_index < fields.size(); ++i) {
        if (i == line.size() || line[i] == ',') {
            fields[field_index++] = line.substr(start, i - start);
            start = i + 1;
        }
    }

    return fields;
}

} // namespace detail

[[nodiscard]] constexpr core::expected<OpraCode, OpraParseError>
parse_opra_code(std::string_view code) noexcept {
    if (code.size() != OpraCode::kCodeBytes) {
        return core::unexpected(OpraParseError::InvalidLength);
    }

    OpraCode result{};
    std::size_t root_len = 0;
    for (std::size_t i = 0; i < OpraCode::kRootBytes; ++i) {
        const char ch = code[i];
        if (ch == ' ') {
            continue;
        }
        if (!detail::is_ascii_alnum(ch)) {
            return core::unexpected(OpraParseError::InvalidRoot);
        }
        if (root_len >= OpraCode::kRootBytes) {
            return core::unexpected(OpraParseError::InvalidRoot);
        }
        result.root[root_len++] = ch;
    }
    if (root_len == 0) {
        return core::unexpected(OpraParseError::InvalidRoot);
    }
    if (root_len < OpraCode::kRootBytes) {
        result.root[root_len] = '\0';
    }

    const auto year = detail::parse_unsigned_digits<uint8_t>(code.substr(6, 2));
    const auto month = detail::parse_unsigned_digits<uint8_t>(code.substr(8, 2));
    const auto day = detail::parse_unsigned_digits<uint8_t>(code.substr(10, 2));
    if (!year.has_value() || !month.has_value() || !day.has_value() ||
        *month == 0 || *month > 12 || *day == 0 || *day > 31) {
        return core::unexpected(OpraParseError::InvalidExpiry);
    }

    const char right = code[12];
    if (right == 'C') {
        result.right = OptionRight::Call;
    } else if (right == 'P') {
        result.right = OptionRight::Put;
    } else {
        return core::unexpected(OpraParseError::InvalidRight);
    }

    const auto strike_millis = detail::parse_unsigned_digits<uint64_t>(code.substr(13, 8));
    if (!strike_millis.has_value()) {
        return core::unexpected(OpraParseError::InvalidStrike);
    }
    const auto strike_raw = core::checked_mul(
        static_cast<int64_t>(*strike_millis),
        FixedPrice::kScale / OpraCode::kStrikePriceScaleDivisor);
    if (!strike_raw.has_value()) {
        return core::unexpected(OpraParseError::InvalidStrike);
    }

    result.expiry_year = *year;
    result.expiry_month = *month;
    result.expiry_day = *day;
    result.strike = FixedPrice::from_raw(*strike_raw);
    return result;
}

[[nodiscard]] constexpr core::expected<ExchangeQuote, OpraParseError>
parse_exchange_quote_line(std::string_view line) noexcept {
    const auto fields = detail::split_csv_fields(line);
    for (const auto field : fields) {
        if (field.empty()) {
            return core::unexpected(OpraParseError::MissingField);
        }
    }

    const auto symbol = parse_opra_code(fields[0]);
    const auto timestamp = detail::parse_timestamp_ns(fields[1]);
    const auto bid = detail::parse_decimal_price(fields[2]);
    const auto ask = detail::parse_decimal_price(fields[3]);
    const auto bid_qty = detail::parse_quantity(fields[4]);
    const auto ask_qty = detail::parse_quantity(fields[5]);

    if (!symbol.has_value()) {
        return core::unexpected(symbol.error());
    }
    if (!timestamp.has_value()) {
        return core::unexpected(timestamp.error());
    }
    if (!bid.has_value() || !ask.has_value() || *bid <= FixedPrice{} || *ask <= FixedPrice{} ||
        *bid > *ask) {
        return core::unexpected(OpraParseError::InvalidPrice);
    }
    if (!bid_qty.has_value()) {
        return core::unexpected(bid_qty.error());
    }
    if (!ask_qty.has_value()) {
        return core::unexpected(ask_qty.error());
    }

    return ExchangeQuote{
        .symbol = *symbol,
        .timestamp_ns = *timestamp,
        .bid = *bid,
        .ask = *ask,
        .bid_qty = *bid_qty,
        .ask_qty = *ask_qty,
        .exchange = fields[6],
    };
}

} // namespace options_ats_core::market_data
