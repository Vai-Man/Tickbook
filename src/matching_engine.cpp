#include "tickbook/matching_engine.h"

namespace tickbook {

std::vector<Trade> MatchingEngine::match(OrderRef id, Side side, Price limit, bool has_limit, Qty& remaining) {
    std::vector<Trade> trades;
    const Side maker_side = opposite(side);
    while (remaining > 0) {
        auto maker = book_.front_order(maker_side);
        if (!maker) break;
        if (has_limit) {
            const bool crosses = side == Side::Buy ? maker->price <= limit : maker->price >= limit;
            if (!crosses) break;
        }
        const Qty fill = remaining < maker->qty ? remaining : maker->qty;
        trades.push_back(Trade{id, maker->ref, maker->price, fill, side});
        book_.execute(maker->ref, fill);
        remaining -= fill;
    }
    return trades;
}

std::vector<Trade> MatchingEngine::submit_limit(OrderRef id, Side side, Price price, Qty qty) {
    if (qty == 0 || book_.contains(id)) return {};
    Qty remaining = qty;
    std::vector<Trade> trades = match(id, side, price, true, remaining);
    if (remaining > 0) book_.add(id, side, price, remaining);
    return trades;
}

std::vector<Trade> MatchingEngine::submit_market(OrderRef id, Side side, Qty qty) {
    if (qty == 0 || book_.contains(id)) return {};
    Qty remaining = qty;
    return match(id, side, 0, false, remaining);
}

}  // namespace tickbook
