#include <catch2/catch_test_macros.hpp>
#include "oms/fix_parser.hpp"

using namespace options_ats_core::oms;
using namespace options_ats_core::core;

namespace {
// SOH = 0x01, standard FIX delimiter.
constexpr char SOH = '\x01';

std::string make_fix_nos(std::string_view cl_ord_id = "ORD001",
                         std::string_view side = "1",
                         std::string_view qty = "100",
                         std::string_view price = "10.05",
                         std::string_view symbol = "AAPL") {
    std::string msg;
    msg += "35=D"; msg += SOH;
    msg += "11="; msg += cl_ord_id; msg += SOH;
    msg += "54="; msg += side; msg += SOH;
    msg += "38="; msg += qty; msg += SOH;
    msg += "44="; msg += price; msg += SOH;
    msg += "55="; msg += symbol; msg += SOH;
    msg += "59=3"; msg += SOH; // IOC
    return msg;
}
} // namespace

TEST_CASE("FIX: parse valid NewOrderSingle", "[fix]") {
    auto msg = make_fix_nos();
    auto result = parse_fix_new_order(msg);
    REQUIRE(result.has_value());

    auto& order = *result;
    REQUIRE(order.msg_type == FixMsgType::NewOrderSingle);
    REQUIRE(order.cl_ord_id == "ORD001");
    REQUIRE(order.side == FixSide::Buy);
    REQUIRE(order.quantity == 100);
    REQUIRE(order.symbol == "AAPL");
    REQUIRE(order.tif == TimeInForce::IOC);

    // Price: $10.05
    REQUIRE(order.price.to_raw() == 1'005'000'000);
}

TEST_CASE("FIX: parse sell side", "[fix]") {
    auto msg = make_fix_nos("ORD002", "2");
    auto result = parse_fix_new_order(msg);
    REQUIRE(result.has_value());
    REQUIRE(result->side == FixSide::Sell);
}

TEST_CASE("FIX: missing MsgType rejected", "[fix]") {
    std::string msg;
    msg += "11=ORD001"; msg += SOH;
    msg += "54=1"; msg += SOH;

    auto result = parse_fix_new_order(msg);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == FixParseError::MissingTag);
}

TEST_CASE("FIX: invalid side rejected", "[fix]") {
    auto msg = make_fix_nos("ORD001", "9"); // Invalid side.
    auto result = parse_fix_new_order(msg);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == FixParseError::InvalidSide);
}

TEST_CASE("FIX: zero quantity rejected", "[fix]") {
    auto msg = make_fix_nos("ORD001", "1", "0");
    auto result = parse_fix_new_order(msg);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == FixParseError::InvalidQuantity);
}

TEST_CASE("FIX: negative quantity rejected", "[fix]") {
    auto msg = make_fix_nos("ORD001", "1", "-50");
    auto result = parse_fix_new_order(msg);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == FixParseError::InvalidQuantity);
}

TEST_CASE("FIX: price with many decimals", "[fix]") {
    auto msg = make_fix_nos("ORD001", "1", "100", "123.456789");
    auto result = parse_fix_new_order(msg);
    REQUIRE(result.has_value());
    // 123.456789 * 10^8 = 12345678900
    REQUIRE(result->price.to_raw() == 12'345'678'900);
}

TEST_CASE("FIX: price with excess precision rejected", "[fix]") {
    auto msg = make_fix_nos("ORD001", "1", "100", "123.456789123");
    auto result = parse_fix_new_order(msg);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == FixParseError::InvalidPrice);
}

TEST_CASE("FIX: non-positive price rejected", "[fix]") {
    auto zero = parse_fix_new_order(make_fix_nos("ORD001", "1", "100", "0"));
    REQUIRE_FALSE(zero.has_value());
    REQUIRE(zero.error() == FixParseError::InvalidPrice);

    auto negative = parse_fix_new_order(make_fix_nos("ORD001", "1", "100", "-1.25"));
    REQUIRE_FALSE(negative.has_value());
    REQUIRE(negative.error() == FixParseError::InvalidPrice);
}

TEST_CASE("FIX: overflowing price rejected", "[fix]") {
    auto msg = make_fix_nos("ORD001", "1", "100", "9223372036854775807");
    auto result = parse_fix_new_order(msg);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == FixParseError::InvalidPrice);
}

TEST_CASE("FIX: integer price (no decimal)", "[fix]") {
    auto msg = make_fix_nos("ORD001", "1", "100", "50");
    auto result = parse_fix_new_order(msg);
    REQUIRE(result.has_value());
    REQUIRE(result->price.to_raw() == 50 * FixedPrice::kScale);
}

TEST_CASE("FIX: cancel request", "[fix]") {
    std::string msg;
    msg += "35=F"; msg += SOH;
    msg += "11=ORD001"; msg += SOH;
    msg += "54=1"; msg += SOH;
    msg += "38=100"; msg += SOH;
    msg += "55=AAPL"; msg += SOH;

    auto result = parse_fix_new_order(msg);
    REQUIRE(result.has_value());
    REQUIRE(result->msg_type == FixMsgType::OrderCancelRequest);
}

TEST_CASE("FIX: unknown message type rejected", "[fix]") {
    std::string msg;
    msg += "35=Z"; msg += SOH;
    msg += "11=ORD001"; msg += SOH;
    msg += "54=1"; msg += SOH;
    msg += "38=100"; msg += SOH;

    auto result = parse_fix_new_order(msg);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == FixParseError::InvalidMsgType);
}

TEST_CASE("FIX: invalid TimeInForce rejected", "[fix]") {
    std::string msg = make_fix_nos();
    const auto pos = msg.find("59=3");
    REQUIRE(pos != std::string::npos);
    msg.replace(pos, 4, "59=9");

    auto result = parse_fix_new_order(msg);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == FixParseError::InvalidTag);
}
