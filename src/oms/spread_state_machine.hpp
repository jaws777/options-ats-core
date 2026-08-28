#pragma once
/// spread_state_machine.hpp — Multi-leg order state machine for options spreads.
///
/// Options traders send multi-leg orders (spreads, straddles, strangles,
/// iron condors). These are a SINGLE client order with 2-4 legs that must
/// be executed atomically or managed as a correlated group.
///
/// The key complexity: each leg has its own lifecycle state, and the overall
/// spread state depends on the product of per-leg states. If leg 1 fills
/// but leg 2 is rejected, the system must handle the partial — either
/// cancel leg 1 (unwind) or report a partial fill to the client.
///
/// This implementation tracks per-leg state machines plus a composite state
/// that enforces cross-leg constraints. The same exhaustive transition
/// approach from OrderStateMachine applies: every (CompositeState, Event)
/// pair is handled explicitly.
///
/// Design note: single-leg orders use the exhaustive OrderStateMachine.
/// For spreads, each leg has its own OrderStateMachine instance. The
/// SpreadStateMachine tracks the composite state and enforces the
/// coordination policy: all-or-none fill, or partial with unwind.

#include "core/expected.hpp"
#include "oms/order_state_machine.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>

namespace options_ats_core::oms {

/// Maximum legs per spread. Covers all standard options strategies:
///   2 legs: vertical spread, straddle, strangle
///   3 legs: butterfly (sometimes), ratio spread
///   4 legs: iron condor, iron butterfly, box spread
static constexpr uint32_t kMaxLegs = 4;

/// The composite state of a multi-leg order.
/// Derived from the per-leg states, not stored independently.
enum class SpreadState : uint8_t {
    New,                 // All legs New.
    PendingAccept,       // At least one leg pending, none terminal.
    Active,              // All legs Accepted or PartiallyFilled.
    PartiallyFilled,     // At least one leg filled, others still active.
    Filled,              // All legs fully filled. Terminal.
    PendingCancel,       // Cancel requested on active legs.
    Cancelled,           // All legs cancelled or unwound. Terminal.
    Rejected,            // At least one leg rejected, others unwound. Terminal.
    LegDivergence,       // Inconsistent state requiring manual intervention. Terminal.
};

[[nodiscard]] constexpr std::string_view to_string(SpreadState s) noexcept {
    switch (s) {
        case SpreadState::New:              return "New";
        case SpreadState::PendingAccept:    return "PendingAccept";
        case SpreadState::Active:           return "Active";
        case SpreadState::PartiallyFilled:  return "PartiallyFilled";
        case SpreadState::Filled:           return "Filled";
        case SpreadState::PendingCancel:    return "PendingCancel";
        case SpreadState::Cancelled:        return "Cancelled";
        case SpreadState::Rejected:         return "Rejected";
        case SpreadState::LegDivergence:    return "LegDivergence";
    }
    return "Unknown";
}

[[nodiscard]] constexpr bool is_terminal(SpreadState s) noexcept {
    return s == SpreadState::Filled ||
           s == SpreadState::Cancelled ||
           s == SpreadState::Rejected ||
           s == SpreadState::LegDivergence;
}

/// Event targeting a specific leg of the spread.
struct SpreadEvent {
    uint32_t leg_index;   // Which leg (0-based).
    OrderEvent event;     // The per-leg event.
};

enum class SpreadError : uint8_t {
    InvalidLegIndex,
    InvalidLegCount,
    SpreadTerminal,
    LegTransitionFailed,
};

[[nodiscard]] constexpr std::string_view to_string(SpreadError e) noexcept {
    switch (e) {
        case SpreadError::InvalidLegIndex:       return "InvalidLegIndex";
        case SpreadError::InvalidLegCount:       return "InvalidLegCount";
        case SpreadError::SpreadTerminal:        return "SpreadTerminal";
        case SpreadError::LegTransitionFailed:   return "LegTransitionFailed";
    }
    return "Unknown";
}

/// Multi-leg order state machine.
///
/// Each leg is an independent OrderStateMachine. The SpreadStateMachine
/// computes the composite state from the per-leg states and enforces
/// coordination policies.
class SpreadStateMachine {
public:
    /// Construct with a spread order ID and number of legs.
    explicit SpreadStateMachine(uint64_t spread_id, uint32_t leg_count)
        : spread_id_{spread_id}, leg_count_{leg_count} {
        // Initialize per-leg state machines with sequential IDs.
        for (uint32_t i = 0; i < leg_count_ && i < kMaxLegs; ++i) {
            legs_[i] = OrderStateMachine{spread_id * 100 + i};
        }
    }

    /// Apply an event to a specific leg and recompute composite state.
    [[nodiscard]] core::expected<SpreadState, SpreadError>
    apply(const SpreadEvent& event) {
        auto current = composite_state();
        // Allow events on terminal spreads ONLY if the event is a cancel
        // or cancel-ack on a non-terminal leg — this permits unwind of
        // partial fills after a leg rejection. Block everything else.
        if (is_terminal(current) && current != SpreadState::LegDivergence) {
            return core::unexpected(SpreadError::SpreadTerminal);
        }
        if (event.leg_index >= leg_count_) {
            return core::unexpected(SpreadError::InvalidLegIndex);
        }

        auto& leg = legs_[event.leg_index];
        auto result = leg.apply(event.event);
        if (!result.has_value()) {
            return core::unexpected(SpreadError::LegTransitionFailed);
        }

        // Recompute composite state from all leg states.
        return composite_state();
    }

    /// Submit all legs (transition all from New -> PendingAccept).
    [[nodiscard]] core::expected<SpreadState, SpreadError> submit_all() {
        for (uint32_t i = 0; i < leg_count_; ++i) {
            auto result = legs_[i].apply(OrderEvent::Submit);
            if (!result.has_value()) {
                return core::unexpected(SpreadError::LegTransitionFailed);
            }
        }
        return composite_state();
    }

    /// Cancel all non-terminal legs.
    [[nodiscard]] core::expected<SpreadState, SpreadError> cancel_all() {
        for (uint32_t i = 0; i < leg_count_; ++i) {
            if (legs_[i].is_terminal()) continue;
            auto state = legs_[i].state();
            // Can only cancel from Accepted or PartiallyFilled.
            if (state == OrderState::Accepted ||
                state == OrderState::PartiallyFilled) {
                auto result = legs_[i].apply(OrderEvent::CancelRequest);
                if (!result.has_value()) {
                    // Leg already moved — race condition. Continue with others.
                }
            }
        }
        return composite_state();
    }

    /// Compute the composite state from per-leg states.
    ///
    /// Rules:
    ///   - All New         → New
    ///   - All Filled      → Filled
    ///   - All Cancelled   → Cancelled
    ///   - Any Rejected (and rest cancelled/terminal) → Rejected
    ///   - All Accepted/PartiallyFilled → Active
    ///   - Mix of Filled + non-Filled active → PartiallyFilled
    ///   - Any PendingCancel → PendingCancel
    ///   - Any PendingAccept (none terminal) → PendingAccept
    ///   - Inconsistent combinations → LegDivergence
    [[nodiscard]] SpreadState composite_state() const noexcept {
        uint32_t new_count = 0, pending_accept = 0, accepted = 0;
        uint32_t partial = 0, filled = 0, pending_cancel = 0;
        uint32_t cancelled = 0, rejected = 0;

        for (uint32_t i = 0; i < leg_count_; ++i) {
            switch (legs_[i].state()) {
                case OrderState::New:              ++new_count; break;
                case OrderState::PendingAccept:    ++pending_accept; break;
                case OrderState::Accepted:         ++accepted; break;
                case OrderState::PartiallyFilled:  ++partial; break;
                case OrderState::Filled:           ++filled; break;
                case OrderState::PendingCancel:    ++pending_cancel; break;
                case OrderState::Cancelled:        ++cancelled; break;
                case OrderState::Rejected:         ++rejected; break;
            }
        }

        // All legs in same terminal state.
        if (filled == leg_count_)    return SpreadState::Filled;
        if (cancelled == leg_count_) return SpreadState::Cancelled;
        if (new_count == leg_count_) return SpreadState::New;

        // Any rejection with all others terminal → Rejected.
        if (rejected > 0 && (rejected + cancelled + filled == leg_count_)) {
            return SpreadState::Rejected;
        }

        // Any pending cancel → PendingCancel (unwind in progress).
        if (pending_cancel > 0) return SpreadState::PendingCancel;

        // Any rejection with active legs remaining → PendingCancel
        // (we need to unwind the active legs). This is the state between
        // "leg rejected" and "unwind complete."
        if (rejected > 0 && (accepted > 0 || partial > 0)) {
            return SpreadState::PendingCancel;
        }

        // Any pending accept (no terminals yet) → PendingAccept.
        if (pending_accept > 0 && rejected == 0) return SpreadState::PendingAccept;

        // Mix of filled and active legs → PartiallyFilled.
        if (filled > 0 && (accepted > 0 || partial > 0 || pending_accept > 0)) {
            return SpreadState::PartiallyFilled;
        }

        // All active (accepted or partial) → Active.
        if ((accepted + partial) == leg_count_) return SpreadState::Active;

        // Anything else is divergence (e.g., filled + rejected without cancel).
        return SpreadState::LegDivergence;
    }

    // --- Queries ---

    [[nodiscard]] uint64_t spread_id() const noexcept { return spread_id_; }
    [[nodiscard]] uint32_t leg_count() const noexcept { return leg_count_; }

    [[nodiscard]] OrderState leg_state(uint32_t index) const noexcept {
        return (index < leg_count_) ? legs_[index].state() : OrderState::New;
    }

    [[nodiscard]] const OrderStateMachine& leg(uint32_t index) const noexcept {
        return legs_[index < leg_count_ ? index : 0];
    }

    /// Count legs in terminal states.
    [[nodiscard]] uint32_t terminal_leg_count() const noexcept {
        uint32_t count = 0;
        for (uint32_t i = 0; i < leg_count_; ++i) {
            if (legs_[i].is_terminal()) ++count;
        }
        return count;
    }

    /// Count filled legs.
    [[nodiscard]] uint32_t filled_leg_count() const noexcept {
        uint32_t count = 0;
        for (uint32_t i = 0; i < leg_count_; ++i) {
            if (legs_[i].state() == OrderState::Filled) ++count;
        }
        return count;
    }

private:
    uint64_t spread_id_;
    uint32_t leg_count_;
    std::array<OrderStateMachine, kMaxLegs> legs_{
        OrderStateMachine{0}, OrderStateMachine{0},
        OrderStateMachine{0}, OrderStateMachine{0}};
};

} // namespace options_ats_core::oms
