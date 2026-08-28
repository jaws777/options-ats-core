#include <gtest/gtest.h>
#include "market_data/series_book.hpp"

using namespace options_ats_core::market_data;
using namespace options_ats_core::core;

namespace {

MmQuote make_quote(uint32_t mm_id, double bid, double ask,
                   int32_t bid_size, int32_t ask_size) {
    return MmQuote{
        .mm_id = mm_id,
        .bid = FixedPrice::from_raw(static_cast<int64_t>(bid * FixedPrice::kScale)),
        .ask = FixedPrice::from_raw(static_cast<int64_t>(ask * FixedPrice::kScale)),
        .bid_size = bid_size,
        .ask_size = ask_size,
        .timestamp_tsc = 0,
        .active = true,
    };
}

NbboEntry make_nbbo(double bid, double ask) {
    return NbboEntry{
        .bid = FixedPrice::from_raw(static_cast<int64_t>(bid * FixedPrice::kScale)),
        .ask = FixedPrice::from_raw(static_cast<int64_t>(ask * FixedPrice::kScale)),
        .timestamp_ns = 1000,
    };
}

RetailOrder make_retail(uint64_t id, Side side, int32_t qty) {
    return RetailOrder{.client_order_id = id, .side = side,
                       .quantity = qty, .timestamp_tsc = 500};
}

} // namespace

TEST(SeriesBookTest, EmptyBookNoLiquidity) {
    SeriesBook book;
    EXPECT_EQ(book.best_bid(), nullptr);
    EXPECT_EQ(book.best_ask(), nullptr);
    EXPECT_EQ(book.active_mm_count(), 0u);
}

TEST(SeriesBookTest, BestBidIsHighestAcrossMMs) {
    SeriesBook book;
    book.update_quote(make_quote(0, 10.00, 10.05, 100, 100));
    book.update_quote(make_quote(1, 10.02, 10.07, 100, 100));
    EXPECT_EQ(book.best_bid()->mm_id, 1u);
}

TEST(SeriesBookTest, RetailBuyFillsAgainstBestAsk) {
    SeriesBook book;
    book.update_quote(make_quote(0, 10.00, 10.05, 100, 100));
    book.update_quote(make_quote(1, 10.00, 10.03, 100, 50));

    auto result = book.match_retail_order(make_retail(1, Side::Buy, 30), make_nbbo(10.00, 10.05));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->size(), 1u);
    EXPECT_EQ(result->front().mm_id, 1u);
    EXPECT_EQ(result->front().quantity, 30);
}

TEST(SeriesBookTest, NbboViolationRejected) {
    SeriesBook book;
    book.update_quote(make_quote(0, 10.00, 10.10, 100, 100)); // Ask above NBBO.

    auto result = book.match_retail_order(make_retail(1, Side::Buy, 50), make_nbbo(10.00, 10.05));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), AtsMatchError::NbboViolation);
}

TEST(SeriesBookTest, ProRataAllocation) {
    SeriesBook book;
    book.update_quote(make_quote(0, 10.00, 10.05, 100, 100));
    book.update_quote(make_quote(1, 10.00, 10.05, 100, 200));
    book.update_quote(make_quote(2, 10.00, 10.05, 100, 300));

    auto result = book.match_retail_order(make_retail(1, Side::Buy, 300), make_nbbo(10.00, 10.05));
    ASSERT_TRUE(result.has_value());

    int32_t total = 0;
    for (const auto& f : *result) total += f.quantity;
    EXPECT_EQ(total, 300);
}

TEST(SeriesBookTest, PriceImprovementCaptured) {
    SeriesBook book;
    book.update_quote(make_quote(0, 10.00, 10.03, 100, 100));

    auto result = book.match_retail_order(make_retail(1, Side::Buy, 50), make_nbbo(10.00, 10.05));
    ASSERT_TRUE(result.has_value());
    auto improvement = result->front().nbbo_ask - result->front().price;
    EXPECT_GT(improvement.to_raw(), 0);
}
