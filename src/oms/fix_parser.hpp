#pragma once
/// fix_parser.hpp — Minimal FIX 4.4 NewOrderSingle parser.
///
/// FIX messages are tag=value pairs delimited by SOH (0x01).
/// This parses the subset of tags needed for order entry:
///   Tag 11  = ClOrdID (client order ID)
///   Tag 54  = Side (1=Buy, 2=Sell)
///   Tag 38  = OrderQty
///   Tag 44  = Price
///   Tag 55  = Symbol
///   Tag 59  = TimeInForce (0=Day, 1=GTC, 3=IOC, 4=FOK)
///   Tag 35  = MsgType (D=NewOrderSingle, F=OrderCancelRequest)
///
/// A production parser would additionally validate checksum (tag 10),
/// body length (tag 9), and sequence numbers (tag 34). This module focuses
/// on application-layer parsing.

#include "core/expected.hpp"
#include "core/fixed_price.hpp"
#include "core/safe_numeric.hpp"

#include <array>
#include <charconv>
#include <cstdint>
#include <string_view>

namespace options_ats_core::oms {

using core::FixedPrice;

enum class FixParseError : uint8_t {
    MissingTag,
    InvalidTag,
    InvalidSide,
    InvalidQuantity,
    InvalidPrice,
    InvalidMsgType,
    MalformedMessage,
};

[[nodiscard]] constexpr std::string_view to_string(FixParseError e) noexcept {
    switch (e) {
        case FixParseError::MissingTag:       return "MissingTag";
        case FixParseError::InvalidTag:       return "InvalidTag";
        case FixParseError::InvalidSide:      return "InvalidSide";
        case FixParseError::InvalidQuantity:  return "InvalidQuantity";
        case FixParseError::InvalidPrice:     return "InvalidPrice";
        case FixParseError::InvalidMsgType:   return "InvalidMsgType";
        case FixParseError::MalformedMessage: return "MalformedMessage";
    }
    return "Unknown";
}

// FIX protocol uses single-character codes for MsgType (tag 35):
// 'D' = NewOrderSingle, 'F' = OrderCancelRequest. We map these to an enum
// for type safety rather than passing raw chars through the system.
enum class FixMsgType : uint8_t {
    NewOrderSingle,      // D
    OrderCancelRequest,  // F
    Unknown,
};

// FIX Side (tag 54): single-char numeric codes. 1=Buy, 2=Sell.
// Explicit uint8_t values match the FIX wire encoding directly.
enum class FixSide : uint8_t {
    Buy  = 1,
    Sell = 2,
};

// FIX TimeInForce (tag 59): numeric codes per FIX 4.4 spec.
// Note the gap: IOC=3, not 2. Value 2 is reserved for OPG (At the Opening),
// which is not relevant for an options ATS. FOK=4 follows IOC.
enum class TimeInForce : uint8_t {
    Day = 0,
    GTC = 1,
    IOC = 3,  // Immediate-Or-Cancel. 2 is OPG (At the Opening) — intentional gap.
    FOK = 4,
};

/// Parsed NewOrderSingle fields.
/// string_view fields (cl_ord_id, symbol) reference the original FIX message
/// buffer — this is zero-copy parsing. The caller's message buffer MUST outlive
/// this struct. In production you'd copy these into a fixed-size char array
/// (e.g., std::array<char, 20>) so the parsed order is self-contained and safe
/// to enqueue to another thread without lifetime concerns.
struct FixNewOrder {
    FixMsgType msg_type{FixMsgType::Unknown};
    std::string_view cl_ord_id{};    // Tag 11 — references original msg buffer (zero-copy)
    FixSide side{FixSide::Buy};      // Tag 54
    int32_t quantity{0};             // Tag 38
    FixedPrice price{};              // Tag 44
    std::string_view symbol{};       // Tag 55 — references original msg buffer (zero-copy)
    TimeInForce tif{TimeInForce::Day}; // Tag 59
};

namespace detail {

/// Find value for a given FIX tag in a SOH-delimited message.
/// Returns empty string_view if not found.
///
/// Linear scan: FIX messages are typically <500 bytes with <20 tags. For this
/// size, a linear scan over SOH-delimited fields is faster than building a hash
/// map — the overhead of hashing and allocating a map exceeds the cost of a
/// simple byte scan that fits entirely in L1 cache. The function searches for
/// the "tag=" prefix (e.g., "35=") at the start of each SOH-delimited field.
[[nodiscard]] constexpr std::string_view
find_fix_tag(std::string_view msg, std::string_view tag_prefix) noexcept {
    std::size_t pos = 0;
    while (pos < msg.size()) {
        // Check if this field starts with our tag prefix (e.g., "35=").
        if (msg.substr(pos).starts_with(tag_prefix)) {
            std::size_t val_start = pos + tag_prefix.size();
            // Value runs from after "tag=" to the next SOH (or end of message).
            std::size_t val_end = msg.find('\x01', val_start);
            if (val_end == std::string_view::npos) {
                val_end = msg.size(); // Last field may lack trailing SOH.
            }
            return msg.substr(val_start, val_end - val_start);
        }
        // Advance past this field's SOH delimiter to the next field.
        std::size_t next = msg.find('\x01', pos);
        if (next == std::string_view::npos) break;
        pos = next + 1;
    }
    return {}; // Tag not present in message.
}

[[nodiscard]] inline core::expected<int32_t, FixParseError>
parse_fix_int(std::string_view val) noexcept {
    if (val.empty()) return core::unexpected(FixParseError::MissingTag);
    int32_t result = 0;
    auto [ptr, ec] = std::from_chars(val.data(), val.data() + val.size(), result);
    if (ec != std::errc{} || ptr != val.data() + val.size()) {
        return core::unexpected(FixParseError::InvalidTag);
    }
    return result;
}

/// Decimal string -> FixedPrice conversion.
/// Strategy: split on '.', parse whole and fractional parts separately, then
/// scale the fractional part up to FixedPrice::kScale (e.g., 10^8).
/// Example: "123.45" -> whole=123, frac=45, frac has 2 digits so frac_scale=100.
///   raw = 123 * 10^8 + 45 * (10^8 / 100) = 12_300_000_000 + 45_000_000
/// Uses checked arithmetic throughout. FIX is an external input boundary, so
/// malformed or overflowing prices must be rejected before they enter the OMS.
[[nodiscard]] inline core::expected<FixedPrice, FixParseError>
parse_fix_price(std::string_view val) noexcept {
    if (val.empty()) return core::unexpected(FixParseError::MissingTag);

    // Split on decimal point. Integer prices (no '.') have empty frac.
    auto dot = val.find('.');
    std::string_view whole = (dot == std::string_view::npos) ? val : val.substr(0, dot);
    std::string_view frac = (dot == std::string_view::npos) ?
        std::string_view{} : val.substr(dot + 1);
    if (whole.empty() || frac.size() > static_cast<std::size_t>(FixedPrice::kDecimalDigits)) {
        return core::unexpected(FixParseError::InvalidPrice);
    }
    if (whole.front() == '-') {
        return core::unexpected(FixParseError::InvalidPrice);
    }

    // Parse the whole-number part.
    int64_t whole_val = 0;
    auto [wp, wec] = std::from_chars(whole.data(), whole.data() + whole.size(), whole_val);
    if (wec != std::errc{} || wp != whole.data() + whole.size()) {
        return core::unexpected(FixParseError::InvalidPrice);
    }

    // Parse the fractional part and compute its decimal scale (10^num_digits).
    int64_t frac_val = 0;
    int64_t frac_scale = 1;
    if (!frac.empty()) {
        auto [fp, fec] = std::from_chars(frac.data(), frac.data() + frac.size(), frac_val);
        if (fec != std::errc{} || fp != frac.data() + frac.size()) {
            return core::unexpected(FixParseError::InvalidPrice);
        }
        for (std::size_t i = 0; i < frac.size(); ++i) {
            const auto next_scale = core::checked_mul(frac_scale, int64_t{10});
            if (!next_scale.has_value()) {
                return core::unexpected(FixParseError::InvalidPrice);
            }
            frac_scale = *next_scale;
        }
    }

    // Combine: scale whole part to fixed-point, then add fractional contribution.
    const auto whole_raw = core::checked_mul(whole_val, FixedPrice::kScale);
    if (!whole_raw.has_value()) {
        return core::unexpected(FixParseError::InvalidPrice);
    }
    const auto frac_raw = core::checked_mul(frac_val, FixedPrice::kScale / frac_scale);
    if (!frac_raw.has_value()) {
        return core::unexpected(FixParseError::InvalidPrice);
    }
    const auto raw = core::checked_add(*whole_raw, *frac_raw);
    if (!raw.has_value()) {
        return core::unexpected(FixParseError::InvalidPrice);
    }
    if (*raw <= 0) {
        return core::unexpected(FixParseError::InvalidPrice);
    }
    return FixedPrice::from_raw(*raw);
}

} // namespace detail

/// Parse a FIX message (SOH-delimited) into a FixNewOrder.
/// Tag extraction order here does NOT matter — find_fix_tag scans the entire
/// message for each tag, so tags can appear in any order in the wire message.
/// Validation strategy: reject early with typed errors (core::expected), never
/// proceed with invalid data. This keeps the error path explicit and forces
/// callers to handle every failure mode.
[[nodiscard]] inline core::expected<FixNewOrder, FixParseError>
parse_fix_new_order(std::string_view msg) noexcept {
    FixNewOrder result;

    // MsgType (35) — must be present and recognized. Reject unknown types
    // rather than silently ignoring them (defensive parsing).
    auto msg_type_val = detail::find_fix_tag(msg, "35=");
    if (msg_type_val.empty()) return core::unexpected(FixParseError::MissingTag);
    if (msg_type_val == "D") {
        result.msg_type = FixMsgType::NewOrderSingle;
    } else if (msg_type_val == "F") {
        result.msg_type = FixMsgType::OrderCancelRequest;
    } else {
        return core::unexpected(FixParseError::InvalidMsgType);
    }

    // ClOrdID (11) — mandatory. Every order needs a unique client order ID
    // for idempotent resubmission and reconciliation.
    result.cl_ord_id = detail::find_fix_tag(msg, "11=");
    if (result.cl_ord_id.empty()) {
        return core::unexpected(FixParseError::MissingTag);
    }

    // Side (54) — must be 1 (Buy) or 2 (Sell). No default — invalid side
    // is a hard reject. An ATS cannot guess intent.
    auto side_val = detail::find_fix_tag(msg, "54=");
    if (side_val == "1") {
        result.side = FixSide::Buy;
    } else if (side_val == "2") {
        result.side = FixSide::Sell;
    } else {
        return core::unexpected(FixParseError::InvalidSide);
    }

    // OrderQty (38) — must be a positive integer. Zero or negative quantities
    // are rejected as invalid.
    auto qty_val = detail::find_fix_tag(msg, "38=");
    auto qty = detail::parse_fix_int(qty_val);
    if (!qty.has_value() || *qty <= 0) {
        return core::unexpected(FixParseError::InvalidQuantity);
    }
    result.quantity = *qty;

    // Price (44) — OPTIONAL. Market orders don't carry a price; limit orders do.
    // If absent, price remains default-constructed (zero). The OMS layer
    // decides whether a missing price is valid based on the order type.
    auto price_val = detail::find_fix_tag(msg, "44=");
    if (!price_val.empty()) {
        auto price = detail::parse_fix_price(price_val);
        if (!price.has_value()) return core::unexpected(price.error());
        result.price = *price;
    }

    // Symbol (55) — stored as string_view into the original message buffer.
    result.symbol = detail::find_fix_tag(msg, "55=");

    // TimeInForce (59) — defaults to Day if absent or explicitly "0".
    auto tif_val = detail::find_fix_tag(msg, "59=");
    if (tif_val == "0" || tif_val.empty()) result.tif = TimeInForce::Day;
    else if (tif_val == "1") result.tif = TimeInForce::GTC;
    else if (tif_val == "3") result.tif = TimeInForce::IOC;
    else if (tif_val == "4") result.tif = TimeInForce::FOK;
    else return core::unexpected(FixParseError::InvalidTag);

    return result;
}

} // namespace options_ats_core::oms
