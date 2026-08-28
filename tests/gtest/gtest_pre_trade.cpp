#include <gtest/gtest.h>
#include "risk/pre_trade.hpp"

#include <atomic>
#include <thread>
#include <vector>

using namespace options_ats_core::risk;

TEST(PreTradeTest, PositionWithinLimit) {
    PreTradeRiskCheck risk;
    risk.update_limits(RiskLimits{.max_position = 1000});
    auto r = risk.check_position(500);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 500);
}

TEST(PreTradeTest, PositionOverLimitRejected) {
    PreTradeRiskCheck risk;
    risk.update_limits(RiskLimits{.max_position = 100});
    ASSERT_TRUE(risk.check_position(80).has_value());
    auto r = risk.check_position(30);
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), RiskReject::PositionLimitBreached);
    EXPECT_EQ(risk.net_position(), 80); // Unchanged.
}

TEST(PreTradeTest, OrderRateLimit) {
    PreTradeRiskCheck risk;
    risk.update_limits(RiskLimits{.max_orders_per_second = 3});
    EXPECT_TRUE(risk.check_order_rate().has_value());
    EXPECT_TRUE(risk.check_order_rate().has_value());
    EXPECT_TRUE(risk.check_order_rate().has_value());
    EXPECT_FALSE(risk.check_order_rate().has_value());

    risk.reset_order_count();
    EXPECT_TRUE(risk.check_order_rate().has_value());
}

TEST(PreTradeTest, HotReloadLimits) {
    PreTradeRiskCheck risk;
    risk.update_limits(RiskLimits{.max_position = 100});
    ASSERT_TRUE(risk.check_position(80).has_value());

    risk.update_limits(RiskLimits{.max_position = 200});
    auto r = risk.check_position(80); // 160 < 200, now OK.
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 160);
}

TEST(PreTradeTest, ConcurrentPositionUpdates) {
    PreTradeRiskCheck risk;
    risk.update_limits(RiskLimits{.max_position = 1'000'000});

    constexpr int kThreads = 4;
    constexpr int kOps = 10'000;

    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < kOps; ++i) {
                auto result = risk.check_position(1);
                if (!result.has_value()) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& t : threads) t.join();
    EXPECT_EQ(failures.load(std::memory_order_relaxed), 0);
    EXPECT_EQ(risk.net_position(), kThreads * kOps);
}
