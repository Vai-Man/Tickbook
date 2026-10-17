#include "tickbook/order_book.h"

#include <algorithm>

namespace tickbook {

OrderBook::OrderBook(std::size_t expected_orders) {
    pool_.reserve(expected_orders);
    index_.reserve(expected_orders);
}

bool OrderBook::add(OrderRef ref, Side side, Price price, Qty qty) {
    if (qty == 0 || index_.count(ref)) return false;
    std::uint32_t slot;
    if (!free_.empty()) {
        slot = free_.back();
        free_.pop_back();
    } else {
        slot = static_cast<std::uint32_t>(pool_.size());
        pool_.emplace_back();
    }
    Order& o = pool_[slot];
    o.ref = ref;
    o.price = price;
    o.qty = qty;
    o.side = side;
    o.prev = o.next = kNil;

    Level& lvl = side_map(side)[price];
    if (lvl.tail == kNil) {
        lvl.head = lvl.tail = slot;
    } else {
        pool_[lvl.tail].next = slot;
        o.prev = lvl.tail;
        lvl.tail = slot;
    }
    lvl.qty += qty;
    ++lvl.count;
    index_.emplace(ref, slot);
    return true;
}

void OrderBook::unlink(std::uint32_t slot, LevelMap::iterator level_it) {
    Order& o = pool_[slot];
    Level& lvl = level_it->second;
    if (o.prev != kNil) pool_[o.prev].next = o.next; else lvl.head = o.next;
    if (o.next != kNil) pool_[o.next].prev = o.prev; else lvl.tail = o.prev;
    lvl.qty -= o.qty;
    --lvl.count;
    if (lvl.count == 0) side_map(o.side).erase(level_it);
}

bool OrderBook::remove(OrderRef ref) {
    auto it = index_.find(ref);
    if (it == index_.end()) return false;
    const std::uint32_t slot = it->second;
    Order& o = pool_[slot];
    unlink(slot, side_map(o.side).find(o.price));
    index_.erase(it);
    free_.push_back(slot);
    return true;
}

bool OrderBook::reduce(OrderRef ref, Qty qty) {
    auto it = index_.find(ref);
    if (it == index_.end() || qty == 0) return false;
    const std::uint32_t slot = it->second;
    Order& o = pool_[slot];
    if (qty >= o.qty) return remove(ref);
    auto level_it = side_map(o.side).find(o.price);
    level_it->second.qty -= qty;
    o.qty -= qty;
    return true;
}

bool OrderBook::cancel(OrderRef ref, Qty qty) { return reduce(ref, qty); }
bool OrderBook::execute(OrderRef ref, Qty qty) { return reduce(ref, qty); }

bool OrderBook::replace(OrderRef old_ref, OrderRef new_ref, Qty qty, Price price) {
    auto it = index_.find(old_ref);
    if (it == index_.end() || qty == 0) return false;
    if (new_ref != old_ref && index_.count(new_ref)) return false;
    const Side side = pool_[it->second].side;
    remove(old_ref);
    return add(new_ref, side, price, qty);
}

std::optional<OrderInfo> OrderBook::find(OrderRef ref) const {
    auto it = index_.find(ref);
    if (it == index_.end()) return std::nullopt;
    const Order& o = pool_[it->second];
    return OrderInfo{o.ref, o.side, o.price, o.qty};
}

std::optional<Price> OrderBook::best_bid() const {
    const auto& m = levels_[0];
    if (m.empty()) return std::nullopt;
    return m.rbegin()->first;
}

std::optional<Price> OrderBook::best_ask() const {
    const auto& m = levels_[1];
    if (m.empty()) return std::nullopt;
    return m.begin()->first;
}

std::optional<double> OrderBook::mid() const {
    auto b = best_bid(), a = best_ask();
    if (!b || !a) return std::nullopt;
    return (static_cast<double>(*b) + static_cast<double>(*a)) / 2.0;
}

std::optional<Price> OrderBook::spread() const {
    auto b = best_bid(), a = best_ask();
    if (!b || !a) return std::nullopt;
    return *a - *b;
}

bool OrderBook::crossed() const {
    auto b = best_bid(), a = best_ask();
    return b && a && *b >= *a;
}

Qty OrderBook::qty_at(Side side, Price price) const {
    const auto& m = side_map(side);
    auto it = m.find(price);
    return it == m.end() ? 0 : it->second.qty;
}

std::optional<OrderInfo> OrderBook::front_order(Side side) const {
    const auto& m = side_map(side);
    if (m.empty()) return std::nullopt;
    const Level& lvl = side == Side::Buy ? m.rbegin()->second : m.begin()->second;
    const Order& o = pool_[lvl.head];
    return OrderInfo{o.ref, o.side, o.price, o.qty};
}

std::vector<LevelView> OrderBook::depth(Side side, std::size_t n) const {
    std::vector<LevelView> out;
    const auto& m = side_map(side);
    if (side == Side::Buy) {
        for (auto it = m.rbegin(); it != m.rend() && out.size() < n; ++it)
            out.push_back({it->first, it->second.qty, it->second.count});
    } else {
        for (auto it = m.begin(); it != m.end() && out.size() < n; ++it)
            out.push_back({it->first, it->second.qty, it->second.count});
    }
    return out;
}

bool apply(OrderBook& book, const Event& ev) {
    switch (ev.type) {
        case EventType::Add: return book.add(ev.ref, ev.side, ev.price, ev.shares);
        case EventType::Execute:
        case EventType::ExecuteWithPrice: return book.execute(ev.ref, ev.shares);
        case EventType::Cancel: return book.cancel(ev.ref, ev.shares);
        case EventType::Delete: return book.remove(ev.ref);
        case EventType::Replace: return book.replace(ev.ref, ev.new_ref, ev.shares, ev.price);
        default: return true;
    }
}

}  // namespace tickbook
