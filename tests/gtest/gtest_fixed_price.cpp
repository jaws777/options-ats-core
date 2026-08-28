#include <gtest/gtest.h>
#include "core/fixed_price.hpp"

using namespace options_ats_core::core;

TEST(FixedPriceTest, ConstructionAndRawRoundTrip) {
    constexpr auto p = FixedPrice::from_raw(150'000'000);
    EXPECT_EQ(p.to_raw(), 150'000'000);

    constexpr auto zero = FixedPrice{};
    EXPECT_EQ(zero.to_raw(), 0);
    EXPECT_TRUE(zero.is_zero());
}

TEST(FixedPriceTest, FromDollarsCents) {
    constexpr auto p = FixedPrice::from_dollars_cents(1, 50);
    EXPECT_EQ(p.to_raw(), 150'000'000);

    constexpr auto p2 = FixedPrice::from_dollars_cents(100, 1);
    EXPECT_EQ(p2.to_raw(), 10'001'000'000);
}

TEST(FixedPriceTest, ConstevalFromDouble) {
    constexpr auto p = FixedPrice::from_double(3.14);
    EXPECT_EQ(p.to_raw(), 314'000'000);
}

TEST(FixedPriceTest, Arithmetic) {
    constexpr auto a = FixedPrice::from_double(10.50);
    constexpr auto b = FixedPrice::from_double(3.25);
    EXPECT_EQ((a + b).to_raw(), FixedPrice::from_double(13.75).to_raw());
    EXPECT_EQ((a - b).to_raw(), FixedPrice::from_double(7.25).to_raw());
    EXPECT_EQ((a * 100).to_raw(), FixedPrice::from_double(1050.0).to_raw());
}

TEST(FixedPriceTest, ThreeWayComparison) {
    constexpr auto low  = FixedPrice::from_double(1.00);
    constexpr auto high = FixedPrice::from_double(2.00);
    EXPECT_LT(low, high);
    EXPECT_GT(high, low);
    EXPECT_EQ(low, FixedPrice::from_double(1.00));
}

TEST(FixedPriceTest, ToCharsFormatting) {
    auto check = [](FixedPrice p, std::string_view expected) {
        auto buf = p.to_chars();
        EXPECT_EQ(std::string_view(buf.data()), expected);
    };
    check(FixedPrice::from_double(1.50),   "1.50");
    check(FixedPrice::from_double(100.01), "100.01");
    check(FixedPrice::from_raw(0),         "0.00");
}

TEST(FixedPriceTest, TickConstants) {
    constexpr auto penny  = FixedPrice::penny_tick();
    constexpr auto nickel = FixedPrice::nickel_tick();
    EXPECT_EQ(penny.to_raw(), 1'000'000);
    EXPECT_EQ(nickel.to_raw(), 5'000'000);
    EXPECT_EQ(penny * 5, nickel);
}

TEST(FixedPriceTest, CheckedArithmeticOverflow) {
    constexpr auto max_price = FixedPrice::from_raw(std::numeric_limits<int64_t>::max());
    auto result = max_price.try_add(FixedPrice::penny_tick());
    EXPECT_FALSE(result.has_value());
}
