#pragma once
/// spsc_queue.hpp — Lock-free single-producer single-consumer bounded queue.
///
/// Design choices:
/// - Power-of-2 capacity enforced via requires clause. Enables mask-based
///   indexing (bitwise AND) instead of modulo division.
/// - alignas(64) on head_ and tail_ prevents false sharing. These are written
///   by different threads — without padding they may share a cache line,
///   costing ~40-70ns per access on modern x86.
/// - memory_order_release on producer writes, memory_order_acquire on consumer
///   reads. seq_cst is unnecessary: head is only written by consumer, tail
///   only by producer — no total order required.
/// Verification references:
/// - tests/unit/test_spsc_queue.cpp
/// - tests/benchmarks/bench_spsc_queue.cpp

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <new>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>

namespace options_ats_core::infra {

template <typename T, std::size_t Capacity>
    requires(std::has_single_bit(Capacity) && Capacity >= 2)
class SpscQueue {
public:
    // 64B matches the dominant x86 cache line size on colocated trading hosts.
    static constexpr std::size_t kCacheLineSize = 64;

    SpscQueue() noexcept = default;

    // Non-copyable, non-movable — shared across threads.
    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;

    /// Producer: attempt to enqueue an element. Returns false if full.
    [[nodiscard]] bool try_push(const T& item) noexcept(
        std::is_nothrow_copy_constructible_v<T>) {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t head = head_.load(std::memory_order_acquire);

        if (tail - head >= Capacity) {
            return false; // Full
        }

        buffer_[tail & kMask] = item;
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    /// Producer: attempt to enqueue via move.
    [[nodiscard]] bool try_push(T&& item) noexcept(
        std::is_nothrow_move_constructible_v<T>) {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t head = head_.load(std::memory_order_acquire);

        if (tail - head >= Capacity) {
            return false;
        }

        buffer_[tail & kMask] = std::move(item);
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    /// Producer: construct the element once at the slot to avoid extra temporaries.
    template <typename... Args>
    [[nodiscard]] bool try_emplace(Args&&... args) noexcept(
        std::is_nothrow_constructible_v<T, Args...> &&
        std::is_nothrow_move_assignable_v<T>) {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t head = head_.load(std::memory_order_acquire);

        if (tail - head >= Capacity) {
            return false;
        }

        buffer_[tail & kMask] = T(std::forward<Args>(args)...);
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    /// Consumer: attempt to dequeue an element.
    [[nodiscard]] std::optional<T> try_pop() noexcept(
        std::is_nothrow_move_constructible_v<T>) {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t tail = tail_.load(std::memory_order_acquire);

        if (head == tail) {
            return std::nullopt; // Empty
        }

        T item = std::move(buffer_[head & kMask]);
        head_.store(head + 1, std::memory_order_release);
        return item;
    }

    /// Consumer: attempt to dequeue into output parameter. Avoids optional overhead.
    [[nodiscard]] bool try_pop(T& out) noexcept(
        std::is_nothrow_move_assignable_v<T>) {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t tail = tail_.load(std::memory_order_acquire);

        if (head == tail) {
            return false;
        }

        out = std::move(buffer_[head & kMask]);
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        const std::size_t head = head_.load(std::memory_order_acquire);
        return tail - head;
    }

    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

    [[nodiscard]] static constexpr std::size_t capacity() noexcept {
        return Capacity;
    }

private:
    static constexpr std::size_t kMask = Capacity - 1;

    std::array<T, Capacity> buffer_{};

    // Pad to separate cache lines. Prevents false sharing between
    // producer (writes tail_) and consumer (writes head_).
    alignas(kCacheLineSize) std::atomic<std::size_t> head_{0};
    alignas(kCacheLineSize) std::atomic<std::size_t> tail_{0};
};

} // namespace options_ats_core::infra
