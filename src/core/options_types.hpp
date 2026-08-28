#pragma once
/// options_types.hpp — Options-specific domain types.
///
/// Core types for listed options: exercise style, settlement, Greeks,
/// and option specification. These are referenced throughout the ATS
/// but are especially relevant for:
///   - SeriesBook: keying quotes by option specification
///   - Risk engine: post-trade Greeks aggregation
///   - EQ engine: per-name scoring (name = underlying)
///
/// Design note on Greeks:
/// Greeks are computed asynchronously — not on the matching hot path. Each
/// fill is consumed by a Greeks calculator (Black-Scholes or vol-surface
/// lookup), aggregated by underlying, and tracked as delta, gamma, vega,
/// theta. The matching engine does not need Greeks; the risk engine does.

#include "core/fixed_price.hpp"

#include <cstdint>
#include <string_view>

namespace options_ats_core::core {

/// Exercise style determines when the holder can exercise.
/// American: any time before expiry (standard for equity options).
/// European: only at expiry (standard for index options like SPX).
///
/// Engineering impact: American options can be assigned at any time,
/// which means the settlement service must monitor early exercise risk
/// continuously. European options only need assignment processing at expiry.
enum class ExerciseStyle : uint8_t {
    American,   // Equity options: AAPL, MSFT, etc.
    European,   // Index options: SPX, NDX, etc.
};

/// Settlement method after exercise.
/// Physical: delivery of the underlying shares (equity options).
/// Cash: cash payment of intrinsic value (index options).
enum class SettlementType : uint8_t {
    Physical,   // Deliver 100 shares per contract at strike price.
    Cash,       // Pay (spot - strike) * multiplier.
};

/// Option right: call or put.
enum class OptionRight : uint8_t {
    Call,       // Right to buy at strike.
    Put,        // Right to sell at strike.
};

/// Full specification of an options series.
/// This uniquely identifies one tradeable contract.
///
/// Example: AAPL 2026-06-18 C $195.00 American Physical
///   root = "AAPL", expiry = 20260618, right = Call,
///   strike = $195.00 (FixedPrice), exercise = American,
///   settlement = Physical, multiplier = 100
struct OptionSpec {
    uint32_t series_id{0};              // Internal series identifier.
    char root[8]{};                     // Underlying symbol (null-terminated).
    uint32_t expiry_yyyymmdd{0};        // Expiry date as YYYYMMDD integer.
    OptionRight right{OptionRight::Call};
    FixedPrice strike{};                // Strike price (fixed-point).
    ExerciseStyle exercise{ExerciseStyle::American};
    SettlementType settlement{SettlementType::Physical};
    int32_t multiplier{100};            // Contracts * multiplier = shares.

    [[nodiscard]] std::string_view root_view() const noexcept {
        std::size_t len = 0;
        while (len < sizeof(root) && root[len] != '\0') ++len;
        return {root, len};
    }

    /// Days to expiry (caller provides current date as YYYYMMDD).
    /// Simplified — production would use a proper date library.
    [[nodiscard]] constexpr int32_t dte(uint32_t today_yyyymmdd) const noexcept {
        // Rough approximation: (expiry - today) assuming 30-day months.
        // Production: Julian day number conversion.
        if (expiry_yyyymmdd <= today_yyyymmdd) return 0;
        return static_cast<int32_t>(expiry_yyyymmdd - today_yyyymmdd);
    }
};

/// Greeks for a single position.
/// All values are per-contract (multiply by position size for total).
///
/// These are NOT computed on the matching hot path. They are computed
/// asynchronously by a post-trade risk service consuming fills from
/// the WAL or Aeron stream.
///
/// Storage: we use double here (not FixedPrice) because Greeks are
/// derived quantities used for risk monitoring, not for price comparison
/// or order execution. The project-wide prohibition on double applies to
/// price storage or comparison — Greeks are neither. They are
/// intermediate analytics with inherent model uncertainty (vol surface,
/// interest rate assumptions) that dwarfs floating-point imprecision.
struct Greeks {
    double delta{0.0};    // dV/dS: price sensitivity to underlying.
    double gamma{0.0};    // d²V/dS²: rate of change of delta.
    double vega{0.0};     // dV/dσ: sensitivity to implied volatility.
    double theta{0.0};    // dV/dt: time decay (typically negative for long).
    double rho{0.0};      // dV/dr: sensitivity to interest rate.

    /// Aggregate two Greeks (e.g., adding a new fill's Greeks to position).
    constexpr Greeks& operator+=(const Greeks& rhs) noexcept {
        delta += rhs.delta;
        gamma += rhs.gamma;
        vega  += rhs.vega;
        theta += rhs.theta;
        rho   += rhs.rho;
        return *this;
    }

    /// Scale Greeks by position size.
    [[nodiscard]] constexpr Greeks operator*(double size) const noexcept {
        return Greeks{
            .delta = delta * size,
            .gamma = gamma * size,
            .vega  = vega  * size,
            .theta = theta * size,
            .rho   = rho   * size,
        };
    }
};

/// Aggregated Greeks for a portfolio or underlying.
/// Updated asynchronously as fills flow through the post-trade risk service.
struct PositionGreeks {
    uint32_t underlying_hash{0};  // Hash of the underlying symbol.
    int64_t net_contracts{0};     // Net position in contracts.
    Greeks greeks{};              // Aggregated Greeks across all series for this underlying.
};

/// to_string helpers.
[[nodiscard]] constexpr std::string_view to_string(ExerciseStyle e) noexcept {
    switch (e) {
        case ExerciseStyle::American: return "American";
        case ExerciseStyle::European: return "European";
    }
    return "Unknown";
}

[[nodiscard]] constexpr std::string_view to_string(SettlementType s) noexcept {
    switch (s) {
        case SettlementType::Physical: return "Physical";
        case SettlementType::Cash:     return "Cash";
    }
    return "Unknown";
}

[[nodiscard]] constexpr std::string_view to_string(OptionRight r) noexcept {
    switch (r) {
        case OptionRight::Call: return "Call";
        case OptionRight::Put:  return "Put";
    }
    return "Unknown";
}

} // namespace options_ats_core::core
