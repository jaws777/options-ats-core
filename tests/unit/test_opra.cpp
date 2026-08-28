#include <catch2/catch_test_macros.hpp>
#include "market_data/opra.hpp"

using namespace options_ats_core::market_data;

TEST_CASE("OPRA: parses canonical OCC code", "[opra]") {
    const auto code = parse_opra_code("AAPL  240621C00195000");
    REQUIRE(code.has_value());
    REQUIRE(code->root_view() == "AAPL");
    REQUIRE(code->expiry_year == 24);
    REQUIRE(code->expiry_month == 6);
    REQUIRE(code->expiry_day == 21);
    REQUIRE(code->right == OptionRight::Call);
    REQUIRE(code->strike == options_ats_core::core::FixedPrice::from_dollars_cents(195, 0));
}

TEST_CASE("OPRA: rejects malformed code length", "[opra]") {
    const auto code = parse_opra_code("AAPL240621C00195000");
    REQUIRE_FALSE(code.has_value());
    REQUIRE(code.error() == OpraParseError::InvalidLength);
}

TEST_CASE("OPRA: parses exchange quote line", "[opra]") {
    const auto quote = parse_exchange_quote_line(
        "AAPL  240621C00195000,1718386800123456789,2.35,2.40,125,110,CBOE");

    REQUIRE(quote.has_value());
    REQUIRE(quote->symbol.root_view() == "AAPL");
    REQUIRE(quote->bid == options_ats_core::core::FixedPrice::from_dollars_cents(2, 35));
    REQUIRE(quote->ask == options_ats_core::core::FixedPrice::from_dollars_cents(2, 40));
    REQUIRE(quote->bid_qty == 125);
    REQUIRE(quote->ask_qty == 110);
    REQUIRE(quote->exchange == "CBOE");
}

TEST_CASE("OPRA: rejects crossed quote", "[opra]") {
    const auto quote = parse_exchange_quote_line(
        "AAPL  240621C00195000,1718386800123456789,2.45,2.40,125,110,CBOE");
    REQUIRE_FALSE(quote.has_value());
    REQUIRE(quote.error() == OpraParseError::InvalidPrice);
}
