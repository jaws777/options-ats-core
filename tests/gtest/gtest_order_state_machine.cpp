#include <gtest/gtest.h>
#include "oms/order_state_machine.hpp"

using namespace options_ats_core::oms;

TEST(OrderStateMachineTest, HappyPathNewToFilled) {
    auto s = OrderState::New;
    auto r1 = transition(s, OrderEvent::Submit);
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(*r1, OrderState::PendingAccept);

    auto r2 = transition(*r1, OrderEvent::AcceptAck);
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(*r2, OrderState::Accepted);

    auto r3 = transition(*r2, OrderEvent::Fill);
    ASSERT_TRUE(r3.has_value());
    EXPECT_EQ(*r3, OrderState::Filled);
}

TEST(OrderStateMachineTest, CancelFillRace) {
    // Fill arrives while cancel is pending — fill wins.
    auto r = transition(OrderState::PendingCancel, OrderEvent::Fill);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, OrderState::Filled);
}

TEST(OrderStateMachineTest, TerminalStatesRejectAllEvents) {
    for (auto terminal : {OrderState::Filled, OrderState::Cancelled, OrderState::Rejected}) {
        for (auto event : {OrderEvent::Submit, OrderEvent::AcceptAck, OrderEvent::Fill,
                           OrderEvent::CancelRequest, OrderEvent::CancelAck}) {
            auto r = transition(terminal, event);
            EXPECT_FALSE(r.has_value()) << "Terminal " << to_string(terminal)
                                        << " should reject " << to_string(event);
            EXPECT_EQ(r.error(), TransitionError::AlreadyTerminal);
        }
    }
}

TEST(OrderStateMachineTest, ClassTracksState) {
    OrderStateMachine sm{12345};
    EXPECT_EQ(sm.state(), OrderState::New);
    EXPECT_EQ(sm.order_id(), 12345u);

    EXPECT_TRUE(sm.apply(OrderEvent::Submit).has_value());
    EXPECT_TRUE(sm.apply(OrderEvent::AcceptAck).has_value());
    EXPECT_TRUE(sm.apply(OrderEvent::Fill).has_value());

    EXPECT_EQ(sm.state(), OrderState::Filled);
    EXPECT_TRUE(sm.is_terminal());
    EXPECT_EQ(sm.transition_count(), 3u);
}

TEST(OrderStateMachineTest, InvalidTransitionDoesNotChangeState) {
    OrderStateMachine sm{1};
    auto r = sm.apply(OrderEvent::Fill); // Invalid from New.
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(sm.state(), OrderState::New);
}
