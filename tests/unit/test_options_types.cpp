#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "core/options_types.hpp"

using namespace options_ats_core::core;

TEST_CASE("OptionSpec: basic construction and root_view", "[options]") {
    OptionSpec spec{};
    spec.series_id = 12345;
    spec.root[0] = 'A'; spec.root[1] = 'A'; spec.root[2] = 'P';
    spec.root[3] = 'L'; spec.root[4] = '\0';
    spec.expiry_yyyymmdd = 20260618;
    spec.right = OptionRight::Call;
    spec.strike = FixedPrice::from_dollars_cents(195, 0);
    spec.exercise = ExerciseStyle::American;
    spec.settlement = SettlementType::Physical;
    spec.multiplier = 100;

    REQUIRE(spec.root_view() == "AAPL");
    REQUIRE(spec.expiry_yyyymmdd == 20260618);
    REQUIRE(spec.multiplier == 100);
}

TEST_CASE("OptionSpec: DTE calculation", "[options]") {
    OptionSpec spec{};
    spec.expiry_yyyymmdd = 20260618;

    REQUIRE(spec.dte(20260408) > 0);   // Before expiry.
    REQUIRE(spec.dte(20260618) == 0);   // At expiry.
    REQUIRE(spec.dte(20260619) == 0);   // After expiry.
}

TEST_CASE("Greeks: aggregation", "[options]") {
    Greeks g1{.delta = 0.50, .gamma = 0.03, .vega = 0.15, .theta = -0.05, .rho = 0.01};
    Greeks g2{.delta = -0.30, .gamma = 0.02, .vega = 0.10, .theta = -0.03, .rho = 0.005};

    g1 += g2;
    REQUIRE(g1.delta == Catch::Approx(0.20));
    REQUIRE(g1.gamma == Catch::Approx(0.05));
    REQUIRE(g1.vega  == Catch::Approx(0.25));
    REQUIRE(g1.theta == Catch::Approx(-0.08));
}

TEST_CASE("Greeks: scaling by position size", "[options]") {
    Greeks g{.delta = 0.50, .gamma = 0.03, .vega = 0.15, .theta = -0.05, .rho = 0.01};

    auto scaled = g * 100.0;  // 100 contracts.
    REQUIRE(scaled.delta == Catch::Approx(50.0));
    REQUIRE(scaled.gamma == Catch::Approx(3.0));
    REQUIRE(scaled.vega  == Catch::Approx(15.0));
    REQUIRE(scaled.theta == Catch::Approx(-5.0));
}

TEST_CASE("ExerciseStyle: to_string", "[options]") {
    REQUIRE(to_string(ExerciseStyle::American) == "American");
    REQUIRE(to_string(ExerciseStyle::European) == "European");
}

TEST_CASE("SettlementType: to_string", "[options]") {
    REQUIRE(to_string(SettlementType::Physical) == "Physical");
    REQUIRE(to_string(SettlementType::Cash) == "Cash");
}
