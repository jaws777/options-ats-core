#pragma once
/// series_book.hpp — Quote book for a single options series on the ATS.
///
/// Architecture:
/// Market makers are the PASSIVE side — they post two-sided quotes that
/// rest on the book. Retail orders are the AGGRESSIVE side — they arrive
/// and match immediately against the best MM quote.
///
/// The book per series is small: at most one quote per MM. With 5-10 MMs,
/// "matching" is a scan across a flat array to find the best price, not a
/// walk through a deep order book. This is fundamentally different from an
/// exchange order book with thousands of resting orders.
///
/// Allocation: when multiple MMs quote the same best price, allocation is
/// pro-rata by quoted size (standard for options). Pro-rata incentivizes
/// MMs to quote larger size because their fill share is proportional.
///
/// NBBO compliance (Reg NMS Rule 611): every fill must be at or better
/// than the NBBO. The NBBO check is synchronous — it occurs before the
/// fill is confirmed.

#include "core/expected.hpp"
#include "core/fixed_price.hpp"
#include "core/safe_numeric.hpp"
#include "market_data/order_book.hpp" // For Side enum.

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace options_ats_core::market_data {

using core::FixedPrice;

/// Maximum number of Primary Market Makers on the platform.
/// At launch: Akuna, Belvedere, Group One, Optiver, Virtu = 5.
/// Array sized for growth. Direct-indexed by mm_id.
/// Why 16: power-of-2 ensures the MmQuote array aligns naturally to cache
/// line boundaries (64 bytes), and 16 slots give 3x headroom over the 5
/// launch PMMs without wasting meaningful memory. sizeof(MmQuote)*16 fits
/// comfortably in L1 cache, keeping the linear scan fast.
static constexpr uint32_t kMaxMMs = 16;

/// A two-sided quote from a single market maker for a single series.
/// Each PMM posts exactly one two-sided (bid + ask) quote per series.
/// This is the passive liquidity that retail orders match against.
struct MmQuote {
    uint32_t mm_id{0};           // Unique MM identifier; also the index into quotes_[].
    FixedPrice bid{};            // Price the MM will buy at (passive bid).
    FixedPrice ask{};            // Price the MM will sell at (passive ask).
    int32_t bid_size{0};         // Contracts available on the bid. Decremented on fill.
    int32_t ask_size{0};         // Contracts available on the ask. Decremented on fill.
    uint64_t timestamp_tsc{0};   // TSC timestamp of last quote update — used for time
                                 // priority in residual allocation after pro-rata.
    bool active{false};          // MM can withdraw quotes at any time (go dark).
                                 // Inactive quotes are skipped during matching.
};

/// NBBO reference from OPRA (or locally computed from direct feeds).
/// This is the regulatory compliance reference — not a matching input.
/// We never match against the NBBO; we match against MM quotes. The NBBO
/// is used solely to verify that our fill prices don't violate Reg NMS
/// Rule 611 (trade-through prohibition) and to compute price improvement
/// metrics reported back to retail brokers.
struct NbboEntry {
    FixedPrice bid{};            // National best bid across all lit venues.
    FixedPrice ask{};            // National best offer across all lit venues.
    uint64_t timestamp_ns{0};    // Wall-clock nanoseconds — used for staleness checks.
};

/// A fill produced by the ATS matching engine.
/// Every fill captures an NBBO snapshot for deterministic price-improvement
/// analysis. Rule 605 concerns execution-quality disclosures, while Rule 606
/// concerns order-routing disclosures; applicability and required records
/// depend on the regulated entity and workflow. This reference type is not a
/// compliance implementation and must be reviewed by qualified counsel.
struct AtsFill {
    uint64_t client_order_id;
    uint32_t mm_id;           // Which MM was the passive side.
    FixedPrice price;         // Execution price (at MM's quote).
    int32_t quantity;
    FixedPrice nbbo_bid;      // NBBO bid captured at fill time — needed for price
    FixedPrice nbbo_ask;      // Deterministic price-improvement analysis baseline.
    uint64_t timestamp_tsc;
};

/// An incoming retail order (aggressor).
/// Note: there is no price field. Retail orders on this ATS are market /
/// marketable-limit orders — the retail customer does not set a price.
/// They take the best available MM quote. The ATS's job is to find the
/// best MM price that also passes NBBO compliance, not to price-match
/// against a limit. This is standard for retail equity/options ATS flow.
struct RetailOrder {
    uint64_t client_order_id;
    Side side;                // Buy or Sell.
    int32_t quantity;
    uint64_t timestamp_tsc;
    // No price field — retail orders execute at best available MM quote.
    // (Marketable limit or market order semantics.)
};

enum class AtsMatchError : uint8_t {
    NoLiquidity,        // No active MM quotes for this series.
    NbboViolation,      // Best MM quote is worse than NBBO — cannot fill.
    InvalidQuantity,
    QuantityOverflow,
    StaleNbbo,          // NBBO data is too old to rely on.
    PartialFill,        // Only partially filled — liquidity exhausted or NBBO boundary.
};

[[nodiscard]] constexpr std::string_view to_string(AtsMatchError e) noexcept {
    switch (e) {
        case AtsMatchError::NoLiquidity:     return "NoLiquidity";
        case AtsMatchError::NbboViolation:   return "NbboViolation";
        case AtsMatchError::InvalidQuantity: return "InvalidQuantity";
        case AtsMatchError::QuantityOverflow:return "QuantityOverflow";
        case AtsMatchError::StaleNbbo:       return "StaleNbbo";
        case AtsMatchError::PartialFill:     return "PartialFill";
    }
    return "Unknown";
}

/// Per-MM allocation in a pro-rata fill.
struct Allocation {
    uint32_t mm_id;
    int32_t quantity;
};

/// The quote book for a single options series.
///
/// This is NOT a deep order book — it's a flat array of MM quotes.
/// 5 MMs = 5 slots. Best price found by linear scan (cache-friendly,
/// faster than a tree for N ≤ 16).
class SeriesBook {
public:
    // --- Quote Management (MM side) ---

    /// Update or insert a quote from a market maker.
    /// Overwrite semantics: MMs cancel/replace quotes at high frequency
    /// (potentially thousands of times per second). There is no queue of
    /// quotes per MM — each MM has exactly one resting quote, and the latest
    /// update fully replaces the previous one. This is why we direct-index
    /// by mm_id: O(1) overwrite, no search, no allocation.
    void update_quote(const MmQuote& quote) noexcept {
        if (quote.mm_id < kMaxMMs) {
            quotes_[quote.mm_id] = quote;   // Full overwrite — latest quote wins.
            quotes_[quote.mm_id].active = true;
        }
    }

    /// MM withdraws their quote (goes dark on this series).
    void withdraw_quote(uint32_t mm_id) noexcept {
        if (mm_id < kMaxMMs) {
            quotes_[mm_id].active = false;
            quotes_[mm_id].bid_size = 0;
            quotes_[mm_id].ask_size = 0;
        }
    }

    // --- Queries ---

    /// Best bid across all active MMs. Returns nullptr if no bids.
    /// Linear scan is the correct data structure choice here:
    ///   - N <= 16 MMs, so the entire quotes_ array (16 * sizeof(MmQuote))
    ///     fits in 1-2 cache lines. A linear scan over contiguous memory is
    ///     faster than any tree or heap traversal due to cache locality.
    ///   - No branch misprediction pressure at this size.
    ///   - A std::priority_queue or std::set would add heap allocations,
    ///     pointer chasing, and code complexity for zero latency benefit.
    [[nodiscard]] const MmQuote* best_bid() const noexcept {
        const MmQuote* best = nullptr;
        for (const auto& q : quotes_) {
            if (!q.active || q.bid_size <= 0) continue;
            if (!best || q.bid > best->bid) {
                best = &q;
            }
        }
        return best;
    }

    /// Best ask across all active MMs. Returns nullptr if no asks.
    [[nodiscard]] const MmQuote* best_ask() const noexcept {
        const MmQuote* best = nullptr;
        for (const auto& q : quotes_) {
            if (!q.active || q.ask_size <= 0) continue;
            if (!best || q.ask < best->ask) {
                best = &q;
            }
        }
        return best;
    }

    /// BBO from the ATS's own book (not NBBO — that's from OPRA).
    [[nodiscard]] FixedPrice book_bid() const noexcept {
        auto* b = best_bid();
        return b ? b->bid : FixedPrice{};
    }

    [[nodiscard]] FixedPrice book_ask() const noexcept {
        auto* a = best_ask();
        return a ? a->ask : FixedPrice{};
    }

    [[nodiscard]] uint32_t active_mm_count() const noexcept {
        uint32_t count = 0;
        for (const auto& q : quotes_) {
            if (q.active) ++count;
        }
        return count;
    }

    // --- Matching (Retail aggressor side) ---

    /// Match a retail order against MM quotes with pro-rata allocation
    /// and NBBO compliance check.
    ///
    /// Flow:
    /// 1. Find all MMs quoting at the best price for this side.
    /// 2. Check best price against NBBO (Reg NMS Rule 611).
    /// 3. Allocate pro-rata across MMs at best price.
    /// 4. If residual, sweep to next price level (price-time at worse levels).
    /// 5. Produce fills with NBBO snapshot for price improvement reporting.
    /// Match flow:
    ///   1. Validate order quantity.
    ///   2. Invert side: retail buy matches MM asks, retail sell matches MM bids.
    ///   3. Collect all active MM quotes with liquidity on the matching side.
    ///   4. Sort by price (best first: lowest ask for buys, highest bid for sells).
    ///   5. NBBO compliance check — reject if best MM price is worse than NBBO.
    ///   6. Pro-rata allocation at the best price level.
    ///   7. Distribute integer-rounding residual by time priority.
    ///   8. Sweep to next price level if order not fully filled (with NBBO re-check).
    ///   9. Decrement MM quoted sizes and emit fills with NBBO snapshot.
    [[nodiscard]] core::expected<std::vector<AtsFill>, AtsMatchError>
    match_retail_order(const RetailOrder& order, const NbboEntry& nbbo) {
        if (order.quantity <= 0) {
            return core::unexpected(AtsMatchError::InvalidQuantity);
        }

        // Side inversion: the retail order's side is from the customer's
        // perspective. A retail BUY means the customer wants to buy, so we
        // match against MM ASK prices (the price MMs are willing to sell at).
        // A retail SELL matches against MM BID prices. This inversion is
        // fundamental — getting it backwards would fill at the wrong price.
        const bool buying = (order.side == Side::Buy);

        // Collect active MMs with available liquidity, sorted by price.
        struct MmEntry {
            uint32_t mm_id;
            FixedPrice price;  // The relevant price (ask for buy, bid for sell).
            int32_t size;
        };

        std::vector<MmEntry> candidates;
        candidates.reserve(kMaxMMs);

        for (const auto& q : quotes_) {
            if (!q.active) continue;
            if (buying && q.ask_size > 0) {
                candidates.push_back({q.mm_id, q.ask, q.ask_size});
            } else if (!buying && q.bid_size > 0) {
                candidates.push_back({q.mm_id, q.bid, q.bid_size});
            }
        }

        if (candidates.empty()) {
            return core::unexpected(AtsMatchError::NoLiquidity);
        }

        // Sort by price: ascending for buys (best ask = lowest),
        // descending for sells (best bid = highest).
        // Why: for a retail BUY, the best price for the customer is the
        // LOWEST ask (they pay the least). For a retail SELL, the best
        // price is the HIGHEST bid (they receive the most). Sorting puts
        // the best price at front so we allocate there first and only
        // sweep to worse levels if the best level can't fill the full order.
        if (buying) {
            std::sort(candidates.begin(), candidates.end(),
                      [](const MmEntry& a, const MmEntry& b) {
                          return a.price < b.price;
                      });
        } else {
            std::sort(candidates.begin(), candidates.end(),
                      [](const MmEntry& a, const MmEntry& b) {
                          return a.price > b.price;
                      });
        }

        // NBBO compliance check — Reg NMS Rule 611 (trade-through rule).
        // This is non-negotiable and synchronous: we MUST check before
        // confirming any fill. The rule prohibits executing at a price
        // worse than the NBBO (a "trade-through" of a protected quote).
        //   Buy:  fill price (MM ask) must be <= NBBO ask.
        //   Sell: fill price (MM bid) must be >= NBBO bid.
        // If the best MM price fails this check, the entire order is rejected.
        // The to_raw() > 0 guard handles the case where NBBO is not yet set
        // (e.g., pre-open) — we allow fills when there's no NBBO to violate.
        const FixedPrice best_price = candidates.front().price;
        if (buying && nbbo.ask.to_raw() > 0 && best_price > nbbo.ask) {
            return core::unexpected(AtsMatchError::NbboViolation);
        }
        if (!buying && nbbo.bid.to_raw() > 0 && best_price < nbbo.bid) {
            return core::unexpected(AtsMatchError::NbboViolation);
        }

        // Match with pro-rata at each price level.
        std::vector<AtsFill> fills;
        int32_t remaining = order.quantity;

        FixedPrice current_price = candidates.front().price;
        std::size_t level_start = 0;

        while (remaining > 0 && level_start < candidates.size()) {
            // Find all MMs at the current best price level.
            std::size_t level_end = level_start;
            while (level_end < candidates.size() &&
                   candidates[level_end].price == current_price) {
                ++level_end;
            }

            // Total size at this level.
            int64_t total_level_size = 0;
            for (std::size_t i = level_start; i < level_end; ++i) {
                total_level_size += candidates[i].size;
            }

            if (total_level_size <= 0) {
                level_start = level_end;
                if (level_end < candidates.size()) {
                    current_price = candidates[level_end].price;
                }
                continue;
            }

            // NBBO check for this level (subsequent levels may violate).
            if (buying && nbbo.ask.to_raw() > 0 && current_price > nbbo.ask) {
                break; // Can't fill at worse-than-NBBO prices.
            }
            if (!buying && nbbo.bid.to_raw() > 0 && current_price < nbbo.bid) {
                break;
            }

            // Pro-rata allocation at this level.
            // Formula per MM: alloc = floor(level_fill * mm_size / total_level_size)
            // All arithmetic is integer — no floating point, per project convention
            // (fixed-point integers for all price/quantity math).
            // The int64 intermediate prevents overflow: level_fill (int32) *
            // mm_size (int32) can exceed int32 range.
            // Integer division truncates, so sum(allocs) <= level_fill. The
            // difference is the "residual" distributed in the next step.
            int32_t level_fill = static_cast<int32_t>(
                std::min(static_cast<int64_t>(remaining), total_level_size));
            int32_t allocated = 0;

            std::vector<Allocation> allocs;
            allocs.reserve(level_end - level_start);

            for (std::size_t i = level_start; i < level_end; ++i) {
                // Integer pro-rata: (remaining * mm_size) / total_level_size
                // Use int64 intermediate to avoid overflow.
                int64_t share = (static_cast<int64_t>(level_fill) *
                                 static_cast<int64_t>(candidates[i].size)) /
                                total_level_size;
                int32_t alloc_qty = static_cast<int32_t>(
                    std::min(share, static_cast<int64_t>(candidates[i].size)));
                allocs.push_back({candidates[i].mm_id, alloc_qty});
                allocated += alloc_qty;
            }

            // Distribute residual (from integer rounding) by time priority.
            // Because integer division truncates, sum(allocs) < level_fill by
            // up to (N-1) contracts where N = number of MMs at this level.
            // The residual contracts are awarded one-at-a-time to MMs in time
            // priority order (earliest quote first). This is standard practice
            // in options pro-rata markets — pro-rata determines the bulk
            // allocation, time priority breaks the tie for the remainder.
            // Simplification: we use lower mm_id as a proxy for time priority;
            // production would sort by quote timestamp_tsc.
            int32_t residual = level_fill - allocated;
            for (std::size_t i = level_start; i < level_end && residual > 0; ++i) {
                int32_t max_additional = candidates[i].size -
                    allocs[i - level_start].quantity;
                int32_t add = std::min(residual, max_additional);
                allocs[i - level_start].quantity += add;
                residual -= add;
            }

            // Emit fills for this level.
            for (const auto& alloc : allocs) {
                if (alloc.quantity <= 0) continue;

                fills.push_back(AtsFill{
                    .client_order_id = order.client_order_id,
                    .mm_id = alloc.mm_id,
                    .price = current_price,
                    .quantity = alloc.quantity,
                    .nbbo_bid = nbbo.bid,
                    .nbbo_ask = nbbo.ask,
                    .timestamp_tsc = order.timestamp_tsc,
                });

                remaining -= alloc.quantity;

                // Decrement MM's quoted size to reflect the fill.
                // This is essential: without it, the same liquidity could be
                // double-counted if the order sweeps to the next price level
                // or if another retail order arrives before the MM sends a
                // new quote update. The MM's quote is the source of truth for
                // available liquidity until the MM explicitly refreshes it.
                if (alloc.mm_id < kMaxMMs) {
                    if (buying) {
                        quotes_[alloc.mm_id].ask_size -= alloc.quantity;
                    } else {
                        quotes_[alloc.mm_id].bid_size -= alloc.quantity;
                    }
                }
            }

            // Advance to next price level.
            level_start = level_end;
            if (level_end < candidates.size()) {
                current_price = candidates[level_end].price;
            }
        }

        // If we couldn't fill anything at all — no liquidity at valid prices.
        if (fills.empty()) {
            return core::unexpected(AtsMatchError::NoLiquidity);
        }

        return fills;
    }

    /// Staleness-aware matching. If the NBBO timestamp is older than
    /// max_staleness_ns from current_time_ns, reject the order rather than
    /// risk a Rule 611 violation with stale price data.
    ///
    /// This addresses the OPRA backpressure scenario: if the OPRA handler
    /// falls behind, the NBBO table may hold stale data. The matching engine
    /// must NOT fill against a stale NBBO — the actual market may have moved.
    [[nodiscard]] core::expected<std::vector<AtsFill>, AtsMatchError>
    match_retail_order_checked(const RetailOrder& order,
                               const NbboEntry& nbbo,
                               uint64_t current_time_ns,
                               uint64_t max_staleness_ns = 500'000'000) {
        // 500ms default staleness threshold. Configurable per deployment.
        if (nbbo.timestamp_ns > 0 &&
            current_time_ns > nbbo.timestamp_ns &&
            (current_time_ns - nbbo.timestamp_ns) > max_staleness_ns) {
            return core::unexpected(AtsMatchError::StaleNbbo);
        }
        return match_retail_order(order, nbbo);
    }

    /// Read-only access to a specific MM's quote.
    [[nodiscard]] const MmQuote& quote(uint32_t mm_id) const noexcept {
        return quotes_[mm_id < kMaxMMs ? mm_id : 0];
    }

private:
    std::array<MmQuote, kMaxMMs> quotes_{};
};

} // namespace options_ats_core::market_data
