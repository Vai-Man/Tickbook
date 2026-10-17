#include "tickbook/fill_model.h"

#include <algorithm>

namespace tickbook {

bool FillModel::place(std::uint64_t id, Side side, Price price, Qty qty, Nanos now, const OrderBook& book) {
    if (qty == 0 || find(id)) return false;
    if (side == Side::Buy) {
        auto ask = book.best_ask();
        if (ask && price >= *ask) return false;
    } else {
        auto bid = book.best_bid();
        if (bid && price <= *bid) return false;
    }
    SimOrder o;
    o.id = id;
    o.side = side;
    o.price = price;
    o.remaining = qty;
    o.queue_ahead = static_cast<double>(book.qty_at(side, price));
    o.placed_at = now;
    orders_.push_back(o);
    return true;
}

bool FillModel::cancel(std::uint64_t id) {
    auto it = std::find_if(orders_.begin(), orders_.end(), [&](const SimOrder& o) { return o.id == id; });
    if (it == orders_.end()) return false;
    orders_.erase(it);
    return true;
}

const SimOrder* FillModel::find(std::uint64_t id) const {
    for (const auto& o : orders_)
        if (o.id == id) return &o;
    return nullptr;
}

void FillModel::on_event(const Event& ev, const OrderBook& book) {
    if (orders_.empty()) return;
    switch (ev.type) {
        case EventType::Execute:
        case EventType::ExecuteWithPrice: on_execution(ev, book); break;
        case EventType::Cancel:
        case EventType::Delete:
        case EventType::Replace: on_removal(ev, book); break;
        default: break;
    }
}

void FillModel::on_execution(const Event& ev, const OrderBook& book) {
    auto info = book.find(ev.ref);
    if (!info) return;
    const Qty executed = std::min(ev.shares, info->qty);
    const double mid = book.mid().value_or(static_cast<double>(info->price));
    for (auto it = orders_.begin(); it != orders_.end();) {
        if (it->side != info->side || it->price != info->price) {
            ++it;
            continue;
        }
        if (it->queue_ahead >= executed) {
            it->queue_ahead -= executed;
            ++it;
            continue;
        }
        const double overflow = executed - it->queue_ahead;
        it->queue_ahead = 0.0;
        const Qty filled = static_cast<Qty>(std::min<double>(it->remaining, overflow));
        if (filled > 0) {
            fills_.push_back(Fill{ev.ts, it->id, it->side, it->price, filled, mid});
            it->remaining -= filled;
        }
        it = it->remaining == 0 ? orders_.erase(it) : it + 1;
    }
}

void FillModel::on_removal(const Event& ev, const OrderBook& book) {
    auto info = book.find(ev.ref);
    if (!info) return;
    const Qty removed = ev.type == EventType::Cancel ? std::min(ev.shares, info->qty) : info->qty;
    const Qty level = book.qty_at(info->side, info->price);
    if (level == 0) return;
    const double keep = static_cast<double>(level - std::min(removed, level)) / static_cast<double>(level);
    for (auto& o : orders_)
        if (o.side == info->side && o.price == info->price) o.queue_ahead *= keep;
}

}  // namespace tickbook
