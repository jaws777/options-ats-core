#pragma once
/// seqlock.hpp — Lock-free single-writer multi-reader latest-value store.
///
/// Use case: NBBO table. OPRA handler (single writer) updates the latest
/// BBO for a series. Matching threads (multiple readers) read the latest
/// value without any lock or contention.
///
/// Protocol:
///   Writer: increment sequence (odd = write in progress), write data,
///           increment sequence again (even = write complete).
///   Reader: read sequence (must be even), read data, re-read sequence
///           (must match). If mismatch, retry.
///
/// No lock, no contention. Writer never blocks. Readers never block.
/// Readers may retry if they race with a write — but writes are rare
/// relative to reads (NBBO changes < 1M/sec, reads = every match).
/// Verification references:
/// - tests/gtest/gtest_seqlock.cpp
/// - tests/unit/test_infra_extras.cpp

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <utility>

namespace options_ats_core::infra {

// requires(trivially_copyable): The seqlock copies T through its object
// representation using atomic bytes. A non-trivially-copyable type — one with a
// vtable, heap-allocated members, or a non-trivial copy constructor — would
// be unsafe to copy this way. For example, a type with a std::string member
// would have its internal pointer copied without proper construction,
// leading to double-free or use-after-free. Trivially copyable guarantees
// memcpy-equivalent semantics, which is what we need for the lock-free
// read path where we may assemble a partial snapshot and discard it.
template <typename T>
    requires(std::is_trivially_copyable_v<T>)
class Seqlock {
public:
    Seqlock() noexcept : sequence_{0} {
        for (auto& byte : data_) {
            byte.store(std::byte{0}, std::memory_order_relaxed);
        }
    }

    /// Writer: store a new value. Single writer only — no concurrent writes.
    ///
    /// The payload uses lock-free atomic bytes. A conventional C++ seqlock that
    /// concurrently copies a non-atomic T has a data race even when fences are
    /// present, which is undefined behavior. Sequential consistency places the
    /// odd marker, every payload byte, and the final even marker in one global
    /// order; readers that observe the same even marker on both sides therefore
    /// have a coherent snapshot.
    void store(const T& value) noexcept {
        std::array<std::byte, sizeof(T)> bytes{};
        std::memcpy(bytes.data(), &value, sizeof(T));

        sequence_.fetch_add(1, std::memory_order_seq_cst); // Odd: write in progress.
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            data_[i].store(bytes[i], std::memory_order_seq_cst);
        }
        sequence_.fetch_add(1, std::memory_order_seq_cst); // Even: write complete.
    }

    /// Reader: load the latest value. Retries on writer contention.
    ///
    /// Retry protocol:
    ///   1. Read sequence.
    ///   2. If odd, a write is in progress — spin back to step 1.
    ///   3. Read the atomic payload bytes.
    ///   4. Re-read sequence.
    ///   5. If seq changed since step 1, a write started or completed during
    ///      our read — the data in `result` may be torn/inconsistent. Discard
    ///      it and retry from step 1.
    [[nodiscard]] T load() const noexcept {
        return load_snapshot().first;
    }

    /// Reader: load with sequence number for staleness detection.
    [[nodiscard]] std::pair<T, uint64_t> load_with_seq() const noexcept {
        return load_snapshot();
    }

    /// Current sequence number (even = no write in progress).
    [[nodiscard]] uint64_t sequence() const noexcept {
        return sequence_.load(std::memory_order_acquire);
    }

private:
    static_assert(std::atomic<std::byte>::is_always_lock_free,
                  "Seqlock requires lock-free byte atomics");

    [[nodiscard]] std::pair<T, uint64_t> load_snapshot() const noexcept {
        std::array<std::byte, sizeof(T)> bytes{};
        uint64_t seq1{0};
        for (;;) {
            seq1 = sequence_.load(std::memory_order_seq_cst);
            if (seq1 & 1) continue;
            for (std::size_t i = 0; i < bytes.size(); ++i) {
                bytes[i] = data_[i].load(std::memory_order_seq_cst);
            }
            if (sequence_.load(std::memory_order_seq_cst) == seq1) break;
        }

        T result;
        std::memcpy(&result, bytes.data(), sizeof(T));
        return {result, seq1};
    }

    // sequence_ is aligned to its own 64-byte cache line to avoid false
    // sharing with data_. Readers spin on sequence_ (high read frequency);
    // if it shared a cache line with data_, every writer store to data_
    // would invalidate the readers' cached copy of sequence_, causing
    // unnecessary cache misses and stalls on the reader cores.
    alignas(64) std::atomic<uint64_t> sequence_;
    alignas(64) std::array<std::atomic<std::byte>, sizeof(T)> data_;
};

} // namespace options_ats_core::infra
