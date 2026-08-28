#include <catch2/catch_test_macros.hpp>
#include "risk/pre_trade.hpp"

#include <atomic>
#include <thread>
#include <vector>

using namespace options_ats_core::risk;

TEST_CASE("PreTrade: position check passes within limit", "[risk]") {
    PreTradeRiskCheck risk;
    risk.update_limits(RiskLimits{.max_position = 1000});

    auto result = risk.check_position(500);
    REQUIRE(result.has_value());
    REQUIRE(*result == 500);
    REQUIRE(risk.net_position() == 500);
}

TEST_CASE("PreTrade: position check rejects over limit", "[risk]") {
    PreTradeRiskCheck risk;
    risk.update_limits(RiskLimits{.max_position = 100});

    auto r1 = risk.check_position(80);
    REQUIRE(r1.has_value());

    auto r2 = risk.check_position(30); // 80 + 30 = 110 > 100.
    REQUIRE_FALSE(r2.has_value());
    REQUIRE(r2.error() == RiskReject::PositionLimitBreached);
    REQUIRE(risk.net_position() == 80); // Unchanged.
}

TEST_CASE("PreTrade: negative position (short) within limit", "[risk]") {
    PreTradeRiskCheck risk;
    risk.update_limits(RiskLimits{.max_position = 1000});

    auto result = risk.check_position(-500);
    REQUIRE(result.has_value());
    REQUIRE(*result == -500);
}

TEST_CASE("PreTrade: short position rejected at limit", "[risk]") {
    PreTradeRiskCheck risk;
    risk.update_limits(RiskLimits{.max_position = 100});

    auto r1 = risk.check_position(-80);
    REQUIRE(r1.has_value());

    auto r2 = risk.check_position(-30); // -110 < -100.
    REQUIRE_FALSE(r2.has_value());
}

TEST_CASE("PreTrade: order rate limiting", "[risk]") {
    PreTradeRiskCheck risk;
    risk.update_limits(RiskLimits{.max_orders_per_second = 3});

    REQUIRE(risk.check_order_rate().has_value());
    REQUIRE(risk.check_order_rate().has_value());
    REQUIRE(risk.check_order_rate().has_value());
    REQUIRE_FALSE(risk.check_order_rate().has_value()); // 4th rejected.

    risk.reset_order_count();
    REQUIRE(risk.check_order_rate().has_value()); // Reset — allowed again.
}

TEST_CASE("PreTrade: hot-reload limits", "[risk]") {
    PreTradeRiskCheck risk;
    risk.update_limits(RiskLimits{.max_position = 100});

    auto r1 = risk.check_position(80);
    REQUIRE(r1.has_value());

    // Reload with higher limit.
    risk.update_limits(RiskLimits{.max_position = 200});

    auto r2 = risk.check_position(80); // 160 < 200, now OK.
    REQUIRE(r2.has_value());
    REQUIRE(*r2 == 160);
}

TEST_CASE("PreTrade: concurrent position updates are safe", "[risk]") {
    PreTradeRiskCheck risk;
    risk.update_limits(RiskLimits{.max_position = 1'000'000});

    constexpr int kThreads = 4;
    constexpr int kOpsPerThread = 10'000;

    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < kOpsPerThread; ++i) {
                auto result = risk.check_position(1);
                if (!result.has_value()) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& t : threads) t.join();

    // All updates applied — no lost updates.
    REQUIRE(failures.load(std::memory_order_relaxed) == 0);
    REQUIRE(risk.net_position() == kThreads * kOpsPerThread);
}
