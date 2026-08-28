#include <catch2/catch_test_macros.hpp>
#include "risk/eq_engine.hpp"

using namespace options_ats_core::risk;
using namespace options_ats_core::core;

TEST_CASE("EqEngine: empty engine returns NoData", "[eq]") {
    EqEngine engine;
    auto result = engine.compute_scores(EqWeights{});
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == EqError::NoData);
}

TEST_CASE("EqEngine: invalid weights rejected", "[eq]") {
    EqEngine engine;
    engine.record_fill(0, FixedPrice::from_double(10.03),
                       FixedPrice::from_double(10.05), 1000);
    engine.record_opportunity(0);

    auto result = engine.compute_scores(EqWeights{1000, 1000, 1000}); // Sum != 10000.
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == EqError::InvalidWeights);
}

TEST_CASE("EqEngine: single MM gets max score", "[eq]") {
    EqEngine engine;
    engine.record_fill(0, FixedPrice::from_double(10.03),
                       FixedPrice::from_double(10.05), 500);
    engine.record_opportunity(0);

    auto result = engine.compute_scores(EqWeights{});
    REQUIRE(result.has_value());
    REQUIRE(result->size() == 1);
    REQUIRE(result->front().mm_id == 0);
    REQUIRE(result->front().rank == 1);
    REQUIRE(result->front().composite_bps == 10'000);
}

TEST_CASE("EqEngine: fill rate computed correctly", "[eq]") {
    EqEngine engine;
    // MM 0: 80 fills out of 100 opportunities = 8000 bps.
    for (int i = 0; i < 80; ++i) {
        engine.record_fill(0, FixedPrice::from_double(10.03),
                           FixedPrice::from_double(10.05), 500);
    }
    for (int i = 0; i < 100; ++i) {
        engine.record_opportunity(0);
    }

    REQUIRE(engine.metrics(0).fill_rate_bps() == 8000);
    REQUIRE(engine.metrics(0).fill_count == 80);
}

TEST_CASE("EqEngine: better price improvement ranks higher", "[eq]") {
    EqEngine engine;
    // MM 0: fills at $10.03 vs NBBO $10.05 = $0.02 improvement.
    engine.record_fill(0, FixedPrice::from_double(10.03),
                       FixedPrice::from_double(10.05), 500);
    engine.record_opportunity(0);

    // MM 1: fills at $10.04 vs NBBO $10.05 = $0.01 improvement.
    engine.record_fill(1, FixedPrice::from_double(10.04),
                       FixedPrice::from_double(10.05), 500);
    engine.record_opportunity(1);

    auto result = engine.compute_scores(
        EqWeights{.price_improvement_weight = 10'000,
                  .fill_rate_weight = 0,
                  .response_time_weight = 0});
    REQUIRE(result.has_value());
    REQUIRE(result->size() == 2);
    REQUIRE(result->front().mm_id == 0); // Better improvement.
    REQUIRE(result->front().rank == 1);
}

TEST_CASE("EqEngine: faster response time ranks higher", "[eq]") {
    EqEngine engine;
    // MM 0: 200ns response.
    engine.record_fill(0, FixedPrice::from_double(10.03),
                       FixedPrice::from_double(10.05), 200);
    engine.record_opportunity(0);

    // MM 1: 800ns response (slower).
    engine.record_fill(1, FixedPrice::from_double(10.03),
                       FixedPrice::from_double(10.05), 800);
    engine.record_opportunity(1);

    auto result = engine.compute_scores(
        EqWeights{.price_improvement_weight = 0,
                  .fill_rate_weight = 0,
                  .response_time_weight = 10'000});
    REQUIRE(result.has_value());
    REQUIRE(result->front().mm_id == 0); // Faster response.
}

TEST_CASE("EqEngine: reset clears all metrics", "[eq]") {
    EqEngine engine;
    engine.record_fill(0, FixedPrice::from_double(10.03),
                       FixedPrice::from_double(10.05), 500);
    engine.record_opportunity(0);
    REQUIRE(engine.metrics(0).fill_count == 1);

    engine.reset();
    REQUIRE(engine.metrics(0).fill_count == 0);
}
