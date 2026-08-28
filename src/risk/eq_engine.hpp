#pragma once
/// eq_engine.hpp — Execution Quality measurement engine.
///
/// The EQ engine is the platform's trust surface. Market makers are scored on
/// per-name rolling metrics and flow is reallocated periodically based on
/// relative performance. If MMs don't trust the scoring, they leave. If retail
/// brokers don't trust the metrics, they don't send flow.
///
/// Metrics per (MM, underlying) pair:
///   - Price improvement vs NBBO (in ticks)
///   - Fill rate (fills / opportunities)
///   - Response time (quote-to-fill latency)
///   - Composite EQ score (weighted blend)

#include "core/expected.hpp"
#include "core/fixed_price.hpp"
#include "core/safe_numeric.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace options_ats_core::risk {

using core::FixedPrice;

static constexpr uint32_t kMaxMMs = 16;

/// Raw metrics accumulated over a scoring window.
/// These counters are the inputs to the EQ scoring algorithm. They are
/// accumulated per (MM, underlying) pair over a rolling window (typically
/// one month), then normalized and weighted to produce composite scores.
struct EqMetrics {
    uint32_t mm_id{0};
    uint64_t fill_count{0};            // Number of fills this MM participated in.
    uint64_t opportunity_count{0};     // Times this MM was quoting at BBO when
                                       // retail flow arrived. An "opportunity" means
                                       // the MM had a chance to fill — it was at the
                                       // best price and had size. If they filled,
                                       // fill_count increments too. If they didn't
                                       // (e.g., quote was pulled, or size was
                                       // consumed by other MMs in pro-rata), only
                                       // opportunity_count increments. The ratio
                                       // fill_count/opportunity_count = fill rate.
    int64_t total_improvement_raw{0};  // Sum of (NBBO - fill_price) in FixedPrice raw units.
    uint64_t total_response_ns{0};     // Sum of quote-to-fill latency in nanoseconds.

    /// Fill rate as basis points (0-10000). No floating point.
    /// Why basis points instead of percentage: basis points are integers
    /// (1 bps = 0.01%), so we avoid floating-point arithmetic entirely.
    /// This is consistent with the project convention of integer-only math
    /// for all financial quantities (fixed-point integers).
    /// 10000 bps = 100% fill rate.
    [[nodiscard]] constexpr uint32_t fill_rate_bps() const noexcept {
        if (opportunity_count == 0) return 0;
        return static_cast<uint32_t>(
            (fill_count * 10'000ULL) / opportunity_count);
    }

    /// Average price improvement in raw FixedPrice units.
    [[nodiscard]] constexpr int64_t avg_improvement_raw() const noexcept {
        if (fill_count == 0) return 0;
        return total_improvement_raw / static_cast<int64_t>(fill_count);
    }

    /// Average response time in nanoseconds.
    [[nodiscard]] constexpr uint64_t avg_response_ns() const noexcept {
        if (fill_count == 0) return 0;
        return total_response_ns / fill_count;
    }
};

/// Scored result for one MM: composite EQ score in basis points (0-10000).
/// composite_bps IS the trust surface — this single number determines how
/// much retail flow each MM receives in the next allocation period. Higher
/// composite = more flow. This is the incentive mechanism that keeps MMs
/// competing on execution quality. If MMs don't trust the scoring methodology,
/// they leave the platform. If retail brokers don't trust it, they stop
/// routing flow. Transparency of this score is existential for the ATS.
struct EqScore {
    uint32_t mm_id{0};
    uint32_t composite_bps{0};   // The trust surface: higher = better EQ = more flow.
    uint32_t fill_rate_bps{0};
    int64_t avg_improvement_raw{0};
    uint64_t avg_response_ns{0};
    uint32_t rank{0};            // 1 = best.
};

/// Weights for composite score (must sum to 10000 bps = 100%).
/// Default split: 50% price improvement, 30% fill rate, 20% response time.
/// Rationale:
///   - Price improvement (50%): the primary value proposition to retail —
///     better prices than lit exchanges. Weighted highest because this is
///     what retail brokers evaluate when choosing where to route flow.
///   - Fill rate (30%): MMs that consistently fill when they're at BBO are
///     more valuable than MMs that fade (pull quotes) when flow arrives.
///   - Response time (20%): faster MMs reduce slippage risk but this is
///     less visible to the end customer, hence lowest weight.
/// Weights are in bps and MUST sum to 10000 (100%). This invariant is
/// enforced by valid() and checked before every score computation.
/// Using bps avoids floating-point weights entirely.
struct EqWeights {
    uint32_t price_improvement_weight{5000};  // 50%
    uint32_t fill_rate_weight{3000};          // 30%
    uint32_t response_time_weight{2000};      // 20%

    [[nodiscard]] constexpr bool valid() const noexcept {
        return price_improvement_weight + fill_rate_weight +
               response_time_weight == 10'000;
    }
};

enum class EqError : uint8_t {
    InvalidWeights,
    NoData,
    InvalidMmId,
};

[[nodiscard]] constexpr std::string_view to_string(EqError e) noexcept {
    switch (e) {
        case EqError::InvalidWeights: return "InvalidWeights";
        case EqError::NoData:         return "NoData";
        case EqError::InvalidMmId:    return "InvalidMmId";
    }
    return "Unknown";
}

/// Per-name (underlying) EQ scoring engine.
class EqEngine {
public:
    /// Record a fill event for scoring.
    /// Price improvement sign convention:
    ///   improvement = nbbo_price - fill_price (in raw FixedPrice units).
    ///   Positive value = the customer got a BETTER price than the NBBO.
    ///   For buys: caller passes nbbo_price = NBBO ask. If fill < NBBO ask,
    ///     improvement is positive (customer paid less than the national best).
    ///   For sells: caller passes nbbo_price = NBBO bid. Convention is inverted
    ///     at the call site so that positive always means "better for customer."
    ///   Zero or negative = no improvement or price dis-improvement (rare on ATS).
    /// response_ns is the latency from the moment retail flow arrived to the
    /// moment the MM's quote was matched (quote-to-fill). Lower is better.
    void record_fill(uint32_t mm_id, FixedPrice fill_price,
                     FixedPrice nbbo_price, uint64_t response_ns) noexcept {
        if (mm_id >= kMaxMMs) return;
        auto& m = metrics_[mm_id];
        m.mm_id = mm_id;
        ++m.fill_count;

        // Price improvement = NBBO - fill_price (positive = client got better price).
        // For buys: improvement = NBBO ask - fill price (lower fill = better).
        // For sells: improvement = fill price - NBBO bid (higher fill = better).
        // Caller passes the signed improvement already computed.
        int64_t improvement = nbbo_price.to_raw() - fill_price.to_raw();
        m.total_improvement_raw += improvement;
        m.total_response_ns += response_ns;
    }

    /// Record that an MM was at BBO when flow arrived (opportunity).
    /// An "opportunity" means: at the instant retail flow arrived, this MM
    /// was quoting at the best price (BBO) with available size. It had the
    /// chance to fill. Whether it actually filled depends on pro-rata
    /// allocation and whether the MM's quote was still active at match time.
    /// This is called for EVERY MM at BBO, even those that ultimately get
    /// filled — record_fill is called separately for actual fills.
    /// fill_rate = fill_count / opportunity_count. An MM that is always at
    /// BBO but frequently fades (pulls quotes before fill) will have a low
    /// fill rate, which penalizes its composite score.
    void record_opportunity(uint32_t mm_id) noexcept {
        if (mm_id >= kMaxMMs) return;
        metrics_[mm_id].mm_id = mm_id;
        ++metrics_[mm_id].opportunity_count;
    }

    /// Compute composite scores and rank all MMs.
    [[nodiscard]] core::expected<std::vector<EqScore>, EqError>
    compute_scores(const EqWeights& weights) const {
        if (!weights.valid()) {
            return core::unexpected(EqError::InvalidWeights);
        }

        // Collect MMs with data.
        std::vector<EqScore> scores;
        for (const auto& m : metrics_) {
            if (m.fill_count == 0 && m.opportunity_count == 0) continue;
            scores.push_back(EqScore{
                .mm_id = m.mm_id,
                .composite_bps = 0,
                .fill_rate_bps = m.fill_rate_bps(),
                .avg_improvement_raw = m.avg_improvement_raw(),
                .avg_response_ns = m.avg_response_ns(),
                .rank = 0,
            });
        }

        if (scores.empty()) {
            return core::unexpected(EqError::NoData);
        }

        // Normalize each dimension to 0-10000 bps range relative to peers.
        // Best performer gets 10000, worst gets 0 (or proportional).
        normalize_and_weight(scores, weights);

        // Rank by composite (descending).
        std::sort(scores.begin(), scores.end(),
                  [](const EqScore& a, const EqScore& b) {
                      return a.composite_bps > b.composite_bps;
                  });

        for (uint32_t i = 0; i < scores.size(); ++i) {
            scores[i].rank = i + 1;
        }

        return scores;
    }

    /// Get raw metrics for a specific MM.
    [[nodiscard]] const EqMetrics& metrics(uint32_t mm_id) const noexcept {
        return metrics_[mm_id < kMaxMMs ? mm_id : 0];
    }

    /// Reset all metrics (start of new scoring window).
    void reset() noexcept {
        metrics_ = {};
    }

private:
    /// Normalize raw metrics to a common 0-10000 bps scale and compute
    /// the weighted composite score.
    ///
    /// Approach: min-max normalization relative to peers in this scoring window.
    /// Each dimension is mapped from [worst_mm, best_mm] -> [0, 10000].
    /// This means scores are RELATIVE, not absolute — an MM's score depends
    /// on how it performed vs other MMs in the same window. This is intentional:
    /// it creates a competitive dynamic where MMs must outperform each other,
    /// not just meet an absolute threshold.
    ///
    /// If all MMs have the same value for a dimension, they all get 5000 (midpoint)
    /// to avoid division by zero and to not arbitrarily penalize or reward.
    ///
    /// Single-MM edge case: if only one MM has data, it gets a perfect 10000.
    void normalize_and_weight(std::vector<EqScore>& scores,
                              const EqWeights& weights) const {
        if (scores.size() <= 1) {
            if (!scores.empty()) scores[0].composite_bps = 10'000;
            return;
        }

        // Find min/max for each dimension across all scored MMs.
        int64_t min_pi = scores[0].avg_improvement_raw;
        int64_t max_pi = min_pi;
        uint32_t min_fr = scores[0].fill_rate_bps;
        uint32_t max_fr = min_fr;
        uint64_t min_rt = scores[0].avg_response_ns;
        uint64_t max_rt = min_rt;

        for (const auto& s : scores) {
            min_pi = std::min(min_pi, s.avg_improvement_raw);
            max_pi = std::max(max_pi, s.avg_improvement_raw);
            min_fr = std::min(min_fr, s.fill_rate_bps);
            max_fr = std::max(max_fr, s.fill_rate_bps);
            min_rt = std::min(min_rt, s.avg_response_ns);
            max_rt = std::max(max_rt, s.avg_response_ns);
        }

        for (auto& s : scores) {
            // Price improvement: higher is better. Normalize to 0-10000.
            // Formula: (value - min) / (max - min) * 10000
            uint32_t pi_score = 5000; // default if all MMs have same improvement
            if (max_pi != min_pi) {
                pi_score = static_cast<uint32_t>(
                    ((s.avg_improvement_raw - min_pi) * 10'000) /
                    (max_pi - min_pi));
            }

            // Fill rate: higher is better. Already in bps.
            uint32_t fr_score = 5000;
            if (max_fr != min_fr) {
                fr_score = static_cast<uint32_t>(
                    (static_cast<uint64_t>(s.fill_rate_bps - min_fr) * 10'000) /
                    (max_fr - min_fr));
            }

            // Response time: LOWER is better, so the normalization is INVERTED.
            // Formula: (max - value) / (max - min) * 10000
            // The fastest MM (lowest ns) gets 10000, the slowest gets 0.
            // This inversion is necessary because for the other two dimensions
            // higher raw values are better, but for latency lower is better.
            uint32_t rt_score = 5000;
            if (max_rt != min_rt) {
                rt_score = static_cast<uint32_t>(
                    ((max_rt - s.avg_response_ns) * 10'000) /
                    (max_rt - min_rt));
            }

            // Weighted composite: sum(dimension_score * weight) / 10000.
            // Each dimension_score is 0-10000, each weight is 0-10000, and
            // weights sum to 10000. So the result is in 0-10000 bps range.
            // uint64 intermediate prevents overflow (10000 * 10000 = 10^8,
            // fits in uint64 but not uint32).
            s.composite_bps = static_cast<uint32_t>(
                (static_cast<uint64_t>(pi_score) * weights.price_improvement_weight +
                 static_cast<uint64_t>(fr_score) * weights.fill_rate_weight +
                 static_cast<uint64_t>(rt_score) * weights.response_time_weight) /
                10'000);
        }
    }

    std::array<EqMetrics, kMaxMMs> metrics_{};
};

} // namespace options_ats_core::risk
