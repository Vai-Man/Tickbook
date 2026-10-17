// A limit order book for one instrument with price-time priority.
//
// Orders live in a pooled array and are linked into a FIFO list per price level, so
// adding, cancelling and executing an order are O(log L) in the number of price levels
// (the level lookup) and O(1) in everything else. The reference-to-slot lookup is a hash map.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "tickbook/itch.h"
#include "tickbook/types.h"

namespace tickbook {

struct LevelView {
    Price price = 0;
    Qty qty = 0;
    std::uint32_t orders = 0;
};

struct OrderInfo {
    OrderRef ref = 0;
    Side side = Side::Buy;
    Price price = 0;
    Qty qty = 0;
};

class OrderBook {
public:
    OrderBook() = default;
    explicit OrderBook(std::size_t expected_orders);

    // All mutators return false (and change nothing) if the request is invalid.
    bool add(OrderRef ref, Side side, Price price, Qty qty);
    bool cancel(OrderRef ref, Qty qty);   // reduce an order by qty; removes it at zero
    bool execute(OrderRef ref, Qty qty);  // same effect on the book as cancel
    bool remove(OrderRef ref);            // delete the whole order
    bool replace(OrderRef old_ref, OrderRef new_ref, Qty qty, Price price);  // loses time priority

    bool contains(OrderRef ref) const { return index_.count(ref) != 0; }
    std::optional<OrderInfo> find(OrderRef ref) const;

    std::optional<Price> best_bid() const;
    std::optional<Price> best_ask() const;
    // Midpoint in price units; nullopt unless both sides are populated.
    std::optional<double> mid() const;
    std::optional<Price> spread() const;

    Qty qty_at(Side side, Price price) const;
    // Oldest resting order at the best price on a side.
    std::optional<OrderInfo> front_order(Side side) const;
    // Up to n levels, best first.
    std::vector<LevelView> depth(Side side, std::size_t n) const;

    std::size_t order_count() const { return index_.size(); }
    std::size_t level_count(Side side) const { return levels_[side == Side::Buy ? 0 : 1].size(); }
    bool crossed() const;

private:
    static constexpr std::uint32_t kNil = 0xFFFFFFFFu;

    struct Order {
        OrderRef ref = 0;
        Price price = 0;
        Qty qty = 0;
        Side side = Side::Buy;
        std::uint32_t prev = kNil, next = kNil;
    };
    struct Level {
        Qty qty = 0;
        std::uint32_t count = 0;
        std::uint32_t head = kNil, tail = kNil;
    };
    using LevelMap = std::map<Price, Level>;

    LevelMap& side_map(Side s) { return levels_[s == Side::Buy ? 0 : 1]; }
    const LevelMap& side_map(Side s) const { return levels_[s == Side::Buy ? 0 : 1]; }
    void unlink(std::uint32_t slot, LevelMap::iterator level_it);
    bool reduce(OrderRef ref, Qty qty);

    std::vector<Order> pool_;
    std::vector<std::uint32_t> free_;
    std::unordered_map<OrderRef, std::uint32_t> index_;
    LevelMap levels_[2];  // [0] bids (best = largest key), [1] asks (best = smallest key)
};

// Apply an ITCH event to a book. Returns true if the book changed as the event describes.
// Events that are not about order flow (system events, directory) return true and do nothing.
bool apply(OrderBook& book, const Event& ev);

}  // namespace tickbook
