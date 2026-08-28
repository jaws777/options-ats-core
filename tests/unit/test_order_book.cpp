#include <catch2/catch_test_macros.hpp>
#include "market_data/order_book.hpp"

#include <limits>

using namespace options_ats_core::market_data;
using namespace options_ats_core::core;

namespace {

Order make_order(uint64_t id, Side side, double price, int32_t qty,
                 uint64_t ts = 0) {
    return Order{
        .order_id = id,
        .side = side,
        .price = FixedPrice::from_raw(static_cast<int64_t>(
            price * static_cast<double>(FixedPrice::kScale))),
        .quantity = qty,
        .filled_qty = 0,
        .timestamp_tsc = ts,
    };
}

} // namespace

TEST_CASE("OrderBook: add passive orders", "[order_book]") {
    OrderBook book;

    auto r1 = book.add_order(make_order(1, Side::Buy, 10.00, 100));
    REQUIRE(r1.has_value());
    REQUIRE(r1->empty()); // No fills — no asks to match against.

    auto r2 = book.add_order(make_order(2, Side::Sell, 10.05, 50));
    REQUIRE(r2.has_value());
    REQUIRE(r2->empty()); // No fills — ask > bid.

    REQUIRE(book.best_bid() == FixedPrice::from_raw(1'000'000'000));
    REQUIRE(book.best_ask() == FixedPrice::from_raw(1'005'000'000));
    REQUIRE(book.order_count() == 2);
}

TEST_CASE("OrderBook: crossing order produces fill", "[order_book]") {
    OrderBook book;

    REQUIRE(book.add_order(make_order(1, Side::Sell, 10.00, 100)).has_value()); // Resting ask.

    // Buy at $10.00 crosses the ask.
    auto result = book.add_order(make_order(2, Side::Buy, 10.00, 50));
    REQUIRE(result.has_value());
    REQUIRE(result->size() == 1);

    auto& fill = result->front();
    REQUIRE(fill.aggressor_order_id == 2);
    REQUIRE(fill.passive_order_id == 1);
    REQUIRE(fill.quantity == 50);
    REQUIRE(fill.price == FixedPrice::from_raw(1'000'000'000)); // At passive price.

    // 50 remaining on the resting ask.
    REQUIRE(book.ask_level_count() == 1);
    REQUIRE(book.bid_level_count() == 0); // Aggressor fully filled.
}

TEST_CASE("OrderBook: full fill removes order from book", "[order_book]") {
    OrderBook book;

    REQUIRE(book.add_order(make_order(1, Side::Sell, 10.00, 100)).has_value());
    REQUIRE(book.add_order(make_order(2, Side::Buy, 10.00, 100)).has_value()); // Exact match.

    REQUIRE(book.order_count() == 0);
    REQUIRE(book.ask_level_count() == 0);
    REQUIRE(book.bid_level_count() == 0);
}

TEST_CASE("OrderBook: price-time priority", "[order_book]") {
    OrderBook book;

    // Two asks at same price — first one has time priority.
    REQUIRE(book.add_order(make_order(1, Side::Sell, 10.00, 50, 100)).has_value());
    REQUIRE(book.add_order(make_order(2, Side::Sell, 10.00, 50, 200)).has_value());

    // Buy that partially fills the level.
    auto result = book.add_order(make_order(3, Side::Buy, 10.00, 30));
    REQUIRE(result.has_value());
    REQUIRE(result->size() == 1);
    REQUIRE(result->front().passive_order_id == 1); // First order filled.
    REQUIRE(result->front().quantity == 30);
}

TEST_CASE("OrderBook: multi-level sweep", "[order_book]") {
    OrderBook book;

    REQUIRE(book.add_order(make_order(1, Side::Sell, 10.00, 50)).has_value());
    REQUIRE(book.add_order(make_order(2, Side::Sell, 10.01, 50)).has_value());
    REQUIRE(book.add_order(make_order(3, Side::Sell, 10.02, 50)).has_value());

    // Aggressive buy sweeps all three levels.
    auto result = book.add_order(make_order(4, Side::Buy, 10.02, 120));
    REQUIRE(result.has_value());
    REQUIRE(result->size() == 3);

    REQUIRE((*result)[0].quantity == 50); // $10.00
    REQUIRE((*result)[1].quantity == 50); // $10.01
    REQUIRE((*result)[2].quantity == 20); // $10.02 partial

    // 30 remaining on $10.02 ask, nothing else.
    REQUIRE(book.ask_level_count() == 1);
    REQUIRE(book.bid_level_count() == 0);
}

TEST_CASE("OrderBook: cancel order", "[order_book]") {
    OrderBook book;

    REQUIRE(book.add_order(make_order(1, Side::Buy, 10.00, 100)).has_value());
    REQUIRE(book.order_count() == 1);

    auto result = book.cancel_order(1);
    REQUIRE(result.has_value());
    REQUIRE(*result == 100);
    REQUIRE(book.order_count() == 0);
    REQUIRE(book.bid_level_count() == 0);
}

TEST_CASE("OrderBook: cancel non-existent order returns error", "[order_book]") {
    OrderBook book;
    auto result = book.cancel_order(999);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == BookError::OrderNotFound);
}

TEST_CASE("OrderBook: duplicate order ID rejected", "[order_book]") {
    OrderBook book;
    REQUIRE(book.add_order(make_order(1, Side::Buy, 10.00, 100)).has_value());
    auto result = book.add_order(make_order(1, Side::Sell, 10.05, 50));
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == BookError::DuplicateOrderId);
}

TEST_CASE("OrderBook: invalid quantity rejected", "[order_book]") {
    OrderBook book;
    auto result = book.add_order(make_order(1, Side::Buy, 10.00, 0));
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == BookError::InvalidQuantity);
}

TEST_CASE("OrderBook: spread calculation", "[order_book]") {
    OrderBook book;

    REQUIRE_FALSE(book.spread().has_value()); // Empty book.

    REQUIRE(book.add_order(make_order(1, Side::Buy, 10.00, 100)).has_value());
    REQUIRE_FALSE(book.spread().has_value()); // One side only.

    REQUIRE(book.add_order(make_order(2, Side::Sell, 10.05, 50)).has_value());
    auto spread = book.spread();
    REQUIRE(spread.has_value());

    // $10.05 - $10.00 = $0.05 = 5 penny ticks
    REQUIRE(*spread == FixedPrice::nickel_tick());
}

TEST_CASE("OrderBook: depth query", "[order_book]") {
    OrderBook book;

    REQUIRE(book.add_order(make_order(1, Side::Buy, 10.00, 100)).has_value());
    REQUIRE(book.add_order(make_order(2, Side::Buy, 10.00, 50)).has_value());
    REQUIRE(book.add_order(make_order(3, Side::Buy, 9.99, 200)).has_value());

    auto depth = book.depth(Side::Buy, 5);
    REQUIRE(depth.size() == 2);
    REQUIRE(depth[0].second == 150); // $10.00: 100 + 50
    REQUIRE(depth[1].second == 200); // $9.99: 200
}

TEST_CASE("OrderBook: rejects negative filled quantity on entry", "[order_book]") {
    OrderBook book;
    auto bad = make_order(1, Side::Buy, 10.00, 100);
    bad.filled_qty = -1;

    auto result = book.add_order(bad);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == BookError::InvalidQuantity);
}

TEST_CASE("OrderBook: detects fill quantity overflow", "[order_book]") {
    OrderBook book;
    auto passive = make_order(1, Side::Sell, 10.00, 10);
    passive.filled_qty = std::numeric_limits<int32_t>::max();
    REQUIRE(book.add_order(passive).has_value());

    auto result = book.add_order(make_order(2, Side::Buy, 10.00, 1));
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == BookError::QuantityOverflow);
}
