/// test_end_to_end.cpp — Integration test: full order lifecycle.
///
/// Traces a single retail order through every component in the ATS:
///   1. FIX parse (client gateway)
///   2. OMS state transition (New → PendingAccept → Accepted)
///   3. Pre-trade risk check (atomic position limit)
///   4. SeriesBook match (pro-rata against MM quotes, NBBO compliance)
///   5. WAL append (audit trail)
///   6. EQ metrics update (price improvement, fill rate)
///   7. OMS terminal state (Accepted → Filled)
///
/// This is the test Adam will ask about: "Walk me through a trade
/// from FIX ingest to fill confirmation."

#include <catch2/catch_test_macros.hpp>

#include "oms/fix_parser.hpp"
#include "oms/order_state_machine.hpp"
#include "market_data/series_book.hpp"
#include "risk/pre_trade.hpp"
#include "risk/eq_engine.hpp"
#include "infra/wal.hpp"
#include "infra/seqlock.hpp"

using namespace options_ats_core;

TEST_CASE("End-to-end: retail buy order through full ATS path", "[e2e]") {
    // --- Setup: MMs quoting, NBBO, risk limits ---

    // Two MMs quoting AAPL 260618C00195000.
    market_data::SeriesBook book;
    book.update_quote(market_data::MmQuote{
        .mm_id = 0,  // Optiver
        .bid = core::FixedPrice::from_raw(190'000'000),   // $1.90
        .ask = core::FixedPrice::from_raw(195'000'000),   // $1.95
        .bid_size = 100,
        .ask_size = 100,
        .timestamp_tsc = 1000,
        .active = true,
    });
    book.update_quote(market_data::MmQuote{
        .mm_id = 1,  // Virtu
        .bid = core::FixedPrice::from_raw(190'000'000),   // $1.90
        .ask = core::FixedPrice::from_raw(195'000'000),   // $1.95
        .bid_size = 100,
        .ask_size = 200,
        .timestamp_tsc = 1001,
        .active = true,
    });

    // NBBO from OPRA (stored in seqlock in production).
    auto nbbo = market_data::NbboEntry{
        .bid = core::FixedPrice::from_raw(189'000'000),   // $1.89 (our MMs are tighter)
        .ask = core::FixedPrice::from_raw(197'000'000),   // $1.97
        .timestamp_ns = 1'000'000'000,
    };

    // Risk engine.
    risk::PreTradeRiskCheck risk_check;
    risk_check.update_limits(risk::RiskLimits{
        .max_position = 10'000,
        .max_notional_raw = 0,  // No notional limit for this test.
        .max_orders_per_second = 1'000,
    });

    // EQ engine.
    risk::EqEngine eq;

    // WAL.
    infra::Wal wal(1000);

    // --- Step 1: Parse FIX NewOrderSingle ---

    std::string fix_msg;
    fix_msg += "35=D\x01";      // NewOrderSingle
    fix_msg += "11=ORD001\x01"; // ClOrdID
    fix_msg += "54=1\x01";      // Buy
    fix_msg += "38=150\x01";    // 150 contracts
    fix_msg += "55=AAPL\x01";   // Symbol
    fix_msg += "59=3\x01";      // IOC

    auto parsed = oms::parse_fix_new_order(fix_msg);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->msg_type == oms::FixMsgType::NewOrderSingle);
    REQUIRE(parsed->cl_ord_id == "ORD001");
    REQUIRE(parsed->side == oms::FixSide::Buy);
    REQUIRE(parsed->quantity == 150);
    REQUIRE(parsed->symbol == "AAPL");

    // --- Step 2: OMS state transition — New → PendingAccept ---

    oms::OrderStateMachine osm{1};  // Internal order ID = 1.
    REQUIRE(osm.state() == oms::OrderState::New);

    auto submit = osm.apply(oms::OrderEvent::Submit);
    REQUIRE(submit.has_value());
    REQUIRE(osm.state() == oms::OrderState::PendingAccept);

    // WAL: log order submission.
    auto wal_submit = wal.append(infra::EventType::NewOrder,
                                 1'000'000'100, // timestamp_ns
                                 1,             // entity_id = order_id
                                 parsed->quantity);
    REQUIRE(wal_submit.has_value());

    // --- Step 3: Pre-trade risk check ---

    // Buy 150 contracts — check position limit.
    auto risk_result = risk_check.check_position(150);
    REQUIRE(risk_result.has_value());
    REQUIRE(*risk_result == 150);

    // Order rate check.
    auto rate_result = risk_check.check_order_rate();
    REQUIRE(rate_result.has_value());

    // --- Step 4: OMS — PendingAccept → Accepted (risk passed) ---

    auto accept = osm.apply(oms::OrderEvent::AcceptAck);
    REQUIRE(accept.has_value());
    REQUIRE(osm.state() == oms::OrderState::Accepted);

    // --- Step 5: Match in SeriesBook ---

    // Build the retail order for the matching engine.
    auto retail_order = market_data::RetailOrder{
        .client_order_id = 1,
        .side = market_data::Side::Buy,
        .quantity = 150,
        .timestamp_tsc = 2000,
    };

    auto match_result = book.match_retail_order(retail_order, nbbo);
    REQUIRE(match_result.has_value());

    // Verify fills: 150 contracts pro-rata across 2 MMs at $1.95.
    // MM0: 100 ask_size, MM1: 200 ask_size. Total = 300.
    // Pro-rata: MM0 gets (150 * 100) / 300 = 50, MM1 gets (150 * 200) / 300 = 100.
    int32_t total_filled = 0;
    for (const auto& fill : *match_result) {
        total_filled += fill.quantity;

        // Every fill has NBBO snapshot for price improvement.
        REQUIRE(fill.nbbo_ask.to_raw() == 197'000'000);
        REQUIRE(fill.nbbo_bid.to_raw() == 189'000'000);

        // Fill price ($1.95) is better than NBBO ask ($1.97).
        // Price improvement = $0.02 per contract.
        auto improvement = fill.nbbo_ask - fill.price;
        REQUIRE(improvement.to_raw() == 2'000'000); // $0.02

        // --- Step 6: WAL append per fill ---
        auto wal_fill = wal.append(infra::EventType::Fill,
                                   2'000'000'000,    // timestamp_ns
                                   1,                // order_id
                                   fill.price.to_raw(),
                                   fill.quantity);
        REQUIRE(wal_fill.has_value());

        // --- Step 7: EQ metrics update ---
        eq.record_fill(fill.mm_id, fill.price, nbbo.ask, 500); // 500ns response
        eq.record_opportunity(fill.mm_id);
    }

    REQUIRE(total_filled == 150);

    // --- Step 8: OMS — Accepted → Filled (terminal) ---

    auto filled = osm.apply(oms::OrderEvent::Fill);
    REQUIRE(filled.has_value());
    REQUIRE(osm.state() == oms::OrderState::Filled);
    REQUIRE(osm.is_terminal());

    // --- Verify final state ---

    // WAL has 3 entries: 1 submit + 2 fills.
    REQUIRE(wal.size() == 3);

    // Risk: position is now +150.
    REQUIRE(risk_check.net_position() == 150);

    // EQ: both MMs have fill data.
    REQUIRE(eq.metrics(0).fill_count > 0);
    REQUIRE(eq.metrics(1).fill_count > 0);

    // Compute EQ scores.
    auto scores = eq.compute_scores(risk::EqWeights{});
    REQUIRE(scores.has_value());
    REQUIRE(scores->size() == 2);

    // MM1 got more volume (pro-rata) — verify it's tracked.
    uint64_t mm0_fills = eq.metrics(0).fill_count;
    uint64_t mm1_fills = eq.metrics(1).fill_count;
    REQUIRE(mm0_fills == 1);
    REQUIRE(mm1_fills == 1);
}

TEST_CASE("End-to-end: NBBO violation rejects order", "[e2e]") {
    // Setup: MM quoting above NBBO — our price is worse than the market.
    market_data::SeriesBook book;
    book.update_quote(market_data::MmQuote{
        .mm_id = 0,
        .bid = core::FixedPrice::from_raw(190'000'000),
        .ask = core::FixedPrice::from_raw(200'000'000),  // $2.00 — above NBBO.
        .bid_size = 100,
        .ask_size = 100,
        .timestamp_tsc = 1000,
        .active = true,
    });

    // NBBO ask is $1.97 — our $2.00 ask violates Rule 611.
    auto nbbo = market_data::NbboEntry{
        .bid = core::FixedPrice::from_raw(189'000'000),
        .ask = core::FixedPrice::from_raw(197'000'000),
        .timestamp_ns = 1'000'000'000,
    };

    auto order = market_data::RetailOrder{
        .client_order_id = 2,
        .side = market_data::Side::Buy,
        .quantity = 50,
        .timestamp_tsc = 3000,
    };

    auto result = book.match_retail_order(order, nbbo);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == market_data::AtsMatchError::NbboViolation);
}

TEST_CASE("End-to-end: risk limit rejects order", "[e2e]") {
    risk::PreTradeRiskCheck risk_check;
    risk_check.update_limits(risk::RiskLimits{.max_position = 100});

    // Fill position to near limit.
    REQUIRE(risk_check.check_position(90).has_value());

    // This order would breach the limit.
    auto result = risk_check.check_position(20); // 90 + 20 = 110 > 100
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == risk::RiskReject::PositionLimitBreached);

    // Position unchanged.
    REQUIRE(risk_check.net_position() == 90);
}

TEST_CASE("End-to-end: WAL replay parity", "[e2e]") {
    // Simulate two identical matching runs and verify WAL parity.
    // This exercises deterministic replay parity as a candidate deployment gate.

    auto run_matching = []() -> infra::Wal {
        infra::Wal wal(100);

        market_data::SeriesBook book;
        book.update_quote(market_data::MmQuote{
            .mm_id = 0,
            .bid = core::FixedPrice::from_raw(190'000'000),
            .ask = core::FixedPrice::from_raw(195'000'000),
            .bid_size = 100, .ask_size = 100,
            .timestamp_tsc = 1000, .active = true,
        });

        auto nbbo = market_data::NbboEntry{
            .bid = core::FixedPrice::from_raw(189'000'000),
            .ask = core::FixedPrice::from_raw(197'000'000),
            .timestamp_ns = 1'000'000'000,
        };

        // Same input: buy 50 contracts.
        auto order = market_data::RetailOrder{
            .client_order_id = 1,
            .side = market_data::Side::Buy,
            .quantity = 50,
            .timestamp_tsc = 2000,
        };

        auto new_order_seq = wal.append(infra::EventType::NewOrder, 0, 1, 50);
        REQUIRE(new_order_seq.has_value());

        auto fills = book.match_retail_order(order, nbbo);
        if (fills.has_value()) {
            for (const auto& f : *fills) {
                auto fill_seq = wal.append(infra::EventType::Fill, 0, 1,
                                           f.price.to_raw(), f.quantity);
                REQUIRE(fill_seq.has_value());
            }
        }

        return wal;
    };

    // Two identical runs must produce identical WALs.
    auto wal_a = run_matching();
    auto wal_b = run_matching();

    auto parity = wal_a.verify_parity(wal_b);
    REQUIRE(parity.has_value());
    REQUIRE(*parity == true);
}
