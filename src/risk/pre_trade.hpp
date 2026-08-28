#pragma once
/// pre_trade.hpp — Inline pre-trade risk checks using atomic counters.
///
/// All checks are synchronous on the critical path. No mutex.
/// Position limits and order rate use compare_exchange — if the check
/// fails, the order is rejected immediately without blocking other orders.
///
/// Convention: hard limits (position, notional, order rate) are
/// enforced via atomic counters with compare_exchange — never via a lock.
/// Verification references:
/// - tests/unit/test_pre_trade.cpp
/// - tests/gtest/gtest_pre_trade.cpp

#include "core/expected.hpp"
#include "core/fixed_price.hpp"
#include "core/safe_numeric.hpp"

#include <atomic>
#include <cstdint>
#include <string_view>

namespace options_ats_core::risk {

using core::FixedPrice;

enum class RiskReject : uint8_t {
    PositionLimitBreached,
    NotionalLimitBreached,
    OrderRateLimitBreached,
};

[[nodiscard]] constexpr std::string_view to_string(RiskReject e) noexcept {
    switch (e) {
        case RiskReject::PositionLimitBreached:  return "PositionLimitBreached";
        case RiskReject::NotionalLimitBreached:  return "NotionalLimitBreached";
        case RiskReject::OrderRateLimitBreached: return "OrderRateLimitBreached";
    }
    return "Unknown";
}

/// Risk limits — hot-reloadable parameters loaded from the control plane.
/// These are the "source of truth" values that get pushed into the per-field
/// atomics inside PreTradeRiskCheck via update_limits(). The matching thread
/// reads the new values on its next risk check via a relaxed atomic load,
/// so limit changes take effect within one order cycle — no restart needed.
struct RiskLimits {
    int64_t max_position{100'000};         // Max net contracts.
    int64_t max_notional_raw{0};           // Max notional in FixedPrice raw units.
    uint32_t max_orders_per_second{10'000};
};

/// Per-account (or per-MM) risk state. Lock-free.
/// Each field on its own cache line to avoid false sharing
/// between the position update path and the rate check path.
//
// alignas(64): Forces the entire object to start on a 64-byte cache line
// boundary. This is the x86-64 L1 cache line size. Without this, the
// compiler could place the object mid-cache-line, causing the first member
// to straddle two cache lines and defeating our per-field alignment below.
class alignas(64) PreTradeRiskCheck {
public:
    /// Check and update position. Returns reject reason if limit breached.
    /// Uses compare_exchange loop: read current, compute new, CAS.
    [[nodiscard]] core::expected<int64_t, RiskReject>
    check_position(int64_t delta) noexcept {
        // Step 1: Load the current net position. Relaxed is fine here because
        // the CAS below will re-check this value atomically — if it changed
        // between this load and the CAS, the CAS fails and we retry with
        // the updated value (CAS writes the actual current value into `current`).
        int64_t current = net_position_.load(std::memory_order_relaxed);
        int64_t desired{0};
        do {
            // Step 2: Compute the desired new position with overflow protection.
            auto sum = core::checked_add(current, delta);
            if (!sum.has_value()) {
                return core::unexpected(RiskReject::PositionLimitBreached);
            }
            desired = *sum;

            // Step 3: Check against the limit. The limit is loaded relaxed
            // because it's updated infrequently from the control plane via
            // update_limits(). We don't need to synchronize with the limit
            // writer — seeing a slightly stale limit is acceptable and avoids
            // adding a memory barrier to the hot path.
            const int64_t limit = max_position_.load(std::memory_order_relaxed);
            if (desired > limit || desired < -limit) {
                return core::unexpected(RiskReject::PositionLimitBreached);
            }
        } while (!net_position_.compare_exchange_weak(
            // Step 4: CAS — atomically swap net_position_ from `current` to
            // `desired`. If another thread (e.g., a second matching thread or
            // a fill callback) modified net_position_ between our load and now,
            // the CAS fails, writes the actual current value into `current`,
            // and we loop back to recompute `desired` with the true position.
            //
            // compare_exchange_weak (not strong): weak can spuriously fail even
            // when current matches. This is cheaper on ARM (no LL/SC loop
            // internally) and on x86 the difference is negligible. In a retry
            // loop like this, a spurious failure just means one extra iteration
            // — no correctness impact, and the loop is expected to succeed in
            // 1-2 tries under normal contention.
            //
            // memory_order_acq_rel: On success, we need both:
            //   - acquire: so subsequent reads see any state written by the
            //     thread whose CAS we displaced (happens-before relationship).
            //   - release: so our write to net_position_ is visible to the
            //     next thread that loads it.
            current, desired, std::memory_order_acq_rel));

        return desired;
    }

    /// Check and update notional exposure.
    [[nodiscard]] core::expected<int64_t, RiskReject>
    check_notional(int64_t notional_delta_raw) noexcept {
        int64_t current = net_notional_raw_.load(std::memory_order_relaxed);
        int64_t desired{0};
        do {
            auto sum = core::checked_add(current, notional_delta_raw);
            if (!sum.has_value()) {
                return core::unexpected(RiskReject::NotionalLimitBreached);
            }
            desired = *sum;

            const int64_t limit = max_notional_raw_.load(std::memory_order_relaxed);
            if (limit > 0 && (desired > limit || desired < -limit)) {
                return core::unexpected(RiskReject::NotionalLimitBreached);
            }
        } while (!net_notional_raw_.compare_exchange_weak(
            current, desired, std::memory_order_acq_rel));

        return desired;
    }

    /// Check order rate (simple counter — caller resets per window).
    [[nodiscard]] core::expected<uint32_t, RiskReject>
    check_order_rate() noexcept {
        uint32_t current = order_count_.load(std::memory_order_relaxed);
        const uint32_t limit = max_orders_per_second_.load(std::memory_order_relaxed);
        uint32_t desired{0};
        do {
            if (current >= limit) {
                return core::unexpected(RiskReject::OrderRateLimitBreached);
            }
            desired = current + 1;
        } while (!order_count_.compare_exchange_weak(
            current, desired, std::memory_order_acq_rel));

        return desired;
    }

    /// Hot-reload limits without process restart.
    /// Called from the control plane (e.g., a config reload thread or admin RPC
    /// handler) — NOT from the matching thread. The matching thread picks up
    /// the new limits on its next risk check via the relaxed load inside the
    /// CAS loop above. No lock is needed because each limit is a single atomic
    /// store — there is no multi-field consistency requirement (updating
    /// max_position and max_notional independently is safe; they guard
    /// independent counters).
    ///
    /// memory_order_release: ensures the store is visible to any thread that
    /// subsequently loads with acquire or participates in a CAS. The matching
    /// thread uses relaxed loads for the limits (acceptable staleness), but
    /// release here guarantees the value is eventually visible and not
    /// reordered with prior control-plane writes.
    void update_limits(const RiskLimits& limits) noexcept {
        max_position_.store(limits.max_position, std::memory_order_release);
        max_notional_raw_.store(limits.max_notional_raw, std::memory_order_release);
        max_orders_per_second_.store(limits.max_orders_per_second,
                                     std::memory_order_release);
    }

    /// Reset order rate counter (called once per second by timer thread).
    void reset_order_count() noexcept {
        order_count_.store(0, std::memory_order_release);
    }

    // --- Queries ---
    [[nodiscard]] int64_t net_position() const noexcept {
        return net_position_.load(std::memory_order_acquire);
    }
    [[nodiscard]] int64_t net_notional_raw() const noexcept {
        return net_notional_raw_.load(std::memory_order_acquire);
    }
    [[nodiscard]] uint32_t order_count() const noexcept {
        return order_count_.load(std::memory_order_acquire);
    }

private:
    // Each member gets its own cache line (alignas(64)) to prevent false
    // sharing. False sharing occurs when two threads write to different
    // variables that happen to share the same 64-byte cache line — the CPU
    // bounces the entire line between cores, adding hundreds of nanoseconds
    // of latency per access. By isolating each atomic on its own line, a
    // write to net_position_ never invalidates the cache line holding
    // order_count_, even though both are written on the hot path.
    //
    // Limit vs. counter separation: the limit fields (max_position_, etc.)
    // are read-heavy — loaded every risk check but written only on config
    // reload. The counters (net_position_, order_count_) are write-heavy —
    // CAS'd on every order. Putting them on separate cache lines means the
    // frequent counter CAS operations do not invalidate the cache line
    // holding the limit, so the limit load stays in L1.

    alignas(64) std::atomic<int64_t> net_position_{0};     // Write-heavy: CAS'd per order.
    alignas(64) std::atomic<int64_t> max_position_{100'000}; // Read-heavy: loaded per check, written on reload.

    alignas(64) std::atomic<int64_t> net_notional_raw_{0};   // Write-heavy.
    alignas(64) std::atomic<int64_t> max_notional_raw_{0};   // Read-heavy.

    alignas(64) std::atomic<uint32_t> order_count_{0};             // Write-heavy: CAS'd per order.
    alignas(64) std::atomic<uint32_t> max_orders_per_second_{10'000}; // Read-heavy: loaded per check.
};

} // namespace options_ats_core::risk
