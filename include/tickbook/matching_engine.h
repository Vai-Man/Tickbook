// A price-time-priority matching engine on top of OrderBook.
#pragma once

#include <vector>

#include "tickbook/order_book.h"

namespace tickbook {

struct Trade {
    OrderRef taker = 0;
    OrderRef maker = 0;
    Price price = 0;  // always the resting (maker) order's price
    Qty qty = 0;
    Side taker_side = Side::Buy;
};

class MatchingEngine {
public:
    // Match against the opposite side while prices cross, then rest any remainder.
    // Returns no trades (and does nothing) for a zero quantity or a duplicate id.
    std::vector<Trade> submit_limit(OrderRef id, Side side, Price price, Qty qty);
    // Match at any price; whatever cannot be filled is discarded.
    std::vector<Trade> submit_market(OrderRef id, Side side, Qty qty);
    bool cancel(OrderRef id) { return book_.remove(id); }

    const OrderBook& book() const { return book_; }

private:
    std::vector<Trade> match(OrderRef id, Side side, Price limit, bool has_limit, Qty& remaining);
    OrderBook book_;
};

}  // namespace tickbook
