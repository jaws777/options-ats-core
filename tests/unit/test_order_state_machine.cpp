#include <catch2/catch_test_macros.hpp>
#include "oms/order_state_machine.hpp"

using namespace options_ats_core::oms;

// --- Pure transition function tests ---

TEST_CASE("OrderStateMachine: happy path - new to filled", "[osm]") {
    auto s = OrderState::New;

    auto r1 = transition(s, OrderEvent::Submit);
    REQUIRE(r1.has_value());
    REQUIRE(*r1 == OrderState::PendingAccept);
    s = *r1;

    auto r2 = transition(s, OrderEvent::AcceptAck);
    REQUIRE(r2.has_value());
    REQUIRE(*r2 == OrderState::Accepted);
    s = *r2;

    auto r3 = transition(s, OrderEvent::Fill);
    REQUIRE(r3.has_value());
    REQUIRE(*r3 == OrderState::Filled);
}

TEST_CASE("OrderStateMachine: partial fill then full fill", "[osm]") {
    auto s = OrderState::Accepted;

    auto r1 = transition(s, OrderEvent::PartialFill);
    REQUIRE(*r1 == OrderState::PartiallyFilled);
    s = *r1;

    auto r2 = transition(s, OrderEvent::PartialFill);
    REQUIRE(*r2 == OrderState::PartiallyFilled); // Still partial.
    s = *r2;

    auto r3 = transition(s, OrderEvent::Fill);
    REQUIRE(*r3 == OrderState::Filled);
}

TEST_CASE("OrderStateMachine: cancel from accepted", "[osm]") {
    auto s = OrderState::Accepted;

    auto r1 = transition(s, OrderEvent::CancelRequest);
    REQUIRE(*r1 == OrderState::PendingCancel);
    s = *r1;

    auto r2 = transition(s, OrderEvent::CancelAck);
    REQUIRE(*r2 == OrderState::Cancelled);
}

TEST_CASE("OrderStateMachine: cancel rejected returns to accepted", "[osm]") {
    auto s = OrderState::PendingCancel;

    auto r = transition(s, OrderEvent::CancelReject);
    REQUIRE(r.has_value());
    REQUIRE(*r == OrderState::Accepted);
}

TEST_CASE("OrderStateMachine: fill while pending cancel", "[osm]") {
    // Race condition: cancel request sent but fill arrives first.
    auto s = OrderState::PendingCancel;

    auto r = transition(s, OrderEvent::Fill);
    REQUIRE(r.has_value());
    REQUIRE(*r == OrderState::Filled);
}

TEST_CASE("OrderStateMachine: partial fill while pending cancel", "[osm]") {
    auto s = OrderState::PendingCancel;

    auto r = transition(s, OrderEvent::PartialFill);
    REQUIRE(r.has_value());
    REQUIRE(*r == OrderState::PendingCancel); // Still pending cancel.
}

TEST_CASE("OrderStateMachine: reject from pending accept", "[osm]") {
    auto s = OrderState::PendingAccept;

    auto r = transition(s, OrderEvent::RejectAck);
    REQUIRE(r.has_value());
    REQUIRE(*r == OrderState::Rejected);
}

TEST_CASE("OrderStateMachine: fill during pending accept (exchange race)", "[osm]") {
    // Some venues fill before sending accept ack.
    auto s = OrderState::PendingAccept;

    auto r = transition(s, OrderEvent::Fill);
    REQUIRE(r.has_value());
    REQUIRE(*r == OrderState::Filled);
}

// --- Invalid transition tests ---

TEST_CASE("OrderStateMachine: terminal states reject all events", "[osm]") {
    for (auto terminal : {OrderState::Filled, OrderState::Cancelled,
                          OrderState::Rejected}) {
        for (auto event : {OrderEvent::Submit, OrderEvent::AcceptAck,
                           OrderEvent::RejectAck, OrderEvent::PartialFill,
                           OrderEvent::Fill, OrderEvent::CancelRequest,
                           OrderEvent::CancelAck, OrderEvent::CancelReject}) {
            auto r = transition(terminal, event);
            REQUIRE_FALSE(r.has_value());
            REQUIRE(r.error() == TransitionError::AlreadyTerminal);
        }
    }
}

TEST_CASE("OrderStateMachine: New only accepts Submit", "[osm]") {
    for (auto event : {OrderEvent::AcceptAck, OrderEvent::RejectAck,
                       OrderEvent::Fill, OrderEvent::CancelRequest,
                       OrderEvent::CancelAck, OrderEvent::CancelReject}) {
        auto r = transition(OrderState::New, event);
        REQUIRE_FALSE(r.has_value());
        REQUIRE(r.error() == TransitionError::InvalidTransition);
    }
}

TEST_CASE("OrderStateMachine: Accepted rejects Submit and CancelAck", "[osm]") {
    auto r1 = transition(OrderState::Accepted, OrderEvent::Submit);
    REQUIRE_FALSE(r1.has_value());

    auto r2 = transition(OrderState::Accepted, OrderEvent::CancelAck);
    REQUIRE_FALSE(r2.has_value());

    auto r3 = transition(OrderState::Accepted, OrderEvent::CancelReject);
    REQUIRE_FALSE(r3.has_value());
}

// --- OrderStateMachine class tests ---

TEST_CASE("OrderStateMachine: class tracks state and transitions", "[osm]") {
    OrderStateMachine sm{12345};

    REQUIRE(sm.state() == OrderState::New);
    REQUIRE(sm.order_id() == 12345);
    REQUIRE(sm.transition_count() == 0);
    REQUIRE_FALSE(sm.is_terminal());

    REQUIRE(sm.apply(OrderEvent::Submit).has_value());
    REQUIRE(sm.state() == OrderState::PendingAccept);
    REQUIRE(sm.transition_count() == 1);

    REQUIRE(sm.apply(OrderEvent::AcceptAck).has_value());
    REQUIRE(sm.apply(OrderEvent::Fill).has_value());

    REQUIRE(sm.state() == OrderState::Filled);
    REQUIRE(sm.is_terminal());
    REQUIRE(sm.transition_count() == 3);
}

TEST_CASE("OrderStateMachine: failed transition does not change state", "[osm]") {
    OrderStateMachine sm{1};

    auto r = sm.apply(OrderEvent::Fill); // Invalid from New.
    REQUIRE_FALSE(r.has_value());
    REQUIRE(sm.state() == OrderState::New); // Unchanged.
    REQUIRE(sm.transition_count() == 0);
}

TEST_CASE("OrderStateMachine: to_string coverage", "[osm]") {
    REQUIRE(to_string(OrderState::New) == "New");
    REQUIRE(to_string(OrderState::Filled) == "Filled");
    REQUIRE(to_string(OrderEvent::Submit) == "Submit");
    REQUIRE(to_string(OrderEvent::CancelAck) == "CancelAck");
    REQUIRE(to_string(TransitionError::InvalidTransition) == "InvalidTransition");
    REQUIRE(to_string(TransitionError::AlreadyTerminal) == "AlreadyTerminal");
}
