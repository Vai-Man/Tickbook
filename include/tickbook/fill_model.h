// Queue-position fill model for simulated passive orders.
//
// A simulated limit order joins the back of the visible queue at its price. It can only be
// filled once enough volume has executed at that price to clear the orders in front of it.
// Cancellations ahead of it are not observable in the feed, so they are assumed to be spread
// evenly over the queue: a cancel of x shares out of a level holding L shares moves the order
// forward by the fraction x / L of the queue ahead of it.
//
// on_event() must be called BEFORE the event is applied to the book, because it needs the
// resting order's price and side, which are gone afterwards.
#pragma once

#include <cstdint>
#include <vector>

#include "tickbook/itch.h"
#include "tickbook/order_book.h"

namespace tickbook {

struct SimOrder {
    std::uint64_t id = 0;
    Side side = Side::Buy;
    Price price = 0;
    Qty remaining = 0;
    double queue_ahead = 0.0;
    Nanos placed_at = 0;
};

struct Fill {
    Nanos ts = 0;
    std::uint64_t order_id = 0;
    Side side = Side::Buy;
    Price price = 0;
    Qty qty = 0;
    double mid_at_fill = 0.0;  // book midpoint (price units) when the fill happened
};

class FillModel {
public:
    // Rest an order at the back of the queue. Returns false if it would cross the book
    // (the simulator only places passive, post-only orders).
    bool place(std::uint64_t id, Side side, Price price, Qty qty, Nanos now, const OrderBook& book);
    bool cancel(std::uint64_t id);
    void on_event(const Event& ev, const OrderBook& book);

    const std::vector<Fill>& fills() const { return fills_; }
    const std::vector<SimOrder>& orders() const { return orders_; }
    const SimOrder* find(std::uint64_t id) const;

private:
    void on_execution(const Event& ev, const OrderBook& book);
    void on_removal(const Event& ev, const OrderBook& book);

    std::vector<SimOrder> orders_;
    std::vector<Fill> fills_;
};

}  // namespace tickbook
