#pragma once
/// lmax_disruptor.hpp — Minimal LMAX Disruptor-style SPSC ring.
///
/// Scope:
/// - Single producer, single consumer.
/// - Preallocated ring buffer addressed by monotonically increasing sequence.
/// - Producer claims a slot, writes in place, then publishes.
/// - Consumer reads the published slot and advances its sequence.
///
/// This is intentionally narrower than the full LMAX Disruptor:
/// no multicast consumers, no dependency graph, no wait strategies.

#include "core/expected.hpp"

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace options_ats_core::infra {

enum class DisruptorError : uint8_t {
    RingFull,
    RingEmpty,
};

template <typename T, std::size_t Capacity>
    requires(std::has_single_bit(Capacity) && Capacity >= 2 &&
             std::is_default_constructible_v<T>)
class LmaxDisruptor {
public:
    static constexpr std::size_t kCacheLineSize = 64;
    static constexpr std::size_t kMask = Capacity - 1;

    struct ProducerClaim {
        uint64_t sequence;
        T* event;
    };

    struct ConsumerRecord {
        uint64_t sequence;
        const T* event;
    };

    LmaxDisruptor() noexcept = default;

    LmaxDisruptor(const LmaxDisruptor&) = delete;
    LmaxDisruptor& operator=(const LmaxDisruptor&) = delete;

    [[nodiscard]] core::expected<ProducerClaim, DisruptorError>
    try_claim() noexcept {
        const uint64_t next_sequence =
            next_producer_sequence_.load(std::memory_order_relaxed);
        const uint64_t consumer_sequence =
            consumer_sequence_.load(std::memory_order_acquire);

        if (next_sequence - consumer_sequence >= Capacity) {
            return core::unexpected(DisruptorError::RingFull);
        }

        return ProducerClaim{
            .sequence = next_sequence,
            .event = &buffer_[static_cast<std::size_t>(next_sequence) & kMask],
        };
    }

    void publish(uint64_t sequence) noexcept {
        next_producer_sequence_.store(sequence + 1, std::memory_order_relaxed);
        published_sequence_.store(sequence, std::memory_order_release);
    }

    [[nodiscard]] core::expected<ConsumerRecord, DisruptorError>
    try_consume() noexcept {
        const uint64_t next_sequence =
            next_consumer_sequence_.load(std::memory_order_relaxed);
        const uint64_t published_sequence =
            published_sequence_.load(std::memory_order_acquire);

        if (next_sequence > published_sequence) {
            return core::unexpected(DisruptorError::RingEmpty);
        }

        return ConsumerRecord{
            .sequence = next_sequence,
            .event = &buffer_[static_cast<std::size_t>(next_sequence) & kMask],
        };
    }

    void release(uint64_t sequence) noexcept {
        next_consumer_sequence_.store(sequence + 1, std::memory_order_relaxed);
        consumer_sequence_.store(sequence + 1, std::memory_order_release);
    }

    [[nodiscard]] uint64_t published_sequence() const noexcept {
        return published_sequence_.load(std::memory_order_acquire);
    }

    [[nodiscard]] uint64_t consumer_sequence() const noexcept {
        return consumer_sequence_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::size_t size() const noexcept {
        const uint64_t producer =
            next_producer_sequence_.load(std::memory_order_acquire);
        const uint64_t consumer =
            consumer_sequence_.load(std::memory_order_acquire);
        return static_cast<std::size_t>(producer - consumer);
    }

    [[nodiscard]] static constexpr std::size_t capacity() noexcept {
        return Capacity;
    }

private:
    std::array<T, Capacity> buffer_{};

    alignas(kCacheLineSize) std::atomic<uint64_t> next_producer_sequence_{0};
    alignas(kCacheLineSize) std::atomic<uint64_t> published_sequence_{
        static_cast<uint64_t>(-1)};
    alignas(kCacheLineSize) std::atomic<uint64_t> next_consumer_sequence_{0};
    alignas(kCacheLineSize) std::atomic<uint64_t> consumer_sequence_{0};
};

} // namespace options_ats_core::infra
