#include <catch2/catch_test_macros.hpp>
#include "market_data/series_book.hpp"

using namespace options_ats_core::market_data;
using namespace options_ats_core::core;

namespace {

MmQuote make_quote(uint32_t mm_id, double bid, double ask,
                   int32_t bid_size, int32_t ask_size, uint64_t ts = 0) {
    return MmQuote{
        .mm_id = mm_id,
        .bid = FixedPrice::from_raw(static_cast<int64_t>(
            bid * static_cast<double>(FixedPrice::kScale))),
        .ask = FixedPrice::from_raw(static_cast<int64_t>(
            ask * static_cast<double>(FixedPrice::kScale))),
        .bid_size = bid_size,
        .ask_size = ask_size,
        .timestamp_tsc = ts,
        .active = true,
    };
}

NbboEntry make_nbbo(double bid, double ask, uint64_t ts = 1000) {
    return NbboEntry{
        .bid = FixedPrice::from_raw(static_cast<int64_t>(
            bid * static_cast<double>(FixedPrice::kScale))),
        .ask = FixedPrice::from_raw(static_cast<int64_t>(
            ask * static_cast<double>(FixedPrice::kScale))),
        .timestamp_ns = ts,
    };
}

RetailOrder make_retail(uint64_t id, Side side, int32_t qty, uint64_t ts = 500) {
    return RetailOrder{
        .client_order_id = id,
        .side = side,
        .quantity = qty,
        .timestamp_tsc = ts,
    };
}

} // namespace

// --- Quote Management ---

TEST_CASE("SeriesBook: empty book has no best bid/ask", "[series_book]") {
    SeriesBook book;
    REQUIRE(book.best_bid() == nullptr);
    REQUIRE(book.best_ask() == nullptr);
    REQUIRE(book.active_mm_count() == 0);
}

TEST_CASE("SeriesBook: single MM quote", "[series_book]") {
    SeriesBook book;
    book.update_quote(make_quote(0, 10.00, 10.05, 100, 100));

    REQUIRE(book.active_mm_count() == 1);
    REQUIRE(book.best_bid() != nullptr);
    REQUIRE(book.best_ask() != nullptr);
    REQUIRE(book.best_bid()->mm_id == 0);
    REQUIRE(book.best_ask()->mm_id == 0);
}

TEST_CASE("SeriesBook: best bid is highest across MMs", "[series_book]") {
    SeriesBook book;
    book.update_quote(make_quote(0, 10.00, 10.05, 100, 100));
    book.update_quote(make_quote(1, 10.02, 10.07, 100, 100));
    book.update_quote(make_quote(2, 9.98, 10.03, 100, 100));

    REQUIRE(book.best_bid()->mm_id == 1);  // $10.02 is highest bid.
    REQUIRE(book.best_ask()->mm_id == 2);  // $10.03 is lowest ask.
}

TEST_CASE("SeriesBook: withdraw removes MM from best-of-book", "[series_book]") {
    SeriesBook book;
    book.update_quote(make_quote(0, 10.02, 10.05, 100, 100)); // Best bid.
    book.update_quote(make_quote(1, 10.00, 10.07, 100, 100));

    REQUIRE(book.best_bid()->mm_id == 0);

    book.withdraw_quote(0);
    REQUIRE(book.best_bid()->mm_id == 1);  // Falls to MM 1.
    REQUIRE(book.active_mm_count() == 1);
}

TEST_CASE("SeriesBook: quote update replaces previous", "[series_book]") {
    SeriesBook book;
    book.update_quote(make_quote(0, 10.00, 10.05, 100, 100));
    book.update_quote(make_quote(0, 10.01, 10.04, 200, 200)); // Update.

    REQUIRE(book.quote(0).bid_size == 200);
    auto bid = book.best_bid();
    REQUIRE(bid != nullptr);
    // $10.01 bid after update.
    REQUIRE(bid->bid.to_raw() == static_cast<int64_t>(10.01 * FixedPrice::kScale));
}

// --- Matching: Basic ---

TEST_CASE("SeriesBook: retail buy fills against best MM ask", "[series_book]") {
    SeriesBook book;
    book.update_quote(make_quote(0, 10.00, 10.05, 100, 100));
    book.update_quote(make_quote(1, 10.00, 10.03, 100, 50));  // Better ask.

    auto nbbo = make_nbbo(10.00, 10.05);
    auto order = make_retail(1, Side::Buy, 30);

    auto result = book.match_retail_order(order, nbbo);
    REQUIRE(result.has_value());
    REQUIRE(result->size() == 1);
    REQUIRE(result->front().mm_id == 1);         // Filled against MM 1 (best ask).
    REQUIRE(result->front().quantity == 30);

    // MM 1's ask size should be decremented.
    REQUIRE(book.quote(1).ask_size == 20);
}

TEST_CASE("SeriesBook: retail sell fills against best MM bid", "[series_book]") {
    SeriesBook book;
    book.update_quote(make_quote(0, 10.02, 10.05, 100, 100)); // Best bid.
    book.update_quote(make_quote(1, 10.00, 10.03, 100, 100));

    auto nbbo = make_nbbo(10.00, 10.05);
    auto order = make_retail(1, Side::Sell, 50);

    auto result = book.match_retail_order(order, nbbo);
    REQUIRE(result.has_value());
    REQUIRE(result->size() == 1);
    REQUIRE(result->front().mm_id == 0);  // MM 0 has best bid $10.02.
    REQUIRE(result->front().quantity == 50);
}

// --- Matching: Pro-Rata ---

TEST_CASE("SeriesBook: pro-rata allocation at same price", "[series_book]") {
    SeriesBook book;
    // Three MMs at same ask = $10.05, sizes 100, 200, 300 (total 600).
    book.update_quote(make_quote(0, 10.00, 10.05, 100, 100));
    book.update_quote(make_quote(1, 10.00, 10.05, 100, 200));
    book.update_quote(make_quote(2, 10.00, 10.05, 100, 300));

    auto nbbo = make_nbbo(10.00, 10.05);
    auto order = make_retail(1, Side::Buy, 300);

    auto result = book.match_retail_order(order, nbbo);
    REQUIRE(result.has_value());

    // Pro-rata: 300 * (100/600) = 50, 300 * (200/600) = 100, 300 * (300/600) = 150.
    int32_t total_filled = 0;
    for (const auto& fill : *result) {
        total_filled += fill.quantity;
        if (fill.mm_id == 0) REQUIRE(fill.quantity == 50);
        if (fill.mm_id == 1) REQUIRE(fill.quantity == 100);
        if (fill.mm_id == 2) REQUIRE(fill.quantity == 150);
    }
    REQUIRE(total_filled == 300);
}

TEST_CASE("SeriesBook: pro-rata residual distributed to first MM", "[series_book]") {
    SeriesBook book;
    // Two MMs at same ask, sizes 100, 100 (total 200). Order for 3.
    // Pro-rata: 3 * 100/200 = 1 each. Residual = 1 → goes to first MM.
    book.update_quote(make_quote(0, 10.00, 10.05, 100, 100));
    book.update_quote(make_quote(1, 10.00, 10.05, 100, 100));

    auto nbbo = make_nbbo(10.00, 10.05);
    auto order = make_retail(1, Side::Buy, 3);

    auto result = book.match_retail_order(order, nbbo);
    REQUIRE(result.has_value());

    int32_t total_filled = 0;
    for (const auto& fill : *result) total_filled += fill.quantity;
    REQUIRE(total_filled == 3);
}

// --- Matching: Multi-Level ---

TEST_CASE("SeriesBook: sweeps to next price level", "[series_book]") {
    SeriesBook book;
    book.update_quote(make_quote(0, 10.00, 10.03, 100, 30));  // Best ask, small.
    book.update_quote(make_quote(1, 10.00, 10.05, 100, 100)); // Worse ask, large.

    auto nbbo = make_nbbo(10.00, 10.05);
    auto order = make_retail(1, Side::Buy, 50);

    auto result = book.match_retail_order(order, nbbo);
    REQUIRE(result.has_value());
    REQUIRE(result->size() == 2);

    // First fill: 30 at $10.03 from MM 0.
    REQUIRE((*result)[0].mm_id == 0);
    REQUIRE((*result)[0].quantity == 30);

    // Second fill: 20 at $10.05 from MM 1.
    REQUIRE((*result)[1].mm_id == 1);
    REQUIRE((*result)[1].quantity == 20);
}

// --- NBBO Compliance ---

TEST_CASE("SeriesBook: NBBO violation rejected on buy", "[series_book]") {
    SeriesBook book;
    // MM ask is $10.10, but NBBO ask is $10.05. Our fill would be worse.
    book.update_quote(make_quote(0, 10.00, 10.10, 100, 100));

    auto nbbo = make_nbbo(10.00, 10.05);
    auto order = make_retail(1, Side::Buy, 50);

    auto result = book.match_retail_order(order, nbbo);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == AtsMatchError::NbboViolation);
}

TEST_CASE("SeriesBook: NBBO violation rejected on sell", "[series_book]") {
    SeriesBook book;
    // MM bid is $9.95, but NBBO bid is $10.00. Our fill would be worse.
    book.update_quote(make_quote(0, 9.95, 10.05, 100, 100));

    auto nbbo = make_nbbo(10.00, 10.05);
    auto order = make_retail(1, Side::Sell, 50);

    auto result = book.match_retail_order(order, nbbo);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == AtsMatchError::NbboViolation);
}

TEST_CASE("SeriesBook: fill at NBBO is valid (equal, not better)", "[series_book]") {
    SeriesBook book;
    // MM ask exactly at NBBO ask — valid.
    book.update_quote(make_quote(0, 10.00, 10.05, 100, 100));

    auto nbbo = make_nbbo(10.00, 10.05);
    auto order = make_retail(1, Side::Buy, 50);

    auto result = book.match_retail_order(order, nbbo);
    REQUIRE(result.has_value());
    REQUIRE(result->front().quantity == 50);
}

TEST_CASE("SeriesBook: price improvement captured in fill", "[series_book]") {
    SeriesBook book;
    // MM ask is $10.03, NBBO ask is $10.05. Price improvement = $0.02.
    book.update_quote(make_quote(0, 10.00, 10.03, 100, 100));

    auto nbbo = make_nbbo(10.00, 10.05);
    auto order = make_retail(1, Side::Buy, 50);

    auto result = book.match_retail_order(order, nbbo);
    REQUIRE(result.has_value());
    auto& fill = result->front();
    // Fill carries the NBBO snapshot for price improvement calculation.
    REQUIRE(fill.nbbo_ask.to_raw() == nbbo.ask.to_raw());
    // Price improvement = NBBO ask - fill price.
    auto improvement = fill.nbbo_ask - fill.price;
    REQUIRE(improvement.to_raw() > 0);
}

// --- Edge Cases ---

TEST_CASE("SeriesBook: no liquidity returns error", "[series_book]") {
    SeriesBook book;
    auto nbbo = make_nbbo(10.00, 10.05);
    auto order = make_retail(1, Side::Buy, 50);

    auto result = book.match_retail_order(order, nbbo);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == AtsMatchError::NoLiquidity);
}

TEST_CASE("SeriesBook: invalid quantity rejected", "[series_book]") {
    SeriesBook book;
    book.update_quote(make_quote(0, 10.00, 10.05, 100, 100));
    auto nbbo = make_nbbo(10.00, 10.05);

    auto result = book.match_retail_order(make_retail(1, Side::Buy, 0), nbbo);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == AtsMatchError::InvalidQuantity);
}

TEST_CASE("SeriesBook: partial fill when liquidity exhausted", "[series_book]") {
    SeriesBook book;
    // Only 30 contracts available across all MMs.
    book.update_quote(make_quote(0, 10.00, 10.05, 100, 30));

    auto nbbo = make_nbbo(10.00, 10.05);
    auto order = make_retail(1, Side::Buy, 100); // Wants 100.

    auto result = book.match_retail_order(order, nbbo);
    REQUIRE(result.has_value());

    int32_t total = 0;
    for (const auto& f : *result) total += f.quantity;
    REQUIRE(total == 30); // Only got 30.
}

TEST_CASE("SeriesBook: sweep stops at NBBO boundary", "[series_book]") {
    SeriesBook book;
    book.update_quote(make_quote(0, 10.00, 10.03, 100, 20));  // OK — within NBBO.
    book.update_quote(make_quote(1, 10.00, 10.07, 100, 100)); // Above NBBO ask.

    auto nbbo = make_nbbo(10.00, 10.05);
    auto order = make_retail(1, Side::Buy, 100);

    auto result = book.match_retail_order(order, nbbo);
    REQUIRE(result.has_value());

    // Only fills at $10.03 (20 contracts). $10.07 level skipped — NBBO violation.
    int32_t total = 0;
    for (const auto& f : *result) total += f.quantity;
    REQUIRE(total == 20);
    REQUIRE(result->size() == 1);
    REQUIRE(result->front().mm_id == 0);
}
