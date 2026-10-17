// Dependency-free tests for the parser, order book, matching engine, fill model,
// backtester and the synthetic feed generator.
#include <cmath>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include "tickbook/backtest.h"
#include "tickbook/fill_model.h"
#include "tickbook/itch.h"
#include "tickbook/matching_engine.h"
#include "tickbook/order_book.h"
#include "tickbook/synth.h"

using namespace tickbook;

namespace {

int g_failures = 0;
int g_checks = 0;
std::vector<std::pair<std::string, std::function<void()>>>& registry() {
    static std::vector<std::pair<std::string, std::function<void()>>> r;
    return r;
}
struct Register {
    Register(const char* name, std::function<void()> fn) { registry().emplace_back(name, std::move(fn)); }
};

#define CHECK(cond)                                                                        \
    do {                                                                                   \
        ++g_checks;                                                                        \
        if (!(cond)) {                                                                     \
            ++g_failures;                                                                  \
            std::cout << "  CHECK failed: " #cond " (line " << __LINE__ << ")\n";          \
        }                                                                                  \
    } while (0)
#define TEST(name)                               \
    static void name();                          \
    static Register reg_##name(#name, name);     \
    static void name()

bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }

// Parse a single encoded message (skipping its 2-byte length prefix).
bool parse_one(const std::vector<std::uint8_t>& msg, Event& ev) {
    return parse_message(msg.data() + 2, msg.size() - 2, ev);
}

Event make_event(EventType t, OrderRef ref, Side side = Side::Buy, Price price = 0, Qty shares = 0, Nanos ts = 0) {
    Event e;
    e.type = t;
    e.ref = ref;
    e.side = side;
    e.price = price;
    e.shares = shares;
    e.ts = ts;
    return e;
}

}  // namespace

// ================================================================== ITCH
TEST(itch_add_roundtrip) {
    std::vector<std::uint8_t> m;
    encode_add(m, 7, 34'200'123'456'789ULL, 0x0102030405060708ULL, Side::Sell, 250, "AAPL", 1'234'500);
    Event ev;
    CHECK(parse_one(m, ev));
    CHECK(ev.type == EventType::Add);
    CHECK(ev.locate == 7);
    CHECK(ev.ts == 34'200'123'456'789ULL);
    CHECK(ev.ref == 0x0102030405060708ULL);
    CHECK(ev.side == Side::Sell);
    CHECK(ev.shares == 250);
    CHECK(ev.price == 1'234'500);
    CHECK(std::string(ev.stock) == "AAPL");
}

TEST(itch_buy_side_roundtrip) {
    std::vector<std::uint8_t> m;
    encode_add(m, 1, 5, 9, Side::Buy, 100, "X", 100);
    Event ev;
    CHECK(parse_one(m, ev));
    CHECK(ev.side == Side::Buy);
}

TEST(itch_execute_roundtrip) {
    std::vector<std::uint8_t> m;
    encode_execute(m, 3, 99, 42, 300, 777);
    Event ev;
    CHECK(parse_one(m, ev));
    CHECK(ev.type == EventType::Execute && ev.ref == 42 && ev.shares == 300 && ev.locate == 3);
}

TEST(itch_cancel_delete_replace_roundtrip) {
    std::vector<std::uint8_t> m;
    Event ev;
    encode_cancel(m, 2, 10, 5, 40);
    CHECK(parse_one(m, ev) && ev.type == EventType::Cancel && ev.ref == 5 && ev.shares == 40);
    m.clear();
    encode_delete(m, 2, 11, 6);
    CHECK(parse_one(m, ev) && ev.type == EventType::Delete && ev.ref == 6);
    m.clear();
    encode_replace(m, 2, 12, 7, 8, 500, 987'600);
    CHECK(parse_one(m, ev) && ev.type == EventType::Replace && ev.ref == 7 && ev.new_ref == 8);
    CHECK(ev.shares == 500 && ev.price == 987'600);
}

TEST(itch_system_event_and_directory) {
    std::vector<std::uint8_t> m;
    Event ev;
    encode_system_event(m, 1, 'O');
    CHECK(parse_one(m, ev) && ev.type == EventType::SystemEvent && ev.event_code == 'O');
    m.clear();
    encode_stock_directory(m, 12, 1, "MSFT");
    CHECK(parse_one(m, ev) && ev.type == EventType::StockDirectory && std::string(ev.stock) == "MSFT" && ev.locate == 12);
}

TEST(itch_add_with_mpid_parses_like_add) {
    std::vector<std::uint8_t> m;
    encode_add(m, 1, 1, 77, Side::Buy, 100, "ABCD", 5000);
    m[2] = 'F';
    for (int i = 0; i < 4; ++i) m.push_back('M');
    Event ev;
    CHECK(parse_message(m.data() + 2, m.size() - 2, ev));
    CHECK(ev.type == EventType::Add && ev.ref == 77);
}

TEST(itch_execute_with_price) {
    std::vector<std::uint8_t> m;
    encode_execute(m, 1, 1, 55, 100, 1);
    m[2] = 'C';
    m.push_back('Y');
    for (int shift = 24; shift >= 0; shift -= 8) m.push_back(static_cast<std::uint8_t>(123456 >> shift));
    Event ev;
    CHECK(parse_message(m.data() + 2, m.size() - 2, ev));
    CHECK(ev.type == EventType::ExecuteWithPrice && ev.ref == 55 && ev.price == 123456);
}

TEST(itch_unknown_type_is_other) {
    std::vector<std::uint8_t> body(20, 0);
    body[0] = 'Z';
    Event ev;
    CHECK(parse_message(body.data(), body.size(), ev));
    CHECK(ev.type == EventType::Other);
}

TEST(itch_short_message_rejected) {
    std::vector<std::uint8_t> m;
    encode_add(m, 1, 1, 1, Side::Buy, 100, "A", 1);
    Event ev;
    CHECK(!parse_message(m.data() + 2, 20, ev));
    CHECK(!parse_message(m.data() + 2, 5, ev));
}

TEST(itch_stock_name_is_trimmed) {
    std::vector<std::uint8_t> m;
    encode_add(m, 1, 1, 1, Side::Buy, 100, "IBM", 1);
    Event ev;
    parse_one(m, ev);
    CHECK(std::string(ev.stock) == "IBM");
}

TEST(itch_framing_counts_messages) {
    std::vector<std::uint8_t> feed;
    encode_system_event(feed, 1, 'O');
    encode_add(feed, 1, 2, 1, Side::Buy, 100, "A", 100);
    encode_delete(feed, 1, 3, 1);
    int seen = 0;
    const auto n = for_each_message(feed.data(), feed.size(), [&](const Event&) { ++seen; });
    CHECK(n == 3 && seen == 3);
}

TEST(itch_framing_ignores_truncated_tail) {
    std::vector<std::uint8_t> feed;
    encode_add(feed, 1, 2, 1, Side::Buy, 100, "A", 100);
    encode_add(feed, 1, 3, 2, Side::Buy, 100, "A", 100);
    feed.resize(feed.size() - 5);
    int seen = 0;
    for_each_message(feed.data(), feed.size(), [&](const Event&) { ++seen; });
    CHECK(seen == 1);
}

TEST(itch_empty_buffer) {
    int seen = 0;
    CHECK(for_each_message(nullptr, 0, [&](const Event&) { ++seen; }) == 0);
    CHECK(seen == 0);
}

// ================================================================== order book
TEST(book_best_prices) {
    OrderBook b;
    b.add(1, Side::Buy, 10000, 100);
    b.add(2, Side::Buy, 10100, 100);
    b.add(3, Side::Sell, 10300, 100);
    b.add(4, Side::Sell, 10200, 100);
    CHECK(*b.best_bid() == 10100);
    CHECK(*b.best_ask() == 10200);
    CHECK(*b.spread() == 100);
    CHECK(near(*b.mid(), 10150.0));
}

TEST(book_empty_side_has_no_best_or_mid) {
    OrderBook b;
    b.add(1, Side::Buy, 10000, 100);
    CHECK(!b.best_ask().has_value());
    CHECK(!b.mid().has_value());
    CHECK(!b.spread().has_value());
}

TEST(book_fifo_within_level) {
    OrderBook b;
    b.add(1, Side::Buy, 10000, 100);
    b.add(2, Side::Buy, 10000, 200);
    CHECK(b.front_order(Side::Buy)->ref == 1);
    b.remove(1);
    CHECK(b.front_order(Side::Buy)->ref == 2);
}

TEST(book_level_quantity_sums_orders) {
    OrderBook b;
    b.add(1, Side::Sell, 10100, 100);
    b.add(2, Side::Sell, 10100, 250);
    CHECK(b.qty_at(Side::Sell, 10100) == 350);
    CHECK(b.qty_at(Side::Sell, 10200) == 0);
}

TEST(book_partial_cancel_reduces_quantity) {
    OrderBook b;
    b.add(1, Side::Buy, 10000, 500);
    CHECK(b.cancel(1, 200));
    CHECK(b.find(1)->qty == 300);
    CHECK(b.qty_at(Side::Buy, 10000) == 300);
}

TEST(book_cancel_of_full_quantity_removes_order) {
    OrderBook b;
    b.add(1, Side::Buy, 10000, 100);
    CHECK(b.cancel(1, 100));
    CHECK(!b.contains(1));
    CHECK(b.level_count(Side::Buy) == 0);
}

TEST(book_execute_keeps_priority_of_remainder) {
    OrderBook b;
    b.add(1, Side::Buy, 10000, 300);
    b.add(2, Side::Buy, 10000, 300);
    b.execute(1, 100);
    CHECK(b.front_order(Side::Buy)->ref == 1);
    CHECK(b.front_order(Side::Buy)->qty == 200);
}

TEST(book_rejects_duplicates_zero_and_unknown) {
    OrderBook b;
    CHECK(b.add(1, Side::Buy, 10000, 100));
    CHECK(!b.add(1, Side::Sell, 10100, 100));
    CHECK(!b.add(2, Side::Buy, 10000, 0));
    CHECK(!b.remove(99));
    CHECK(!b.cancel(99, 10));
    CHECK(!b.execute(99, 10));
    CHECK(b.order_count() == 1);
}

TEST(book_replace_moves_to_back_of_new_level) {
    OrderBook b;
    b.add(1, Side::Buy, 10000, 100);
    b.add(2, Side::Buy, 10000, 100);
    CHECK(b.replace(1, 3, 100, 10000));  // same price: still loses priority
    CHECK(b.front_order(Side::Buy)->ref == 2);
    CHECK(!b.contains(1));
    CHECK(b.contains(3));
}

TEST(book_replace_can_change_price_and_size) {
    OrderBook b;
    b.add(1, Side::Sell, 10100, 100);
    CHECK(b.replace(1, 2, 400, 10200));
    CHECK(b.qty_at(Side::Sell, 10100) == 0);
    CHECK(b.qty_at(Side::Sell, 10200) == 400);
    CHECK(*b.best_ask() == 10200);
}

TEST(book_replace_rejects_unknown_or_colliding_refs) {
    OrderBook b;
    b.add(1, Side::Buy, 10000, 100);
    b.add(2, Side::Buy, 10000, 100);
    CHECK(!b.replace(9, 10, 100, 10000));
    CHECK(!b.replace(1, 2, 100, 10000));
    CHECK(b.contains(1) && b.contains(2));
}

TEST(book_depth_is_ordered_best_first) {
    OrderBook b;
    for (int i = 0; i < 5; ++i) b.add(10 + i, Side::Buy, 10000 - i * 100, 100 + i);
    for (int i = 0; i < 5; ++i) b.add(20 + i, Side::Sell, 10100 + i * 100, 100);
    const auto bids = b.depth(Side::Buy, 3);
    const auto asks = b.depth(Side::Sell, 3);
    CHECK(bids.size() == 3 && asks.size() == 3);
    CHECK(bids[0].price == 10000 && bids[1].price == 9900 && bids[2].price == 9800);
    CHECK(asks[0].price == 10100 && asks[2].price == 10300);
}

TEST(book_reuses_freed_slots_without_corruption) {
    OrderBook b;
    for (OrderRef r = 1; r <= 100; ++r) b.add(r, Side::Buy, 10000 + static_cast<Price>(r % 7) * 100, 100);
    for (OrderRef r = 1; r <= 100; r += 2) b.remove(r);
    for (OrderRef r = 101; r <= 150; ++r) b.add(r, Side::Sell, 20000 + static_cast<Price>(r % 5) * 100, 50);
    CHECK(b.order_count() == 100);
    Qty bid_total = 0;
    for (const auto& l : b.depth(Side::Buy, 100)) bid_total += l.qty;
    CHECK(bid_total == 50 * 100);
}

TEST(book_never_reports_crossed_for_non_crossing_input) {
    OrderBook b;
    b.add(1, Side::Buy, 10000, 100);
    b.add(2, Side::Sell, 10100, 100);
    CHECK(!b.crossed());
}

TEST(book_apply_handles_each_event_type) {
    OrderBook b;
    CHECK(apply(b, make_event(EventType::Add, 1, Side::Buy, 10000, 300)));
    CHECK(apply(b, make_event(EventType::Cancel, 1, Side::Buy, 0, 100)));
    CHECK(b.find(1)->qty == 200);
    CHECK(apply(b, make_event(EventType::Execute, 1, Side::Buy, 0, 50)));
    CHECK(b.find(1)->qty == 150);
    Event rep = make_event(EventType::Replace, 1, Side::Buy, 10100, 400);
    rep.new_ref = 2;
    CHECK(apply(b, rep));
    CHECK(b.find(2)->price == 10100);
    CHECK(apply(b, make_event(EventType::Delete, 2)));
    CHECK(b.order_count() == 0);
    CHECK(apply(b, make_event(EventType::SystemEvent, 0)));  // not order flow: accepted, no effect
}

TEST(book_apply_reports_invalid_events) {
    OrderBook b;
    CHECK(!apply(b, make_event(EventType::Delete, 5)));
    CHECK(!apply(b, make_event(EventType::Execute, 5, Side::Buy, 0, 10)));
}

// ================================================================== matching engine
TEST(engine_non_crossing_limit_rests) {
    MatchingEngine e;
    CHECK(e.submit_limit(1, Side::Buy, 10000, 100).empty());
    CHECK(e.submit_limit(2, Side::Sell, 10100, 100).empty());
    CHECK(e.book().order_count() == 2);
}

TEST(engine_crossing_limit_trades_at_resting_price) {
    MatchingEngine e;
    e.submit_limit(1, Side::Sell, 10000, 100);
    const auto trades = e.submit_limit(2, Side::Buy, 10100, 100);
    CHECK(trades.size() == 1);
    CHECK(trades[0].price == 10000 && trades[0].qty == 100 && trades[0].maker == 1 && trades[0].taker == 2);
    CHECK(e.book().order_count() == 0);
}

TEST(engine_partial_fill_rests_remainder) {
    MatchingEngine e;
    e.submit_limit(1, Side::Sell, 10000, 60);
    const auto trades = e.submit_limit(2, Side::Buy, 10000, 100);
    CHECK(trades.size() == 1 && trades[0].qty == 60);
    CHECK(e.book().find(2)->qty == 40);
    CHECK(*e.book().best_bid() == 10000);
}

TEST(engine_time_priority_at_equal_price) {
    MatchingEngine e;
    e.submit_limit(1, Side::Sell, 10000, 100);
    e.submit_limit(2, Side::Sell, 10000, 100);
    const auto trades = e.submit_limit(3, Side::Buy, 10000, 100);
    CHECK(trades.size() == 1 && trades[0].maker == 1);
}

TEST(engine_price_priority_across_levels) {
    MatchingEngine e;
    e.submit_limit(1, Side::Sell, 10200, 100);
    e.submit_limit(2, Side::Sell, 10100, 100);
    const auto trades = e.submit_limit(3, Side::Buy, 10300, 100);
    CHECK(trades.size() == 1 && trades[0].maker == 2 && trades[0].price == 10100);
}

TEST(engine_market_order_walks_the_book) {
    MatchingEngine e;
    e.submit_limit(1, Side::Sell, 10100, 100);
    e.submit_limit(2, Side::Sell, 10200, 100);
    e.submit_limit(3, Side::Sell, 10300, 100);
    const auto trades = e.submit_market(9, Side::Buy, 250);
    CHECK(trades.size() == 3);
    CHECK(trades[0].price == 10100 && trades[1].price == 10200 && trades[2].price == 10300);
    CHECK(trades[2].qty == 50);
    CHECK(e.book().qty_at(Side::Sell, 10300) == 50);
}

TEST(engine_unfilled_market_remainder_is_discarded) {
    MatchingEngine e;
    e.submit_limit(1, Side::Sell, 10100, 100);
    const auto trades = e.submit_market(2, Side::Buy, 500);
    CHECK(trades.size() == 1 && trades[0].qty == 100);
    CHECK(e.book().order_count() == 0);
}

TEST(engine_limit_does_not_trade_through_its_price) {
    MatchingEngine e;
    e.submit_limit(1, Side::Sell, 10100, 100);
    e.submit_limit(2, Side::Sell, 10300, 100);
    const auto trades = e.submit_limit(3, Side::Buy, 10200, 300);
    CHECK(trades.size() == 1 && trades[0].price == 10100);
    CHECK(e.book().find(3)->qty == 200);
    CHECK(*e.book().best_bid() == 10200);
}

TEST(engine_cancel_removes_resting_order) {
    MatchingEngine e;
    e.submit_limit(1, Side::Buy, 10000, 100);
    CHECK(e.cancel(1));
    CHECK(!e.cancel(1));
    CHECK(e.book().order_count() == 0);
}

TEST(engine_ignores_duplicate_ids_and_zero_quantity) {
    MatchingEngine e;
    e.submit_limit(1, Side::Buy, 10000, 100);
    CHECK(e.submit_limit(1, Side::Sell, 9000, 100).empty());
    CHECK(e.submit_limit(2, Side::Sell, 9000, 0).empty());
    CHECK(e.book().order_count() == 1);
}

TEST(engine_conserves_quantity) {
    MatchingEngine e;
    e.submit_limit(1, Side::Sell, 10000, 70);
    e.submit_limit(2, Side::Sell, 10100, 70);
    const auto trades = e.submit_limit(3, Side::Buy, 10100, 200);
    Qty traded = 0;
    for (const auto& t : trades) traded += t.qty;
    CHECK(traded == 140);
    CHECK(e.book().find(3)->qty == 200 - traded);
}

TEST(engine_book_is_never_crossed_after_random_flow) {
    MatchingEngine e;
    std::uint64_t x = 88172645463325252ULL;
    auto rnd = [&] { x ^= x << 13; x ^= x >> 7; x ^= x << 17; return x; };
    bool crossed = false;
    for (OrderRef id = 1; id <= 5000; ++id) {
        const Side s = rnd() % 2 ? Side::Buy : Side::Sell;
        const Price p = 10000 + static_cast<Price>(rnd() % 21) * 100 - 1000;
        e.submit_limit(id, s, p, static_cast<Qty>(100 + rnd() % 400));
        if (e.book().crossed()) crossed = true;
    }
    CHECK(!crossed);
}

// ================================================================== fill model
namespace {
OrderBook book_with_queue() {
    OrderBook b;
    b.add(1, Side::Buy, 10000, 300);   // ahead of us
    b.add(2, Side::Buy, 10000, 200);
    b.add(3, Side::Sell, 10100, 400);
    return b;
}
}  // namespace

TEST(fill_order_joins_back_of_queue) {
    OrderBook b = book_with_queue();
    FillModel m;
    CHECK(m.place(100, Side::Buy, 10000, 100, 0, b));
    CHECK(near(m.find(100)->queue_ahead, 500.0));
}

TEST(fill_post_only_rejects_crossing_orders) {
    OrderBook b = book_with_queue();
    FillModel m;
    CHECK(!m.place(1, Side::Buy, 10100, 100, 0, b));   // would lift the ask
    CHECK(!m.place(2, Side::Sell, 10000, 100, 0, b));  // would hit the bid
    CHECK(m.orders().empty());
}

TEST(fill_no_fill_until_queue_is_cleared) {
    OrderBook b = book_with_queue();
    FillModel m;
    m.place(100, Side::Buy, 10000, 100, 0, b);
    m.on_event(make_event(EventType::Execute, 1, Side::Buy, 0, 300, 5), b);
    CHECK(m.fills().empty());
    CHECK(near(m.find(100)->queue_ahead, 200.0));
}

TEST(fill_partial_fill_after_queue_clears) {
    OrderBook b = book_with_queue();
    FillModel m;
    m.place(100, Side::Buy, 10000, 100, 0, b);
    m.on_event(make_event(EventType::Execute, 1, Side::Buy, 0, 300, 5), b);
    b.execute(1, 300);
    m.on_event(make_event(EventType::Execute, 2, Side::Buy, 0, 200, 6), b);
    b.execute(2, 200);
    CHECK(m.fills().empty());  // queue exactly cleared, nothing left over for us yet
    b.add(4, Side::Buy, 10000, 500);
    m.on_event(make_event(EventType::Execute, 4, Side::Buy, 0, 40, 7), b);
    CHECK(m.fills().size() == 1);
    CHECK(m.fills()[0].qty == 40);
    CHECK(m.find(100)->remaining == 60);
}

TEST(fill_overflow_beyond_queue_fills_us) {
    OrderBook b;
    b.add(1, Side::Sell, 10100, 200);  // queue ahead of our sell is 200
    b.add(2, Side::Buy, 10000, 100);
    FillModel m;
    m.place(100, Side::Sell, 10100, 100, 0, b);
    m.on_event(make_event(EventType::Execute, 1, Side::Sell, 0, 200, 5), b);
    CHECK(m.fills().empty());
    b.execute(1, 200);
    b.add(3, Side::Sell, 10100, 500);  // a new order queues behind us in reality; the model keeps us first
    m.on_event(make_event(EventType::Execute, 3, Side::Sell, 0, 60, 6), b);
    CHECK(m.fills().size() == 1 && m.fills()[0].qty == 60);
    CHECK(m.find(100)->remaining == 40);
}

TEST(fill_full_fill_removes_order_and_records_price_and_side) {
    OrderBook b;
    b.add(1, Side::Buy, 10000, 100);
    b.add(2, Side::Sell, 10100, 100);
    FillModel m;
    m.place(100, Side::Buy, 10000, 50, 0, b);
    m.on_event(make_event(EventType::Execute, 1, Side::Buy, 0, 100, 5), b);
    b.execute(1, 100);
    CHECK(m.fills().empty());
    b.add(3, Side::Buy, 10000, 400);
    m.on_event(make_event(EventType::Execute, 3, Side::Buy, 0, 80, 9), b);
    CHECK(m.fills().size() == 1);
    CHECK(m.fills()[0].qty == 50 && m.fills()[0].price == 10000 && m.fills()[0].side == Side::Buy);
    CHECK(m.find(100) == nullptr);
}

TEST(fill_execution_at_other_price_is_ignored) {
    OrderBook b = book_with_queue();
    FillModel m;
    m.place(100, Side::Buy, 10000, 100, 0, b);
    m.on_event(make_event(EventType::Execute, 3, Side::Sell, 0, 400, 5), b);
    CHECK(near(m.find(100)->queue_ahead, 500.0));
    CHECK(m.fills().empty());
}

TEST(fill_cancel_ahead_moves_us_forward_proportionally) {
    OrderBook b = book_with_queue();  // level holds 500, we have 500 ahead
    FillModel m;
    m.place(100, Side::Buy, 10000, 100, 0, b);
    m.on_event(make_event(EventType::Cancel, 1, Side::Buy, 0, 150, 5), b);  // 150 of 500 cancelled
    CHECK(near(m.find(100)->queue_ahead, 350.0));
}

TEST(fill_delete_ahead_moves_us_forward_proportionally) {
    OrderBook b = book_with_queue();
    FillModel m;
    m.place(100, Side::Buy, 10000, 100, 0, b);
    m.on_event(make_event(EventType::Delete, 1, Side::Buy, 0, 0, 5), b);  // removes the 300-share order
    CHECK(near(m.find(100)->queue_ahead, 200.0));
}

TEST(fill_cancel_elsewhere_does_not_move_us) {
    OrderBook b = book_with_queue();
    FillModel m;
    m.place(100, Side::Buy, 10000, 100, 0, b);
    m.on_event(make_event(EventType::Delete, 3, Side::Sell, 0, 0, 5), b);
    CHECK(near(m.find(100)->queue_ahead, 500.0));
}

TEST(fill_cancel_own_order) {
    OrderBook b = book_with_queue();
    FillModel m;
    m.place(100, Side::Buy, 10000, 100, 0, b);
    CHECK(m.cancel(100));
    CHECK(!m.cancel(100));
    CHECK(m.orders().empty());
}

TEST(fill_duplicate_id_rejected) {
    OrderBook b = book_with_queue();
    FillModel m;
    CHECK(m.place(100, Side::Buy, 10000, 100, 0, b));
    CHECK(!m.place(100, Side::Buy, 9900, 100, 0, b));
}

TEST(fill_records_midpoint_at_fill_time) {
    OrderBook b;
    b.add(1, Side::Buy, 10000, 100);
    b.add(2, Side::Sell, 10200, 100);
    FillModel m;
    m.place(100, Side::Buy, 10000, 50, 0, b);
    m.on_event(make_event(EventType::Execute, 1, Side::Buy, 0, 100, 5), b);
    b.execute(1, 100);
    b.add(3, Side::Buy, 10000, 300);
    m.on_event(make_event(EventType::Execute, 3, Side::Buy, 0, 100, 6), b);
    CHECK(m.fills().size() == 1);
    CHECK(near(m.fills()[0].mid_at_fill, 10100.0));
}

// ================================================================== synthetic feed
namespace {
struct ReplayStats {
    std::uint64_t applied = 0, rejected = 0, order_events = 0;
    bool ever_crossed = false;
};
ReplayStats replay(const std::vector<std::uint8_t>& feed) {
    ReplayStats s;
    OrderBook book;
    for_each_message(feed.data(), feed.size(), [&](const Event& ev) {
        if (ev.type == EventType::Other || ev.type == EventType::SystemEvent ||
            ev.type == EventType::StockDirectory)
            return;
        ++s.order_events;
        (apply(book, ev) ? s.applied : s.rejected)++;
        if (book.crossed()) s.ever_crossed = true;
    });
    return s;
}
}  // namespace

TEST(synth_feed_is_fully_valid_for_several_seeds) {
    for (unsigned seed = 1; seed <= 5; ++seed) {
        SynthConfig cfg;
        cfg.events = 20'000;
        cfg.seed = seed;
        const ReplayStats s = replay(generate_feed(cfg));
        CHECK(s.rejected == 0);
        CHECK(s.order_events == cfg.events);
    }
}

TEST(synth_book_never_crosses) {
    SynthConfig cfg;
    cfg.events = 50'000;
    CHECK(!replay(generate_feed(cfg)).ever_crossed);
}

TEST(synth_is_deterministic_per_seed) {
    SynthConfig cfg;
    cfg.events = 5'000;
    cfg.seed = 11;
    CHECK(generate_feed(cfg) == generate_feed(cfg));
    SynthConfig other = cfg;
    other.seed = 12;
    CHECK(generate_feed(cfg) != generate_feed(other));
}

TEST(synth_feed_starts_with_system_event_and_directory) {
    SynthConfig cfg;
    cfg.events = 10;
    cfg.symbol = "ZZTOP";
    const auto feed = generate_feed(cfg);
    std::vector<Event> evs;
    for_each_message(feed.data(), feed.size(), [&](const Event& e) { evs.push_back(e); });
    CHECK(evs.size() == 12);
    CHECK(evs[0].type == EventType::SystemEvent);
    CHECK(evs[1].type == EventType::StockDirectory && std::string(evs[1].stock) == "ZZTOP");
}

TEST(synth_timestamps_never_go_backwards) {
    SynthConfig cfg;
    cfg.events = 10'000;
    const auto feed = generate_feed(cfg);
    Nanos last = 0;
    bool ordered = true;
    for_each_message(feed.data(), feed.size(), [&](const Event& e) {
        if (e.ts < last) ordered = false;
        last = e.ts;
    });
    CHECK(ordered);
}

// ================================================================== backtester
namespace {
BacktestResult run_backtest(unsigned seed, std::uint64_t events, MMConfig cfg = {}) {
    SynthConfig sc;
    sc.events = events;
    sc.seed = seed;
    const auto feed = generate_feed(sc);
    OrderBook book;
    Backtester bt(cfg);
    for_each_message(feed.data(), feed.size(), [&](const Event& ev) {
        if (ev.type == EventType::Other || ev.type == EventType::SystemEvent ||
            ev.type == EventType::StockDirectory)
            return;
        bt.on_event(ev, book);
    });
    return bt.finish(book);
}
}  // namespace

TEST(backtest_trades_on_a_live_feed) {
    const BacktestResult r = run_backtest(1, 200'000);
    CHECK(r.quotes_placed > 0);
    CHECK(r.fills > 0);
    CHECK(r.shares_bought > 0 && r.shares_sold > 0);
}

TEST(backtest_is_deterministic) {
    const BacktestResult a = run_backtest(3, 60'000);
    const BacktestResult b = run_backtest(3, 60'000);
    CHECK(a.fills == b.fills && a.final_position == b.final_position && near(a.pnl, b.pnl, 1e-9));
}

TEST(backtest_position_matches_traded_shares) {
    const BacktestResult r = run_backtest(2, 150'000);
    CHECK(r.final_position == static_cast<std::int64_t>(r.shares_bought) - static_cast<std::int64_t>(r.shares_sold));
}

TEST(backtest_never_exceeds_position_limit) {
    MMConfig cfg;
    cfg.max_position = 300;
    const BacktestResult r = run_backtest(4, 200'000, cfg);
    CHECK(r.max_abs_position <= 300 + cfg.quote_size);  // an in-flight quote may fill just past the cap
}

TEST(backtest_pnl_splits_into_spread_capture_and_inventory) {
    const BacktestResult r = run_backtest(5, 150'000);
    CHECK(near(r.pnl, r.spread_capture + r.inventory_pnl, 1e-9));
}

TEST(backtest_spread_capture_is_positive_for_passive_fills) {
    // A passive order is filled at or inside the best price, so its average edge against the
    // midpoint should not be negative on a book with a one-tick spread.
    const BacktestResult r = run_backtest(6, 200'000);
    CHECK(r.fills > 0);
    CHECK(r.edge_per_share >= 0.0);
}

TEST(backtest_with_huge_latency_still_runs) {
    MMConfig cfg;
    cfg.latency_ns = 5'000'000'000ULL;
    const BacktestResult r = run_backtest(7, 50'000, cfg);
    CHECK(r.events == 50'000);
}

TEST(backtest_no_quotes_without_both_sides) {
    OrderBook book;
    Backtester bt;
    Event ev = make_event(EventType::Add, 1, Side::Buy, 10000, 100, 1'000'000'000);
    bt.on_event(ev, book);
    const BacktestResult r = bt.finish(book);
    CHECK(r.quotes_placed == 0 && r.fills == 0);
}

int main() {
    int failed_tests = 0;
    for (auto& [name, fn] : registry()) {
        const int before = g_failures;
        fn();
        const bool ok = g_failures == before;
        std::cout << (ok ? "[ ok ] " : "[FAIL] ") << name << "\n";
        if (!ok) ++failed_tests;
    }
    std::cout << "\n" << registry().size() << " tests, " << g_checks << " checks, " << failed_tests << " failed\n";
    return failed_tests ? 1 : 0;
}
