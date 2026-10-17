#include "tickbook/synth.h"

#include <algorithm>
#include <random>
#include <unordered_map>

#include "tickbook/itch.h"
#include "tickbook/order_book.h"

namespace tickbook {

std::vector<std::uint8_t> generate_feed(const SynthConfig& cfg) {
    std::mt19937_64 rng(cfg.seed);
    auto uniform = [&](std::uint64_t n) { return static_cast<std::uint64_t>(rng() % n); };
    auto unit = [&] { return std::uniform_real_distribution<double>(0.0, 1.0)(rng); };
    std::exponential_distribution<double> gap(1.0 / cfg.mean_gap_ns);

    OrderBook book(1 << 16);
    std::vector<OrderRef> live;
    std::unordered_map<OrderRef, std::size_t> where;
    auto track = [&](OrderRef r) { where[r] = live.size(); live.push_back(r); };
    auto untrack = [&](OrderRef r) {
        const std::size_t i = where[r];
        const OrderRef last = live.back();
        live[i] = last;
        where[last] = i;
        live.pop_back();
        where.erase(r);
    };

    std::vector<std::uint8_t> out;
    out.reserve(static_cast<std::size_t>(cfg.events) * 34 + 128);
    Nanos ts = cfg.start_ts;
    encode_system_event(out, ts, 'O');
    encode_stock_directory(out, cfg.locate, ts, cfg.symbol.c_str());

    OrderRef next_ref = 1;
    std::uint64_t match = 1;
    Price mid = cfg.start_mid;

    // Each helper returns true if it wrote a message.
    auto add_order = [&](Side side) {
        const int offset = static_cast<int>(uniform(4)) + (unit() < 0.3 ? static_cast<int>(uniform(6)) : 0);
        Price p = side == Side::Buy ? mid - (1 + offset) * kTick : mid + (1 + offset) * kTick;
        if (side == Side::Buy) {
            if (auto ask = book.best_ask()) p = std::min<Price>(p, *ask - kTick);
        } else {
            if (auto bid = book.best_bid()) p = std::max<Price>(p, *bid + kTick);
        }
        if (p <= 0) return false;
        const Qty qty = static_cast<Qty>(100 * (1 + uniform(10)));
        const OrderRef ref = next_ref++;
        if (!book.add(ref, side, p, qty)) return false;
        track(ref);
        encode_add(out, cfg.locate, ts, ref, side, qty, cfg.symbol.c_str(), p);
        return true;
    };
    auto delete_random = [&] {
        if (live.empty()) return false;
        const OrderRef ref = live[uniform(live.size())];
        book.remove(ref);
        untrack(ref);
        encode_delete(out, cfg.locate, ts, ref);
        return true;
    };
    auto cancel_random = [&] {
        if (live.empty()) return false;
        const OrderRef ref = live[uniform(live.size())];
        const auto info = book.find(ref);
        if (!info || info->qty <= 100) return delete_random();
        book.cancel(ref, 100);
        encode_cancel(out, cfg.locate, ts, ref, 100);
        return true;
    };
    auto execute_best = [&] {
        const Side side = uniform(2) == 0 ? Side::Buy : Side::Sell;
        const auto front = book.front_order(side);
        if (!front) return false;
        const Qty qty = std::min<Qty>(front->qty, static_cast<Qty>(100 * (1 + uniform(3))));
        book.execute(front->ref, qty);
        if (!book.contains(front->ref)) untrack(front->ref);
        encode_execute(out, cfg.locate, ts, front->ref, qty, match++);
        return true;
    };
    auto replace_random = [&] {
        if (live.empty()) return false;
        const OrderRef old_ref = live[uniform(live.size())];
        const auto info = book.find(old_ref);
        if (!info) return false;
        Price p = info->price + (static_cast<int>(uniform(3)) - 1) * kTick;
        if (info->side == Side::Buy) {
            if (auto ask = book.best_ask()) p = std::min<Price>(p, *ask - kTick);
        } else {
            if (auto bid = book.best_bid()) p = std::max<Price>(p, *bid + kTick);
        }
        if (p <= 0) return false;
        const Qty qty = static_cast<Qty>(100 * (1 + uniform(10)));
        const OrderRef new_ref = next_ref++;
        if (!book.replace(old_ref, new_ref, qty, p)) return false;
        untrack(old_ref);
        track(new_ref);
        encode_replace(out, cfg.locate, ts, old_ref, new_ref, qty, p);
        return true;
    };

    std::uint64_t written = 0;
    std::uint64_t step = 0;
    while (written < cfg.events) {
        ++step;
        ts += static_cast<Nanos>(gap(rng)) + 1;
        if (step % 256 == 0) mid = std::max<Price>(10 * kTick, mid + (static_cast<int>(uniform(3)) - 1) * kTick);

        bool ok;
        if (live.size() < 20 || !book.best_bid() || !book.best_ask()) {
            ok = add_order(uniform(2) == 0 ? Side::Buy : Side::Sell);
        } else {
            const double u = unit();
            if (u < 0.40) ok = add_order(uniform(2) == 0 ? Side::Buy : Side::Sell);
            else if (u < 0.65) ok = delete_random();
            else if (u < 0.70) ok = cancel_random();
            else if (u < 0.88) ok = execute_best();
            else ok = replace_random();
        }
        if (ok) ++written;
    }
    return out;
}

}  // namespace tickbook
