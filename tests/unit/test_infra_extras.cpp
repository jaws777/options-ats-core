#include <catch2/catch_test_macros.hpp>
#include "infra/seqlock.hpp"
#include "infra/wal.hpp"
#include "infra/rate_limiter.hpp"

#include <thread>

using namespace options_ats_core::infra;

// --- Seqlock Tests ---

struct BboSnapshot {
    int64_t bid_raw{0};
    int64_t ask_raw{0};
    int32_t bid_size{0};
    int32_t ask_size{0};
};

TEST_CASE("Seqlock: basic store and load", "[seqlock]") {
    Seqlock<BboSnapshot> sl;

    BboSnapshot snap{.bid_raw = 1000, .ask_raw = 1005, .bid_size = 100, .ask_size = 200};
    sl.store(snap);

    auto loaded = sl.load();
    REQUIRE(loaded.bid_raw == 1000);
    REQUIRE(loaded.ask_raw == 1005);
    REQUIRE(loaded.bid_size == 100);
    REQUIRE(loaded.ask_size == 200);
}

TEST_CASE("Seqlock: sequence increments by 2 per write", "[seqlock]") {
    Seqlock<BboSnapshot> sl;
    REQUIRE(sl.sequence() == 0);

    sl.store(BboSnapshot{});
    REQUIRE(sl.sequence() == 2);

    sl.store(BboSnapshot{});
    REQUIRE(sl.sequence() == 4);
}

TEST_CASE("Seqlock: load_with_seq returns sequence", "[seqlock]") {
    Seqlock<BboSnapshot> sl;
    sl.store(BboSnapshot{.bid_raw = 42});

    auto [snap, seq] = sl.load_with_seq();
    REQUIRE(snap.bid_raw == 42);
    REQUIRE(seq == 2);
}

TEST_CASE("Seqlock: concurrent writer and reader", "[seqlock]") {
    Seqlock<BboSnapshot> sl;
    constexpr int kWrites = 100'000;
    std::atomic<bool> done{false};

    std::thread writer([&] {
        for (int i = 1; i <= kWrites; ++i) {
            sl.store(BboSnapshot{
                .bid_raw = i, .ask_raw = i + 5,
                .bid_size = i, .ask_size = i});
        }
        done.store(true, std::memory_order_release);
    });

    // Reader verifies consistency: bid and ask from same write.
    int reads = 0;
    int consistent_reads = 0;
    while (!done.load(std::memory_order_acquire) || consistent_reads < 100) {
        auto snap = sl.load();
        // Skip initial zero state (no write yet).
        if (snap.bid_raw == 0) { ++reads; if (reads > 10'000'000) break; continue; }
        // If we got a consistent read, ask = bid + 5.
        REQUIRE(snap.ask_raw == snap.bid_raw + 5);
        REQUIRE(snap.ask_size == snap.bid_size);
        ++consistent_reads;
        ++reads;
        if (reads > 10'000'000) break; // Safety.
    }

    writer.join();
    REQUIRE(reads > 0);
}

// --- WAL Tests ---

TEST_CASE("Wal: append and read", "[wal]") {
    Wal wal;
    auto seq = wal.append(EventType::NewOrder, 1000, 42, 100, 200);
    REQUIRE(seq.has_value());
    REQUIRE(*seq == 0);

    auto entry = wal.read(0);
    REQUIRE(entry != nullptr);
    REQUIRE(entry->event_type == EventType::NewOrder);
    REQUIRE(entry->timestamp_ns == 1000);
    REQUIRE(entry->entity_id == 42);
    REQUIRE(entry->payload_a == 100);
}

TEST_CASE("Wal: monotonic sequence", "[wal]") {
    Wal wal;
    for (int i = 0; i < 100; ++i) {
        auto seq = wal.append(EventType::Fill, static_cast<uint64_t>(i * 1000),
                              static_cast<uint64_t>(i));
        REQUIRE(seq.has_value());
        REQUIRE(*seq == static_cast<uint64_t>(i));
    }
    REQUIRE(wal.size() == 100);
    REQUIRE(wal.next_sequence() == 100);
}

TEST_CASE("Wal: bounded capacity rejects append before allocation growth", "[wal]") {
    Wal wal(2);
    REQUIRE(wal.append(EventType::NewOrder, 0, 1).has_value());
    REQUIRE(wal.append(EventType::Fill, 0, 1).has_value());

    auto overflow = wal.append(EventType::QuoteUpdate, 0, 1);
    REQUIRE_FALSE(overflow.has_value());
    REQUIRE(overflow.error() == WalError::BufferFull);
    REQUIRE(wal.size() == 2);
    REQUIRE(wal.next_sequence() == 2);
}

TEST_CASE("Wal: read_range", "[wal]") {
    Wal wal;
    for (int i = 0; i < 10; ++i) {
        auto seq = wal.append(EventType::QuoteUpdate, 0, static_cast<uint64_t>(i));
        REQUIRE(seq.has_value());
    }

    auto range = wal.read_range(3, 4); // Entries 3,4,5,6.
    REQUIRE(range.size() == 4);
    REQUIRE(range[0].entity_id == 3);
    REQUIRE(range[3].entity_id == 6);
}

TEST_CASE("Wal: verify_parity succeeds for identical WALs", "[wal]") {
    Wal wal1, wal2;
    for (int i = 0; i < 50; ++i) {
        auto seq1 = wal1.append(EventType::Fill, 1000, static_cast<uint64_t>(i), i * 10);
        auto seq2 = wal2.append(EventType::Fill, 1000, static_cast<uint64_t>(i), i * 10);
        REQUIRE(seq1.has_value());
        REQUIRE(seq2.has_value());
    }

    auto result = wal1.verify_parity(wal2);
    REQUIRE(result.has_value());
    REQUIRE(*result == true);
}

TEST_CASE("Wal: verify_parity detects divergence", "[wal]") {
    Wal wal1, wal2;
    for (int i = 0; i < 10; ++i) {
        auto seq1 = wal1.append(EventType::Fill, 1000, static_cast<uint64_t>(i), i * 10);
        int64_t payload = (i == 5) ? 999 : i * 10; // Diverge at seq 5.
        auto seq2 = wal2.append(EventType::Fill, 1000, static_cast<uint64_t>(i), payload);
        REQUIRE(seq1.has_value());
        REQUIRE(seq2.has_value());
    }

    auto result = wal1.verify_parity(wal2);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == 5); // Divergence at sequence 5.
}

// --- Rate Limiter Tests ---

TEST_CASE("RateLimiter: allows within limit", "[rate]") {
    RateLimiter limiter(5);
    for (int i = 0; i < 5; ++i) {
        REQUIRE(limiter.try_consume());
    }
    REQUIRE(limiter.remaining() == 0);
}

TEST_CASE("RateLimiter: rejects over limit", "[rate]") {
    RateLimiter limiter(3);
    REQUIRE(limiter.try_consume());
    REQUIRE(limiter.try_consume());
    REQUIRE(limiter.try_consume());
    REQUIRE_FALSE(limiter.try_consume()); // 4th rejected.
}

TEST_CASE("RateLimiter: refill restores tokens", "[rate]") {
    RateLimiter limiter(3);
    REQUIRE(limiter.try_consume());
    REQUIRE(limiter.try_consume());
    REQUIRE(limiter.try_consume());
    REQUIRE_FALSE(limiter.try_consume());

    limiter.refill();
    REQUIRE(limiter.remaining() == 3);
    REQUIRE(limiter.try_consume());
}

TEST_CASE("RateLimiter: bulk consume", "[rate]") {
    RateLimiter limiter(10);
    REQUIRE(limiter.try_consume(7));
    REQUIRE(limiter.remaining() == 3);
    REQUIRE_FALSE(limiter.try_consume(5)); // Only 3 left.
    REQUIRE(limiter.try_consume(3));       // Exactly 3.
    REQUIRE(limiter.remaining() == 0);
}

TEST_CASE("RateLimiter: hot-reload rate", "[rate]") {
    RateLimiter limiter(5);
    limiter.set_rate(10);
    limiter.refill();
    REQUIRE(limiter.remaining() == 10);
    REQUIRE(limiter.rate() == 10);
}

TEST_CASE("RateLimiter: concurrent consume is safe", "[rate]") {
    RateLimiter limiter(100'000);
    constexpr int kThreads = 4;
    constexpr int kOpsPerThread = 10'000;

    std::atomic<int> total_consumed{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&] {
            int local = 0;
            for (int i = 0; i < kOpsPerThread; ++i) {
                if (limiter.try_consume()) ++local;
            }
            total_consumed.fetch_add(local);
        });
    }
    for (auto& t : threads) t.join();

    // All consumes accounted for — no double-consume.
    REQUIRE(total_consumed.load() + static_cast<int>(limiter.remaining()) == 100'000);
}
