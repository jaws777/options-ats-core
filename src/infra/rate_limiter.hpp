#pragma once
/// rate_limiter.hpp — Lock-free token bucket rate limiter.
///
/// Use case: MMs sending too many quote updates per second. Rate limit
/// per MM without blocking the hot path. Uses atomic compare_exchange
/// to decrement tokens — no mutex.
///
/// The token bucket is refilled by a timer thread calling refill().
/// The hot path only calls try_consume() which is lock-free.

#include <atomic>
#include <cstdint>

namespace options_ats_core::infra {

class RateLimiter {
public:
    /// Token bucket model: bucket starts full (tokens_ == tokens_per_second_).
    /// Each try_consume() removes a token; refill() restores to max once per
    /// second from a timer thread. This decouples the refill cadence (timer
    /// thread, ~1 Hz) from the consume path (hot path, called per quote update).
    /// The hot path never blocks on the timer — it only does a CAS on an atomic.
    explicit RateLimiter(uint32_t tokens_per_second = 10'000) noexcept
        : tokens_per_second_{tokens_per_second},
          tokens_{tokens_per_second} {}

    /// Hot path: attempt to consume one token. Returns true if allowed.
    /// Lock-free — uses compare_exchange.
    [[nodiscard]] bool try_consume() noexcept {
        // CAS (Compare-And-Swap) loop — lock-free, no syscall, no mutex.
        // 1. Load current token count. Relaxed is fine here because the CAS
        //    below provides the actual synchronization barrier.
        uint32_t current = tokens_.load(std::memory_order_relaxed);
        do {
            if (current == 0) return false; // Rate limited.
            // 2. Attempt to atomically decrement: if tokens_ still equals
            //    `current`, write `current - 1`. If another thread consumed a
            //    token between our load and this CAS, compare_exchange_weak
            //    fails and reloads the actual value into `current` — we retry
            //    with the fresh value. "Weak" is intentional: spurious failures
            //    are harmless (just retry) and weak is cheaper on ARM/LL-SC.
            // 3. acq_rel: acquire semantics on success so subsequent reads see
            //    data published before the refill; release so our decrement is
            //    visible to other threads.
            // O(1) amortized — contention causes retries but no blocking.
        } while (!tokens_.compare_exchange_weak(
            current, current - 1, std::memory_order_acq_rel));
        return true;
    }

    /// Consume N tokens at once. Returns true if all consumed.
    [[nodiscard]] bool try_consume(uint32_t n) noexcept {
        uint32_t current = tokens_.load(std::memory_order_relaxed);
        do {
            if (current < n) return false;
        } while (!tokens_.compare_exchange_weak(
            current, current - n, std::memory_order_acq_rel));
        return true;
    }

    /// Called by a timer thread once per second — NOT on the hot path.
    /// Resets the bucket to the current rate limit (full refill, not additive).
    /// The store with release ordering ensures the new token count is visible
    /// to hot-path threads that load with acquire in try_consume().
    /// Note: in production you might drive refill from a TSC-based ticker
    /// instead of a timer thread to avoid OS timer jitter (~1-10us).
    void refill() noexcept {
        tokens_.store(tokens_per_second_.load(std::memory_order_relaxed),
                      std::memory_order_release);
    }

    /// Hot-reload path: update the rate limit without process restart.
    /// New rate takes effect on the next refill() call — no lock needed.
    /// Release ordering ensures the new value is visible to the timer thread
    /// when it next loads tokens_per_second_ in refill().
    void set_rate(uint32_t tokens_per_second) noexcept {
        tokens_per_second_.store(tokens_per_second, std::memory_order_release);
    }

    [[nodiscard]] uint32_t remaining() const noexcept {
        return tokens_.load(std::memory_order_acquire);
    }

    [[nodiscard]] uint32_t rate() const noexcept {
        return tokens_per_second_.load(std::memory_order_acquire);
    }

private:
    std::atomic<uint32_t> tokens_per_second_; // Read-only on the hot path (only written by set_rate).
    // tokens_ is on its own cache line (alignas(64)) because it is written by
    // BOTH the hot path (try_consume CAS) and the timer thread (refill store).
    // Without this padding, tokens_ and tokens_per_second_ would share a cache
    // line, causing false sharing: the timer thread's refill() store to tokens_
    // would invalidate the cache line on every core reading tokens_per_second_,
    // even though tokens_per_second_ rarely changes. 64 bytes = x86 cache line.
    alignas(64) std::atomic<uint32_t> tokens_;
};

} // namespace options_ats_core::infra
