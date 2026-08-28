#pragma once
/// order_state_machine.hpp — Exhaustive order lifecycle state machine.
///
/// Every (State, Event) pair is handled explicitly. Invalid transitions
/// return a typed error — never silently ignored. The design maps the full
/// state space, rejects invalid transitions, and supports telemetry on every
/// transition.

#include <cstdint>
#include <string_view>

// core::expected polyfill for AppleClang (no C++23 <expected> yet).
#include "core/expected.hpp"

namespace options_ats_core::oms {

enum class OrderState : uint8_t {
    New,
    PendingAccept,
    Accepted,
    PartiallyFilled,
    Filled,
    PendingCancel,
    Cancelled,
    Rejected,
};

enum class OrderEvent : uint8_t {
    Submit,         // Client submits order.
    AcceptAck,      // Venue acknowledges acceptance.
    RejectAck,      // Venue rejects.
    PartialFill,    // Partial execution.
    Fill,           // Full execution (remaining qty filled).
    CancelRequest,  // Client requests cancel.
    CancelAck,      // Venue confirms cancel.
    CancelReject,   // Venue rejects cancel (e.g., already filled).
};

enum class TransitionError : uint8_t {
    InvalidTransition,
    AlreadyTerminal,
};

[[nodiscard]] constexpr std::string_view to_string(OrderState s) noexcept {
    switch (s) {
        case OrderState::New:             return "New";
        case OrderState::PendingAccept:   return "PendingAccept";
        case OrderState::Accepted:        return "Accepted";
        case OrderState::PartiallyFilled: return "PartiallyFilled";
        case OrderState::Filled:          return "Filled";
        case OrderState::PendingCancel:   return "PendingCancel";
        case OrderState::Cancelled:       return "Cancelled";
        case OrderState::Rejected:        return "Rejected";
    }
    return "Unknown";
}

[[nodiscard]] constexpr std::string_view to_string(OrderEvent e) noexcept {
    switch (e) {
        case OrderEvent::Submit:        return "Submit";
        case OrderEvent::AcceptAck:     return "AcceptAck";
        case OrderEvent::RejectAck:     return "RejectAck";
        case OrderEvent::PartialFill:   return "PartialFill";
        case OrderEvent::Fill:          return "Fill";
        case OrderEvent::CancelRequest: return "CancelRequest";
        case OrderEvent::CancelAck:     return "CancelAck";
        case OrderEvent::CancelReject:  return "CancelReject";
    }
    return "Unknown";
}

[[nodiscard]] constexpr std::string_view to_string(TransitionError e) noexcept {
    switch (e) {
        case TransitionError::InvalidTransition: return "InvalidTransition";
        case TransitionError::AlreadyTerminal:   return "AlreadyTerminal";
    }
    return "Unknown";
}

[[nodiscard]] constexpr bool is_terminal(OrderState s) noexcept {
    return s == OrderState::Filled ||
           s == OrderState::Cancelled ||
           s == OrderState::Rejected;
}

/// Pure function: given (state, event), return the next state or an error.
/// Every pair is handled — no default case that silently swallows transitions.
[[nodiscard]] constexpr core::expected<OrderState, TransitionError>
transition(OrderState state, OrderEvent event) noexcept {
    if (is_terminal(state)) {
        return core::unexpected(TransitionError::AlreadyTerminal);
    }

    switch (state) {
    case OrderState::New:
        switch (event) {
            case OrderEvent::Submit:    return OrderState::PendingAccept;
            default:                    return core::unexpected(TransitionError::InvalidTransition);
        }

    case OrderState::PendingAccept:
        switch (event) {
            case OrderEvent::AcceptAck:   return OrderState::Accepted;
            case OrderEvent::RejectAck:   return OrderState::Rejected;
            case OrderEvent::PartialFill: return OrderState::PartiallyFilled;
            case OrderEvent::Fill:        return OrderState::Filled;
            default:                      return core::unexpected(TransitionError::InvalidTransition);
        }

    case OrderState::Accepted:
        switch (event) {
            case OrderEvent::PartialFill:   return OrderState::PartiallyFilled;
            case OrderEvent::Fill:          return OrderState::Filled;
            case OrderEvent::CancelRequest: return OrderState::PendingCancel;
            default:                        return core::unexpected(TransitionError::InvalidTransition);
        }

    case OrderState::PartiallyFilled:
        switch (event) {
            case OrderEvent::PartialFill:   return OrderState::PartiallyFilled;
            case OrderEvent::Fill:          return OrderState::Filled;
            case OrderEvent::CancelRequest: return OrderState::PendingCancel;
            default:                        return core::unexpected(TransitionError::InvalidTransition);
        }

    case OrderState::PendingCancel:
        switch (event) {
            case OrderEvent::CancelAck:    return OrderState::Cancelled;
            case OrderEvent::CancelReject: return OrderState::Accepted;
            case OrderEvent::PartialFill:  return OrderState::PendingCancel;
            case OrderEvent::Fill:         return OrderState::Filled;
            default:                       return core::unexpected(TransitionError::InvalidTransition);
        }

    // Terminal states handled by the is_terminal guard above.
    case OrderState::Filled:
    case OrderState::Cancelled:
    case OrderState::Rejected:
        return core::unexpected(TransitionError::AlreadyTerminal);
    }

    return core::unexpected(TransitionError::InvalidTransition);
}

/// Tracks order state with transition history for telemetry.
class OrderStateMachine {
public:
    explicit constexpr OrderStateMachine(uint64_t order_id) noexcept
        : order_id_{order_id}, state_{OrderState::New}, transition_count_{0} {}

    [[nodiscard]] core::expected<OrderState, TransitionError>
    apply(OrderEvent event) noexcept {
        auto result = transition(state_, event);
        if (result.has_value()) {
            state_ = *result;
            ++transition_count_;
        }
        return result;
    }

    [[nodiscard]] constexpr OrderState state() const noexcept { return state_; }
    [[nodiscard]] constexpr uint64_t order_id() const noexcept { return order_id_; }
    [[nodiscard]] constexpr uint32_t transition_count() const noexcept {
        return transition_count_;
    }
    [[nodiscard]] constexpr bool is_terminal() const noexcept {
        return oms::is_terminal(state_);
    }

private:
    uint64_t order_id_;
    OrderState state_;
    uint32_t transition_count_;
};

} // namespace options_ats_core::oms
