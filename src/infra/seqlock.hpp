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

#include <atomic>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace options_ats_core::infra {

// requires(trivially_copyable): The seqlock copies T via raw byte-level
// assignment (data_ = value). A non-trivially-copyable type — one with a
// vtable, heap-allocated members, or a non-trivial copy constructor — would
// be unsafe to copy this way. For example, a type with a std::string member
// would have its internal pointer copied without proper construction,
// leading to double-free or use-after-free. Trivially copyable guarantees
// memcpy-equivalent semantics, which is what we need for the lock-free
// read path where we may read a partially-written value and discard it.
template <typename T>
    requires(std::is_trivially_copyable_v<T>)
class Seqlock {
public:
    Seqlock() noexcept : sequence_{0} {
        std::memset(&data_, 0, sizeof(T));
    }

    /// Writer: store a new value. Single writer only — no concurrent writes.
    ///
    /// Full protocol:
    ///   1. Store odd sequence  -> signals "write in progress" to readers.
    ///   2. Thread fence        -> ensures the odd sequence is globally visible
    ///                             BEFORE we start writing data.
    ///   3. Write data          -> non-atomic copy of T.
    ///   4. Thread fence        -> ensures the data write is globally visible
    ///                             BEFORE we store the final even sequence.
    ///   5. Store even sequence -> signals "write complete" to readers.
    ///
    /// Why atomic_thread_fence instead of relying on the release semantics of
    /// the atomic stores alone: data_ is NOT an atomic variable. The compiler
    /// is free to reorder a non-atomic write around atomic stores unless a
    /// fence explicitly prevents it. The fence acts as a full compiler and
    /// hardware barrier, ensuring the non-atomic data_ write cannot migrate
    /// above step 1 or below step 5. Without the fences, a reader could see
    /// the even (complete) sequence but read partially-written data.
    void store(const T& value) noexcept {
        // Odd sequence = write in progress. Readers will retry.
        uint64_t seq = sequence_.load(std::memory_order_relaxed);
        sequence_.store(seq + 1, std::memory_order_release); // Step 1: Odd -> writing.
        std::atomic_thread_fence(std::memory_order_release); // Step 2: Fence before data write.
        data_ = value;                                        // Step 3: Non-atomic data copy.
        std::atomic_thread_fence(std::memory_order_release); // Step 4: Fence after data write.
        sequence_.store(seq + 2, std::memory_order_release); // Step 5: Even -> done.
    }

    /// Reader: load the latest value. Retries on writer contention.
    ///
    /// Retry protocol:
    ///   1. Read sequence (acquire — synchronizes with writer's release store).
    ///   2. If odd, a write is in progress — spin back to step 1.
    ///   3. Acquire fence — ensures we see the data the writer wrote before
    ///      it published the even sequence. Without this, the CPU could
    ///      speculatively load stale data_ from before the writer's store.
    ///   4. Read data (non-atomic copy of T).
    ///   5. Acquire fence — ensures the data read completes before we re-check
    ///      the sequence. Without this, the compiler/CPU could reorder the
    ///      sequence re-read before the data read, defeating the check.
    ///   6. Re-read sequence.
    ///   7. If seq changed since step 1, a write started or completed during
    ///      our read — the data in `result` may be torn/inconsistent. Discard
    ///      it and retry from step 1.
    [[nodiscard]] T load() const noexcept {
        T result;
        uint64_t seq1;
        do {
            seq1 = sequence_.load(std::memory_order_acquire); // Step 1: read sequence.
            if (seq1 & 1) continue;                           // Step 2: odd = write in progress, spin.
            std::atomic_thread_fence(std::memory_order_acquire); // Step 3: fence before data read.
            result = data_;                                      // Step 4: read data (non-atomic).
            std::atomic_thread_fence(std::memory_order_acquire); // Step 5: fence after data read.
        } while (sequence_.load(std::memory_order_acquire) != seq1); // Steps 6-7: re-check sequence.
        return result;
    }

    /// Reader: load with sequence number for staleness detection.
    [[nodiscard]] std::pair<T, uint64_t> load_with_seq() const noexcept {
        T result;
        uint64_t seq1;
        do {
            seq1 = sequence_.load(std::memory_order_acquire);
            if (seq1 & 1) continue;
            std::atomic_thread_fence(std::memory_order_acquire);
            result = data_;
            std::atomic_thread_fence(std::memory_order_acquire);
        } while (sequence_.load(std::memory_order_acquire) != seq1);
        return {result, seq1};
    }

    /// Current sequence number (even = no write in progress).
    [[nodiscard]] uint64_t sequence() const noexcept {
        return sequence_.load(std::memory_order_acquire);
    }

private:
    // sequence_ is aligned to its own 64-byte cache line to avoid false
    // sharing with data_. Readers spin on sequence_ (high read frequency);
    // if it shared a cache line with data_, every writer store to data_
    // would invalidate the readers' cached copy of sequence_, causing
    // unnecessary cache misses and stalls on the reader cores.
    alignas(64) std::atomic<uint64_t> sequence_;
    T data_;
};

} // namespace options_ats_core::infra
