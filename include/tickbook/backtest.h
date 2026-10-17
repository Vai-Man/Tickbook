// Event-driven market-making backtest on a replayed order book.
//
// The strategy quotes a bid and an ask around an inventory-adjusted reservation price, using
// the Avellaneda-Stoikov model (2008). Its orders reach the exchange after a fixed latency and
// are filled by the queue-position model in fill_model.h, so it only trades when the
// historical flow would really have reached it.
#pragma once

#include <cstdint>
#include <vector>

#include "tickbook/fill_model.h"
#include "tickbook/order_book.h"

namespace tickbook {

struct MMConfig {
    double gamma = 1.0;               // risk aversion, per lot of inventory
    double kappa = 50.0;              // order-arrival decay, per dollar of distance from mid
    double horizon_s = 10.0;          // risk horizon (tau), seconds
    Qty quote_size = 100;
    std::int64_t max_position = 500;  // shares; the quote on the side that would breach it is withdrawn
    Nanos latency_ns = 100'000;       // time for an order action to reach the exchange
    Nanos requote_interval_ns = 100'000'000;
    Nanos adverse_horizon_ns = 1'000'000'000;
    double vol_alpha = 0.01;          // EWMA weight for the volatility estimate
    Price min_half_spread = kTick;
    // Quotes are never placed further than this many ticks behind the touch (0 = join or improve it).
    int max_ticks_from_touch = 0;
};

struct BacktestResult {
    std::uint64_t events = 0;
    std::uint64_t quotes_placed = 0;
    std::uint64_t fills = 0;
    std::uint64_t shares_bought = 0;
    std::uint64_t shares_sold = 0;
    std::int64_t final_position = 0;
    std::int64_t max_abs_position = 0;
    // All money figures are in dollars.
    double pnl = 0.0;              // cash + position marked at the final midpoint
    double spread_capture = 0.0;   // sum of (mid at fill - price) * side * shares
    double inventory_pnl = 0.0;    // pnl - spread_capture: gains and losses from holding inventory
    double adverse_cost = 0.0;     // how much the mid moved against fills within the adverse horizon
    std::uint64_t adverse_samples = 0;
    double edge_per_share = 0.0;
    double adverse_per_share = 0.0;
};

class Backtester {
public:
    explicit Backtester(MMConfig cfg = {});

    // Feed every event of ONE instrument, in order. The event is applied to `book`.
    void on_event(const Event& ev, OrderBook& book);
    // Mark to the last midpoint and summarise.
    BacktestResult finish(const OrderBook& book);

    const FillModel& fill_model() const { return fills_; }
    double volatility_per_s2() const { return var_rate_; }

private:
    struct Action {
        Nanos at;
        bool place;  // else cancel
        std::uint64_t id;
        Side side;
        Price price;
        Qty qty;
    };
    struct Pending {
        Nanos at;
        Side side;
        double mid_at_fill;
        Qty qty;
    };

    void run_actions(Nanos now, const OrderBook& book);
    void book_new_fills();
    void resolve_adverse(Nanos now, double mid);
    void requote(Nanos now, const OrderBook& book);

    MMConfig cfg_;
    FillModel fills_;
    std::vector<Action> actions_;
    std::vector<Pending> adverse_;
    std::size_t actions_done_ = 0;
    std::size_t adverse_done_ = 0;
    std::size_t fills_seen_ = 0;

    std::uint64_t next_id_ = 1;
    std::uint64_t bid_id_ = 0, ask_id_ = 0;
    Price bid_price_ = 0, ask_price_ = 0;
    Qty bid_qty_ = 0, ask_qty_ = 0;
    Nanos bid_live_from_ = 0, ask_live_from_ = 0;  // when the quote reaches the exchange

    std::int64_t position_ = 0;
    double cash_ = 0.0;  // price units
    double edge_sum_ = 0.0;
    double adverse_sum_ = 0.0;
    std::uint64_t adverse_n_ = 0;
    std::uint64_t quotes_ = 0, events_ = 0;
    std::uint64_t bought_ = 0, sold_ = 0;
    std::int64_t max_abs_pos_ = 0;

    double last_mid_ = 0.0;
    Nanos last_mid_ts_ = 0;
    Nanos last_quote_ts_ = 0;
    double last_quote_mid_ = 0.0;
    bool have_mid_ = false;
    double var_rate_ = 1e-4;  // $^2 per second
};

}  // namespace tickbook
