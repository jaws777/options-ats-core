#include <catch2/catch_test_macros.hpp>
#include "oms/spread_state_machine.hpp"

using namespace options_ats_core::oms;

namespace {

void require_apply_ok(SpreadStateMachine& sm, SpreadEvent event) {
    auto result = sm.apply(event);
    REQUIRE(result.has_value());
}

void require_submit_ok(SpreadStateMachine& sm) {
    auto result = sm.submit_all();
    REQUIRE(result.has_value());
}

} // namespace

TEST_CASE("Spread: 2-leg happy path to filled", "[spread]") {
    SpreadStateMachine sm{1, 2};
    REQUIRE(sm.composite_state() == SpreadState::New);

    // Submit both legs.
    auto r = sm.submit_all();
    REQUIRE(r.has_value());
    REQUIRE(*r == SpreadState::PendingAccept);

    // Both legs accepted.
    require_apply_ok(sm, SpreadEvent{0, OrderEvent::AcceptAck});
    auto s = sm.apply(SpreadEvent{1, OrderEvent::AcceptAck});
    REQUIRE(s.has_value());
    REQUIRE(*s == SpreadState::Active);

    // Both legs filled.
    require_apply_ok(sm, SpreadEvent{0, OrderEvent::Fill});
    s = sm.apply(SpreadEvent{1, OrderEvent::Fill});
    REQUIRE(s.has_value());
    REQUIRE(*s == SpreadState::Filled);
    REQUIRE(is_terminal(*s));
}

TEST_CASE("Spread: leg 1 fills, leg 2 still active = PartiallyFilled", "[spread]") {
    SpreadStateMachine sm{2, 2};
    require_submit_ok(sm);
    require_apply_ok(sm, SpreadEvent{0, OrderEvent::AcceptAck});
    require_apply_ok(sm, SpreadEvent{1, OrderEvent::AcceptAck});

    // Leg 0 fills, leg 1 still accepted.
    auto s = sm.apply(SpreadEvent{0, OrderEvent::Fill});
    REQUIRE(s.has_value());
    REQUIRE(*s == SpreadState::PartiallyFilled);

    REQUIRE(sm.filled_leg_count() == 1);
    REQUIRE(sm.leg_state(0) == OrderState::Filled);
    REQUIRE(sm.leg_state(1) == OrderState::Accepted);
}

TEST_CASE("Spread: leg 2 rejected, leg 1 cancelled = Rejected", "[spread]") {
    SpreadStateMachine sm{3, 2};
    require_submit_ok(sm);

    // Leg 0 gets accepted first.
    require_apply_ok(sm, SpreadEvent{0, OrderEvent::AcceptAck});

    // Leg 1 rejected by venue.
    require_apply_ok(sm, SpreadEvent{1, OrderEvent::RejectAck});

    // Now we must unwind leg 0. It's Accepted, so we can cancel.
    require_apply_ok(sm, SpreadEvent{0, OrderEvent::CancelRequest});
    auto s = sm.apply(SpreadEvent{0, OrderEvent::CancelAck});
    REQUIRE(s.has_value());
    // One rejected + one cancelled = Rejected.
    REQUIRE(*s == SpreadState::Rejected);
    REQUIRE(is_terminal(*s));
}

TEST_CASE("Spread: cancel_all cancels active legs", "[spread]") {
    SpreadStateMachine sm{4, 2};
    require_submit_ok(sm);
    require_apply_ok(sm, SpreadEvent{0, OrderEvent::AcceptAck});
    require_apply_ok(sm, SpreadEvent{1, OrderEvent::AcceptAck});

    auto s = sm.cancel_all();
    REQUIRE(s.has_value());
    REQUIRE(*s == SpreadState::PendingCancel);

    // Both cancel acks.
    require_apply_ok(sm, SpreadEvent{0, OrderEvent::CancelAck});
    s = sm.apply(SpreadEvent{1, OrderEvent::CancelAck});
    REQUIRE(*s == SpreadState::Cancelled);
}

TEST_CASE("Spread: 4-leg iron condor fills all legs", "[spread]") {
    SpreadStateMachine sm{5, 4};
    require_submit_ok(sm);

    // Accept all 4 legs.
    for (uint32_t i = 0; i < 4; ++i) {
        require_apply_ok(sm, SpreadEvent{i, OrderEvent::AcceptAck});
    }
    REQUIRE(sm.composite_state() == SpreadState::Active);

    // Fill all 4 legs.
    for (uint32_t i = 0; i < 4; ++i) {
        require_apply_ok(sm, SpreadEvent{i, OrderEvent::Fill});
    }
    REQUIRE(sm.composite_state() == SpreadState::Filled);
    REQUIRE(sm.filled_leg_count() == 4);
}

TEST_CASE("Spread: fill during pending cancel (race)", "[spread]") {
    SpreadStateMachine sm{6, 2};
    require_submit_ok(sm);
    require_apply_ok(sm, SpreadEvent{0, OrderEvent::AcceptAck});
    require_apply_ok(sm, SpreadEvent{1, OrderEvent::AcceptAck});

    // Cancel requested on both legs.
    require_apply_ok(sm, SpreadEvent{0, OrderEvent::CancelRequest});
    require_apply_ok(sm, SpreadEvent{1, OrderEvent::CancelRequest});

    // Fill races cancel on leg 0. Fill wins.
    require_apply_ok(sm, SpreadEvent{0, OrderEvent::Fill});
    REQUIRE(sm.leg_state(0) == OrderState::Filled);

    // Leg 1 cancel succeeds.
    auto s = sm.apply(SpreadEvent{1, OrderEvent::CancelAck});
    // One filled, one cancelled → LegDivergence (inconsistent terminal
    // states that require manual intervention — filled leg can't be unwound).
    REQUIRE(s.has_value());
    REQUIRE(*s == SpreadState::LegDivergence);
}

TEST_CASE("Spread: invalid leg index rejected", "[spread]") {
    SpreadStateMachine sm{7, 2};
    auto r = sm.apply(SpreadEvent{5, OrderEvent::Submit}); // Index 5 > leg_count 2.
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error() == SpreadError::InvalidLegIndex);
}

TEST_CASE("Spread: terminal spread rejects further events", "[spread]") {
    SpreadStateMachine sm{8, 2};
    require_submit_ok(sm);
    require_apply_ok(sm, SpreadEvent{0, OrderEvent::AcceptAck});
    require_apply_ok(sm, SpreadEvent{1, OrderEvent::AcceptAck});
    require_apply_ok(sm, SpreadEvent{0, OrderEvent::Fill});
    require_apply_ok(sm, SpreadEvent{1, OrderEvent::Fill});

    REQUIRE(sm.composite_state() == SpreadState::Filled);

    // Attempt to apply event to terminal spread.
    auto r = sm.apply(SpreadEvent{0, OrderEvent::CancelRequest});
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error() == SpreadError::SpreadTerminal);
}

TEST_CASE("Spread: partial fill on one leg", "[spread]") {
    SpreadStateMachine sm{9, 2};
    require_submit_ok(sm);
    require_apply_ok(sm, SpreadEvent{0, OrderEvent::AcceptAck});
    require_apply_ok(sm, SpreadEvent{1, OrderEvent::AcceptAck});

    // Leg 0 partially fills.
    auto s = sm.apply(SpreadEvent{0, OrderEvent::PartialFill});
    REQUIRE(s.has_value());
    REQUIRE(*s == SpreadState::Active); // Both legs still active.
    REQUIRE(sm.leg_state(0) == OrderState::PartiallyFilled);
}
