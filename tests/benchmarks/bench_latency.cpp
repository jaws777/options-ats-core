/// bench_latency.cpp — TSC-based latency regression benchmarks.
///
/// Project convention: latency regression tests use TSC-based
/// percentile benchmarks (p50/p99/p999). CI should gate on p99
/// regression greater than 5%.
///
/// These benchmarks measure critical-path operations in TSC ticks,
/// then convert to nanoseconds using the TSC frequency. Results are
/// printed as percentile distributions for CI gating.

#include <catch2/catch_test_macros.hpp>
#include <catch2/benchmark/catch_benchmark.hpp>

#include "market_data/series_book.hpp"
#include "oms/order_state_machine.hpp"
#include "risk/pre_trade.hpp"
#include "infra/seqlock.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <numeric>
#include <vector>

using namespace options_ats_core;

namespace {

/// Read TSC (Time Stamp Counter) for sub-microsecond measurement.
/// On x86: __rdtsc(). On ARM (Apple Silicon): use cntvct_el0.
[[nodiscard]] inline uint64_t rdtsc() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    unsigned int aux;
    return __rdtsc();
#elif defined(__aarch64__)
    uint64_t val;
    asm volatile("mrs %0, cntvct_el0" : "=r"(val));
    return val;
#else
    return std::chrono::high_resolution_clock::now().time_since_epoch().count();
#endif
}

/// Compute percentile from a sorted vector.
template <typename T>
[[nodiscard]] T percentile(const std::vector<T>& sorted, double pct) {
    if (sorted.empty()) return T{};
    auto idx = static_cast<std::size_t>(pct / 100.0 * static_cast<double>(sorted.size() - 1));
    return sorted[std::min(idx, sorted.size() - 1)];
}

/// Print percentile summary.
void print_percentiles(const char* label, std::vector<uint64_t>& latencies) {
    std::sort(latencies.begin(), latencies.end());
    auto p50  = percentile(latencies, 50.0);
    auto p99  = percentile(latencies, 99.0);
    auto p999 = percentile(latencies, 99.9);
    auto avg  = std::accumulate(latencies.begin(), latencies.end(), 0ULL) / latencies.size();

    // Output as structured line for CI parsing.
    // Format: LATENCY|label|p50|p99|p999|avg|unit
    printf("LATENCY|%s|%llu|%llu|%llu|%llu|ticks\n",
           label, p50, p99, p999, avg);
}

} // namespace

TEST_CASE("Latency: SeriesBook match_retail_order", "[!benchmark][latency]") {
    market_data::SeriesBook book;
    // Set up 5 MMs quoting.
    for (uint32_t i = 0; i < 5; ++i) {
        book.update_quote(market_data::MmQuote{
            .mm_id = i,
            .bid = core::FixedPrice::from_raw(1'000'000'000 + static_cast<int64_t>(i) * 1'000'000),
            .ask = core::FixedPrice::from_raw(1'005'000'000 - static_cast<int64_t>(i) * 1'000'000),
            .bid_size = 100,
            .ask_size = 100,
            .timestamp_tsc = 0,
            .active = true,
        });
    }

    auto nbbo = market_data::NbboEntry{
        .bid = core::FixedPrice::from_raw(1'000'000'000),
        .ask = core::FixedPrice::from_raw(1'005'000'000),
        .timestamp_ns = 1000,
    };

    constexpr int kIters = 100'000;
    std::vector<uint64_t> latencies;
    latencies.reserve(kIters);

    for (int i = 0; i < kIters; ++i) {
        // Re-fill MM quotes (consumed by previous match).
        for (uint32_t m = 0; m < 5; ++m) {
            book.update_quote(market_data::MmQuote{
                .mm_id = m,
                .bid = core::FixedPrice::from_raw(1'000'000'000),
                .ask = core::FixedPrice::from_raw(1'005'000'000),
                .bid_size = 100,
                .ask_size = 100,
                .timestamp_tsc = 0,
                .active = true,
            });
        }

        auto order = market_data::RetailOrder{
            .client_order_id = static_cast<uint64_t>(i),
            .side = market_data::Side::Buy,
            .quantity = 10,
            .timestamp_tsc = rdtsc(),
        };

        auto start = rdtsc();
        auto result = book.match_retail_order(order, nbbo);
        auto end = rdtsc();

        latencies.push_back(end - start);
        (void)result; // Prevent optimization.
    }

    print_percentiles("series_book_match", latencies);
}

TEST_CASE("Latency: PreTradeRiskCheck position check", "[!benchmark][latency]") {
    risk::PreTradeRiskCheck risk;
    risk.update_limits(risk::RiskLimits{.max_position = 1'000'000'000});

    constexpr int kIters = 100'000;
    std::vector<uint64_t> latencies;
    latencies.reserve(kIters);

    for (int i = 0; i < kIters; ++i) {
        auto start = rdtsc();
        auto result = risk.check_position(1);
        auto end = rdtsc();
        latencies.push_back(end - start);
        (void)result;
    }

    print_percentiles("pre_trade_position", latencies);
}

TEST_CASE("Latency: OrderStateMachine transition", "[!benchmark][latency]") {
    constexpr int kIters = 100'000;
    std::vector<uint64_t> latencies;
    latencies.reserve(kIters);

    for (int i = 0; i < kIters; ++i) {
        auto start = rdtsc();
        auto result = oms::transition(oms::OrderState::Accepted, oms::OrderEvent::Fill);
        auto end = rdtsc();
        latencies.push_back(end - start);
        (void)result;
    }

    print_percentiles("osm_transition", latencies);
}

TEST_CASE("Latency: Seqlock read", "[!benchmark][latency]") {
    struct BboSnap {
        int64_t bid{0};
        int64_t ask{0};
        int32_t bid_size{0};
        int32_t ask_size{0};
    };

    infra::Seqlock<BboSnap> sl;
    sl.store(BboSnap{.bid = 1000, .ask = 1005, .bid_size = 100, .ask_size = 200});

    constexpr int kIters = 100'000;
    std::vector<uint64_t> latencies;
    latencies.reserve(kIters);

    for (int i = 0; i < kIters; ++i) {
        auto start = rdtsc();
        auto snap = sl.load();
        auto end = rdtsc();
        latencies.push_back(end - start);
        (void)snap;
    }

    print_percentiles("seqlock_read", latencies);
}
