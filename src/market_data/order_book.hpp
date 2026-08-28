#pragma once
/// order_book.hpp — Limit order book with price-time priority.
///
/// Design for options: the number of distinct price levels per series is
/// typically small (20-50 levels). A production system would use a
/// pre-allocated tick-indexed array for O(1) level lookup since options
/// tick sizes are fixed ($0.01 or $0.05). This implementation uses
/// std::map for correctness-first clarity, with notes on optimization paths.

#include "core/fixed_price.hpp"
#include "core/safe_numeric.hpp"

#include <cstdint>
#include <deque>
#include <map>

// core::expected polyfill for AppleClang (no C++23 <expected> yet).
#include "core/expected.hpp"
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace options_ats_core::market_data {

using core::FixedPrice;

enum class Side : uint8_t { Buy, Sell };

struct Order {
    uint64_t order_id;
    Side side;
    FixedPrice price;
    int32_t quantity;     // Remaining quantity.
    int32_t filled_qty;   // Cumulative filled.
    uint64_t timestamp_tsc;

    [[nodiscard]] bool is_fully_filled() const noexcept {
        return quantity <= 0;
    }
};

struct Fill {
    uint64_t aggressor_order_id;
    uint64_t passive_order_id;
    FixedPrice price;
    int32_t quantity;
    uint64_t timestamp_tsc;
};

enum class BookError : uint8_t {
    DuplicateOrderId,
    OrderNotFound,
    InvalidQuantity,
    InvalidPrice,
    QuantityOverflow,
};

[[nodiscard]] constexpr std::string_view to_string(BookError e) noexcept {
    switch (e) {
        case BookError::DuplicateOrderId: return "DuplicateOrderId";
        case BookError::OrderNotFound:    return "OrderNotFound";
        case BookError::InvalidQuantity:  return "InvalidQuantity";
        case BookError::InvalidPrice:     return "InvalidPrice";
        case BookError::QuantityOverflow: return "QuantityOverflow";
    }
    return "Unknown";
}

/// A price level contains orders in FIFO (time priority) order.
struct PriceLevel {
    FixedPrice price;
    std::deque<Order*> orders;  // Front = oldest (highest time priority).

    [[nodiscard]] int32_t total_quantity() const noexcept {
        int64_t total = 0;
        for (const auto* o : orders) {
            total += o->quantity;
        }
        const auto narrowed = core::checked_narrow_cast<int32_t>(total);
        return narrowed.has_value() ? *narrowed : 0;
    }

    [[nodiscard]] bool empty() const noexcept { return orders.empty(); }
};

class OrderBook {
public:
    /// Add an order. If it crosses the spread, matching occurs first.
    [[nodiscard]] core::expected<std::vector<Fill>, BookError>
    add_order(Order order) {
        if (order.quantity <= 0) {
            return core::unexpected(BookError::InvalidQuantity);
        }
        if (order.price.is_zero()) {
            return core::unexpected(BookError::InvalidPrice);
        }
        if (order.filled_qty < 0) {
            return core::unexpected(BookError::InvalidQuantity);
        }
        if (order_index_.contains(order.order_id)) {
            return core::unexpected(BookError::DuplicateOrderId);
        }

        std::vector<Fill> fills;
        const auto& opposite = (order.side == Side::Buy) ? asks_ : bids_;
        fills.reserve(opposite.size());

        // Match against opposite side.
        auto& mutable_opposite = (order.side == Side::Buy) ? asks_ : bids_;
        const auto match_result = match(order, mutable_opposite, fills);
        if (!match_result.has_value()) {
            return core::unexpected(match_result.error());
        }

        // If residual quantity, rest on the book.
        if (order.quantity > 0) {
            auto* stored = &orders_.emplace_back(order);
            order_index_[stored->order_id] = stored;
            auto& same_side = (order.side == Side::Buy) ? bids_ : asks_;
            same_side[stored->price].price = stored->price;
            same_side[stored->price].orders.push_back(stored);
        }

        return fills;
    }

    /// Cancel an order by ID. Returns the cancelled order's remaining qty.
    [[nodiscard]] core::expected<int32_t, BookError>
    cancel_order(uint64_t order_id) {
        auto it = order_index_.find(order_id);
        if (it == order_index_.end()) {
            return core::unexpected(BookError::OrderNotFound);
        }

        Order* order = it->second;
        int32_t remaining = order->quantity;
        order->quantity = 0;

        // Remove from price level.
        auto& side_book = (order->side == Side::Buy) ? bids_ : asks_;
        auto level_it = side_book.find(order->price);
        if (level_it != side_book.end()) {
            auto& q = level_it->second.orders;
            std::erase(q, order);
            if (q.empty()) side_book.erase(level_it);
        }

        order_index_.erase(it);
        return remaining;
    }

    // --- Queries ---

    [[nodiscard]] std::optional<FixedPrice> best_bid() const noexcept {
        if (bids_.empty()) return std::nullopt;
        return bids_.rbegin()->first;  // Highest bid.
    }

    [[nodiscard]] std::optional<FixedPrice> best_ask() const noexcept {
        if (asks_.empty()) return std::nullopt;
        return asks_.begin()->first;   // Lowest ask.
    }

    [[nodiscard]] std::optional<FixedPrice> spread() const noexcept {
        auto bb = best_bid();
        auto ba = best_ask();
        if (!bb || !ba) return std::nullopt;
        return *ba - *bb;
    }

    [[nodiscard]] std::size_t bid_level_count() const noexcept { return bids_.size(); }
    [[nodiscard]] std::size_t ask_level_count() const noexcept { return asks_.size(); }

    [[nodiscard]] std::size_t order_count() const noexcept {
        return order_index_.size();
    }

    /// Get depth at each price level for a given side.
    [[nodiscard]] std::vector<std::pair<FixedPrice, int32_t>>
    depth(Side side, std::size_t max_levels = 10) const {
        std::vector<std::pair<FixedPrice, int32_t>> result;
        const std::size_t available_levels =
            (side == Side::Buy) ? bids_.size() : asks_.size();
        result.reserve(std::min(max_levels, available_levels));

        if (side == Side::Buy) {
            std::size_t count = 0;
            for (auto it = bids_.rbegin();
                 it != bids_.rend() && count < max_levels; ++it, ++count) {
                result.emplace_back(it->first, it->second.total_quantity());
            }
        } else {
            std::size_t count = 0;
            for (auto it = asks_.begin();
                 it != asks_.end() && count < max_levels; ++it, ++count) {
                result.emplace_back(it->first, it->second.total_quantity());
            }
        }
        return result;
    }

private:
    /// Match an incoming order against the opposite side book.
    [[nodiscard]] core::expected<bool, BookError>
    match(Order& aggressor,
          std::map<FixedPrice, PriceLevel>& opposite,
          std::vector<Fill>& fills) {
        // Buy aggressor matches asks (lowest first) where ask <= buy price.
        // Sell aggressor matches bids (highest first) where bid >= sell price.
        auto can_match = [&](FixedPrice passive_price) -> bool {
            if (aggressor.side == Side::Buy) {
                return passive_price <= aggressor.price;
            } else {
                return passive_price >= aggressor.price;
            }
        };

        while (aggressor.quantity > 0 && !opposite.empty()) {
            auto it = (aggressor.side == Side::Buy)
                          ? opposite.begin()       // Lowest ask
                          : std::prev(opposite.end()); // Highest bid

            if (!can_match(it->first)) {
                break;
            }

            auto& level = it->second;
            while (aggressor.quantity > 0 && !level.orders.empty()) {
                Order* passive = level.orders.front();
                int32_t fill_qty = std::min(aggressor.quantity, passive->quantity);

                fills.push_back(Fill{
                    .aggressor_order_id = aggressor.order_id,
                    .passive_order_id = passive->order_id,
                    .price = passive->price,  // Execute at passive price.
                    .quantity = fill_qty,
                    .timestamp_tsc = aggressor.timestamp_tsc,
                });

                const auto aggressor_remaining =
                    core::checked_sub(aggressor.quantity, fill_qty);
                const auto aggressor_filled =
                    core::checked_add(aggressor.filled_qty, fill_qty);
                const auto passive_remaining =
                    core::checked_sub(passive->quantity, fill_qty);
                const auto passive_filled =
                    core::checked_add(passive->filled_qty, fill_qty);
                if (!aggressor_remaining.has_value() || !aggressor_filled.has_value() ||
                    !passive_remaining.has_value() || !passive_filled.has_value()) {
                    return core::unexpected(BookError::QuantityOverflow);
                }

                aggressor.quantity = *aggressor_remaining;
                aggressor.filled_qty = *aggressor_filled;
                passive->quantity = *passive_remaining;
                passive->filled_qty = *passive_filled;

                if (passive->is_fully_filled()) {
                    order_index_.erase(passive->order_id);
                    level.orders.pop_front();
                }
            }

            if (level.orders.empty()) {
                opposite.erase(it);
            }
        }

        return true;
    }

    // Bids: sorted ascending by price. Best bid = rbegin (highest).
    std::map<FixedPrice, PriceLevel> bids_;
    // Asks: sorted ascending by price. Best ask = begin (lowest).
    std::map<FixedPrice, PriceLevel> asks_;

    // O(1) order lookup by ID for cancel operations.
    std::unordered_map<uint64_t, Order*> order_index_;

    // Stable storage for orders (deque doesn't invalidate pointers on push_back).
    std::deque<Order> orders_;
};

} // namespace options_ats_core::market_data
