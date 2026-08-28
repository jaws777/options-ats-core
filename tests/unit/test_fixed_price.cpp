#include <catch2/catch_test_macros.hpp>
#include "core/fixed_price.hpp"

#include <limits>

using namespace options_ats_core::core;

TEST_CASE("FixedPrice: construction and raw round-trip", "[fixed_price]") {
    constexpr auto p = FixedPrice::from_raw(150'000'000); // $1.50
    STATIC_REQUIRE(p.to_raw() == 150'000'000);

    constexpr auto zero = FixedPrice{};
    STATIC_REQUIRE(zero.to_raw() == 0);
    STATIC_REQUIRE(zero.is_zero());
}

TEST_CASE("FixedPrice: from_dollars_cents", "[fixed_price]") {
    constexpr auto p = FixedPrice::from_dollars_cents(1, 50); // $1.50
    STATIC_REQUIRE(p.to_raw() == 150'000'000);

    constexpr auto p2 = FixedPrice::from_dollars_cents(100, 1); // $100.01
    STATIC_REQUIRE(p2.to_raw() == 10'001'000'000);
}

TEST_CASE("FixedPrice: consteval from_double", "[fixed_price]") {
    constexpr auto p = FixedPrice::from_double(3.14);
    STATIC_REQUIRE(p.to_raw() == 314'000'000);

    constexpr auto neg = FixedPrice::from_double(-0.50);
    STATIC_REQUIRE(neg.to_raw() == -50'000'000);
}

TEST_CASE("FixedPrice: arithmetic", "[fixed_price]") {
    constexpr auto a = FixedPrice::from_double(10.50);
    constexpr auto b = FixedPrice::from_double(3.25);

    STATIC_REQUIRE((a + b).to_raw() == FixedPrice::from_double(13.75).to_raw());
    STATIC_REQUIRE((a - b).to_raw() == FixedPrice::from_double(7.25).to_raw());
    STATIC_REQUIRE((-a).to_raw() == FixedPrice::from_double(-10.50).to_raw());

    // Price * quantity = notional
    constexpr auto notional = a * 100;
    STATIC_REQUIRE(notional.to_raw() == FixedPrice::from_double(1050.0).to_raw());
}

TEST_CASE("FixedPrice: comparison via spaceship operator", "[fixed_price]") {
    constexpr auto low  = FixedPrice::from_double(1.00);
    constexpr auto mid  = FixedPrice::from_double(1.50);
    constexpr auto high = FixedPrice::from_double(2.00);
    constexpr auto mid2 = FixedPrice::from_double(1.50);

    STATIC_REQUIRE(low < mid);
    STATIC_REQUIRE(mid < high);
    STATIC_REQUIRE(mid == mid2);
    STATIC_REQUIRE(high > low);
    STATIC_REQUIRE(low != high);
    STATIC_REQUIRE(low <= mid);
    STATIC_REQUIRE(mid >= mid2);
}

TEST_CASE("FixedPrice: to_chars formatting", "[fixed_price]") {
    auto check = [](FixedPrice p, std::string_view expected) {
        auto buf = p.to_chars();
        REQUIRE(std::string_view(buf.data()) == expected);
    };

    check(FixedPrice::from_double(1.50),   "1.50");
    check(FixedPrice::from_double(100.01), "100.01");
    check(FixedPrice::from_double(0.99),   "0.99");
    check(FixedPrice::from_double(-3.14),  "-3.14");
    check(FixedPrice::from_raw(0),         "0.00");
    check(FixedPrice::from_double(1234.56789), "1234.56789");
}

TEST_CASE("FixedPrice: tick constants", "[fixed_price]") {
    constexpr auto penny  = FixedPrice::penny_tick();
    constexpr auto nickel = FixedPrice::nickel_tick();

    STATIC_REQUIRE(penny.to_raw()  == 1'000'000);   // $0.01 * 10^8
    STATIC_REQUIRE(nickel.to_raw() == 5'000'000);    // $0.05 * 10^8

    // 5 pennies = 1 nickel
    constexpr auto five_pennies = penny * 5;
    STATIC_REQUIRE(five_pennies == nickel);
}

TEST_CASE("FixedPrice: += and -=", "[fixed_price]") {
    auto p = FixedPrice::from_double(10.00);
    p += FixedPrice::penny_tick();
    REQUIRE(p == FixedPrice::from_double(10.01));

    p -= FixedPrice::from_double(0.02);
    REQUIRE(p == FixedPrice::from_double(9.99));
}

TEST_CASE("FixedPrice: checked construction rejects invalid cents", "[fixed_price]") {
    auto p = FixedPrice::try_from_dollars_cents(10, 100);
    REQUIRE_FALSE(p.has_value());
    REQUIRE(p.error() == FixedPriceError::InvalidCents);
}

TEST_CASE("FixedPrice: checked arithmetic reports overflow", "[fixed_price]") {
    constexpr auto max_price = FixedPrice::from_raw(std::numeric_limits<int64_t>::max());
    const auto add_result = max_price.try_add(FixedPrice::penny_tick());
    REQUIRE_FALSE(add_result.has_value());
    REQUIRE(add_result.error() == FixedPriceError::NumericOverflow);

    constexpr auto min_price = FixedPrice::from_raw(std::numeric_limits<int64_t>::min());
    const auto negate_result = min_price.try_negate();
    REQUIRE_FALSE(negate_result.has_value());
    REQUIRE(negate_result.error() == FixedPriceError::NumericOverflow);
}
