#include "tickbook/backtest.h"

#include <algorithm>
#include <cmath>

namespace tickbook {
namespace {

Price floor_tick(double price_units) { return static_cast<Price>(std::floor(price_units / kTick)) * kTick; }
Price ceil_tick(double price_units) { return static_cast<Price>(std::ceil(price_units / kTick)) * kTick; }

}  // namespace

Backtester::Backtester(MMConfig cfg) : cfg_(cfg) {}

void Backtester::on_event(const Event& ev, OrderBook& book) {
    ++events_;
    run_actions(ev.ts, book);        // orders whose latency has elapsed reach the book first
    fills_.on_event(ev, book);       // model fills against the pre-event book
    book_new_fills();
    apply(book, ev);

    const auto mid = book.mid();
    if (!mid) return;
    if (have_mid_ && ev.ts > last_mid_ts_ && *mid != last_mid_) {
        const double dt = std::max(1e-3, static_cast<double>(ev.ts - last_mid_ts_) / 1e9);
        const double d = (*mid - last_mid_) / 1e4;
        var_rate_ = (1.0 - cfg_.vol_alpha) * var_rate_ + cfg_.vol_alpha * d * d / dt;
    }
    if (!have_mid_ || *mid != last_mid_) {
        last_mid_ = *mid;
        last_mid_ts_ = ev.ts;
    }
    have_mid_ = true;
    resolve_adverse(ev.ts, *mid);

    const bool due = ev.ts - last_quote_ts_ >= cfg_.requote_interval_ns;
    const bool moved = std::fabs(*mid - last_quote_mid_) >= kTick;
    if (due || moved) requote(ev.ts, book);
}

void Backtester::run_actions(Nanos now, const OrderBook& book) {
    while (actions_done_ < actions_.size() && actions_[actions_done_].at <= now) {
        const Action& a = actions_[actions_done_++];
        if (a.place) fills_.place(a.id, a.side, a.price, a.qty, a.at, book);
        else fills_.cancel(a.id);
    }
}

void Backtester::book_new_fills() {
    const auto& all = fills_.fills();
    for (; fills_seen_ < all.size(); ++fills_seen_) {
        const Fill& f = all[fills_seen_];
        const int s = sign(f.side);
        position_ += static_cast<std::int64_t>(s) * f.qty;
        cash_ -= static_cast<double>(s) * f.price * f.qty;
        (s > 0 ? bought_ : sold_) += f.qty;
        max_abs_pos_ = std::max<std::int64_t>(max_abs_pos_, std::llabs(position_));
        edge_sum_ += static_cast<double>(s) * (f.mid_at_fill - f.price) * f.qty;
        adverse_.push_back(Pending{f.ts + cfg_.adverse_horizon_ns, f.side, f.mid_at_fill, f.qty});
    }
}

void Backtester::resolve_adverse(Nanos now, double mid) {
    while (adverse_done_ < adverse_.size() && adverse_[adverse_done_].at <= now) {
        const Pending& p = adverse_[adverse_done_++];
        // Cost is positive when the midpoint moved against the side we just traded.
        adverse_sum_ += -static_cast<double>(sign(p.side)) * (mid - p.mid_at_fill) * p.qty;
        adverse_n_ += p.qty;
    }
}

void Backtester::requote(Nanos now, const OrderBook& book) {
    const auto mid = book.mid();
    const auto bb = book.best_bid();
    const auto ba = book.best_ask();
    if (!mid || !bb || !ba || *ba <= *bb) return;

    // A quote that has been live for a while and is no longer in the fill model was filled.
    if (bid_id_ && now >= bid_live_from_ && !fills_.find(bid_id_)) bid_id_ = 0;
    if (ask_id_ && now >= ask_live_from_ && !fills_.find(ask_id_)) ask_id_ = 0;

    const double sigma2 = std::max(var_rate_, 1e-8);
    const double lots = static_cast<double>(position_) / static_cast<double>(cfg_.quote_size);
    const double skew = lots * cfg_.gamma * sigma2 * cfg_.horizon_s;  // dollars
    const double half = 0.5 * (cfg_.gamma * sigma2 * cfg_.horizon_s +
                               (2.0 / cfg_.gamma) * std::log(1.0 + cfg_.gamma / cfg_.kappa));
    const double reservation = *mid - skew * 1e4;
    const Price half_units = std::max<Price>(cfg_.min_half_spread, static_cast<Price>(std::llround(half * 1e4)));

    Price bid = floor_tick(reservation - half_units);
    Price ask = ceil_tick(reservation + half_units);
    bid = std::max<Price>(bid, *bb - cfg_.max_ticks_from_touch * kTick);
    ask = std::min<Price>(ask, *ba + cfg_.max_ticks_from_touch * kTick);
    bid = std::min<Price>(bid, *ba - kTick);  // passive only: never cross the book
    ask = std::max<Price>(ask, *bb + kTick);
    if (bid >= ask) ask = bid + kTick;

    const std::int64_t room_buy = cfg_.max_position - position_;
    const std::int64_t room_sell = cfg_.max_position + position_;
    const Qty bid_qty = room_buy <= 0 ? 0 : static_cast<Qty>(std::min<std::int64_t>(cfg_.quote_size, room_buy));
    const Qty ask_qty = room_sell <= 0 ? 0 : static_cast<Qty>(std::min<std::int64_t>(cfg_.quote_size, room_sell));

    const Nanos at = now + cfg_.latency_ns;
    auto update_side = [&](Side side, std::uint64_t& id, Price& cur_price, Qty& cur_qty, Nanos& live_from,
                           Price want_price, Qty want_qty) {
        const bool have = id != 0;
        if (have && want_qty > 0 && cur_price == want_price && cur_qty == want_qty) return;
        if (have) {
            actions_.push_back(Action{at, false, id, side, cur_price, cur_qty});
            id = 0;
        }
        if (want_qty > 0) {
            id = next_id_++;
            cur_price = want_price;
            cur_qty = want_qty;
            live_from = at;
            actions_.push_back(Action{at, true, id, side, want_price, want_qty});
            ++quotes_;
        }
    };
    update_side(Side::Buy, bid_id_, bid_price_, bid_qty_, bid_live_from_, bid, bid_qty);
    update_side(Side::Sell, ask_id_, ask_price_, ask_qty_, ask_live_from_, ask, ask_qty);

    last_quote_ts_ = now;
    last_quote_mid_ = *mid;
}

BacktestResult Backtester::finish(const OrderBook& book) {
    const double mid = book.mid().value_or(last_mid_);
    BacktestResult r;
    r.events = events_;
    r.quotes_placed = quotes_;
    r.fills = fills_.fills().size();
    r.shares_bought = bought_;
    r.shares_sold = sold_;
    r.final_position = position_;
    r.max_abs_position = max_abs_pos_;
    r.pnl = (cash_ + static_cast<double>(position_) * mid) / 1e4;
    r.spread_capture = edge_sum_ / 1e4;
    r.inventory_pnl = r.pnl - r.spread_capture;
    r.adverse_cost = adverse_sum_ / 1e4;
    r.adverse_samples = adverse_n_;
    const double traded = static_cast<double>(bought_ + sold_);
    r.edge_per_share = traded > 0 ? r.spread_capture / traded : 0.0;
    r.adverse_per_share = adverse_n_ > 0 ? r.adverse_cost / static_cast<double>(adverse_n_) : 0.0;
    return r;
}

}  // namespace tickbook
