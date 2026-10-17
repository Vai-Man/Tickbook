// tickbook: replay, backtest and benchmark an ITCH 5.0 feed.
//
//   tickbook gen      --out FILE [--events N] [--seed S]
//   tickbook replay   --file FILE [--symbol SYM]
//   tickbook backtest --file FILE --symbol SYM
//   tickbook bench    [--events N] [--seed S] [--file FILE --symbol SYM]
//   tickbook demo
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "tickbook/backtest.h"
#include "tickbook/itch.h"
#include "tickbook/order_book.h"
#include "tickbook/synth.h"

using namespace tickbook;

namespace {

struct Args {
    std::string command;
    std::map<std::string, std::string> opts;
    std::string get(const std::string& k, const std::string& dflt = "") const {
        auto it = opts.find(k);
        return it == opts.end() ? dflt : it->second;
    }
    std::uint64_t num(const std::string& k, std::uint64_t dflt) const {
        auto it = opts.find(k);
        return it == opts.end() ? dflt : std::strtoull(it->second.c_str(), nullptr, 10);
    }
};

Args parse_args(int argc, char** argv) {
    Args a;
    if (argc > 1) a.command = argv[1];
    for (int i = 2; i + 1 < argc; i += 2) {
        std::string k = argv[i];
        if (k.rfind("--", 0) == 0) a.opts[k.substr(2)] = argv[i + 1];
    }
    return a;
}

std::string dollars(Price p) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.4f", p / 10000.0);
    return buf;
}

void print_result(const BacktestResult& r) {
    std::printf("events             %llu\n", static_cast<unsigned long long>(r.events));
    std::printf("quotes placed      %llu\n", static_cast<unsigned long long>(r.quotes_placed));
    std::printf("fills              %llu  (bought %llu, sold %llu shares)\n",
                static_cast<unsigned long long>(r.fills), static_cast<unsigned long long>(r.shares_bought),
                static_cast<unsigned long long>(r.shares_sold));
    std::printf("final position     %lld  (max |position| %lld)\n", static_cast<long long>(r.final_position),
                static_cast<long long>(r.max_abs_position));
    std::printf("PnL                $%.2f\n", r.pnl);
    std::printf("  spread capture   $%.2f  ($%.5f per share)\n", r.spread_capture, r.edge_per_share);
    std::printf("  inventory PnL    $%.2f\n", r.inventory_pnl);
    std::printf("adverse selection  $%.2f over %llu shares ($%.5f per share)\n", r.adverse_cost,
                static_cast<unsigned long long>(r.adverse_samples), r.adverse_per_share);
}

// Find the stock-locate code for a symbol from the directory messages.
std::int32_t find_locate(const std::vector<std::uint8_t>& feed, const std::string& symbol) {
    std::int32_t found = -1;
    for_each_message(feed.data(), feed.size(), [&](const Event& ev) {
        if (ev.type == EventType::StockDirectory && symbol == ev.stock) found = ev.locate;
    });
    return found;
}

int cmd_gen(const Args& a) {
    SynthConfig cfg;
    cfg.events = a.num("events", 1'000'000);
    cfg.seed = static_cast<unsigned>(a.num("seed", 1));
    const std::string out = a.get("out");
    if (out.empty()) {
        std::cerr << "gen needs --out FILE\n";
        return 2;
    }
    const auto feed = generate_feed(cfg);
    std::FILE* f = std::fopen(out.c_str(), "wb");
    if (!f) {
        std::cerr << "cannot write " << out << "\n";
        return 1;
    }
    std::fwrite(feed.data(), 1, feed.size(), f);
    std::fclose(f);
    std::cout << "wrote " << cfg.events << " messages (" << feed.size() << " bytes) to " << out << "\n";
    return 0;
}

int cmd_replay(const Args& a) {
    const auto feed = read_file(a.get("file"));
    const std::string symbol = a.get("symbol");
    std::map<std::uint16_t, std::string> names;
    std::unordered_map<std::uint16_t, OrderBook> books;
    std::map<char, std::uint64_t> counts;
    std::uint64_t applied = 0, rejected = 0;

    for_each_message(feed.data(), feed.size(), [&](const Event& ev) {
        ++counts[ev.msg_type];
        if (ev.type == EventType::StockDirectory) names[ev.locate] = ev.stock;
        if (ev.type == EventType::Other || ev.type == EventType::SystemEvent || ev.type == EventType::StockDirectory)
            return;
        if (!symbol.empty()) {
            auto it = names.find(ev.locate);
            if (it == names.end() || it->second != symbol) return;
        }
        (apply(books[ev.locate], ev) ? applied : rejected)++;
    });

    std::cout << "messages by type:";
    for (const auto& [t, n] : counts) std::cout << "  " << t << "=" << n;
    std::cout << "\napplied " << applied << " order events, " << rejected << " rejected\n\n";
    for (const auto& [locate, book] : books) {
        const auto bb = book.best_bid(), ba = book.best_ask();
        if (!bb || !ba) continue;
        const std::string nm = names.count(locate) ? names[locate] : "locate " + std::to_string(locate);
        std::cout << nm << ": bid " << dollars(*bb) << "  ask " << dollars(*ba) << "  orders " << book.order_count()
                  << "\n";
    }
    return 0;
}

int cmd_backtest(const Args& a) {
    const auto feed = read_file(a.get("file"));
    const std::string symbol = a.get("symbol");
    const std::int32_t locate = find_locate(feed, symbol);
    if (locate < 0) {
        std::cerr << "symbol " << symbol << " not found in the feed\n";
        return 1;
    }
    OrderBook book(1 << 20);
    Backtester bt;
    for_each_message(feed.data(), feed.size(), [&](const Event& ev) {
        if (ev.locate != locate || ev.type == EventType::Other || ev.type == EventType::StockDirectory ||
            ev.type == EventType::SystemEvent)
            return;
        bt.on_event(ev, book);
    });
    std::cout << "Market-making backtest on " << symbol << "\n\n";
    print_result(bt.finish(book));
    return 0;
}

int cmd_demo() {
    SynthConfig cfg;
    cfg.events = 400'000;
    const auto feed = generate_feed(cfg);
    OrderBook book;
    Backtester bt;
    for_each_message(feed.data(), feed.size(), [&](const Event& ev) {
        if (ev.type == EventType::Other || ev.type == EventType::StockDirectory || ev.type == EventType::SystemEvent)
            return;
        bt.on_event(ev, book);
    });
    std::cout << "Market-making backtest on a synthetic feed (" << cfg.events << " order events)\n\n";
    print_result(bt.finish(book));
    return 0;
}

int cmd_bench(const Args& a) {
    std::vector<std::uint8_t> feed;
    std::int32_t locate = -1;
    if (!a.get("file").empty()) {
        feed = read_file(a.get("file"));
        locate = find_locate(feed, a.get("symbol"));
        if (locate < 0) {
            std::cerr << "need --symbol present in the feed\n";
            return 1;
        }
    } else {
        SynthConfig cfg;
        cfg.events = a.num("events", 2'000'000);
        cfg.seed = static_cast<unsigned>(a.num("seed", 1));
        feed = generate_feed(cfg);
    }

    // Index message boundaries first so the timed loop measures parsing and book updates only.
    std::vector<std::pair<std::size_t, std::uint16_t>> msgs;
    for (std::size_t pos = 0; pos + 2 <= feed.size();) {
        const std::size_t len = (static_cast<std::size_t>(feed[pos]) << 8) | feed[pos + 1];
        if (pos + 2 + len > feed.size()) break;
        msgs.emplace_back(pos + 2, static_cast<std::uint16_t>(len));
        pos += 2 + len;
    }

    constexpr std::size_t kBatch = 32;
    using clock = std::chrono::steady_clock;
    std::vector<double> batch_ns;
    batch_ns.reserve(msgs.size() / kBatch + 1);

    OrderBook book(1 << 20);
    std::uint64_t applied = 0;
    Event ev;
    const auto t0 = clock::now();
    for (std::size_t i = 0; i < msgs.size(); i += kBatch) {
        const std::size_t end = std::min(msgs.size(), i + kBatch);
        const auto b0 = clock::now();
        for (std::size_t j = i; j < end; ++j) {
            if (!parse_message(feed.data() + msgs[j].first, msgs[j].second, ev)) continue;
            if (locate >= 0 && ev.locate != locate) continue;
            if (ev.type == EventType::Other || ev.type == EventType::SystemEvent ||
                ev.type == EventType::StockDirectory)
                continue;
            applied += apply(book, ev) ? 1 : 0;
        }
        const auto b1 = clock::now();
        batch_ns.push_back(std::chrono::duration<double, std::nano>(b1 - b0).count() / static_cast<double>(end - i));
    }
    const auto t1 = clock::now();
    const double secs = std::chrono::duration<double>(t1 - t0).count();

    std::sort(batch_ns.begin(), batch_ns.end());
    auto pct = [&](double p) { return batch_ns[static_cast<std::size_t>(p * (batch_ns.size() - 1))]; };

    std::printf("messages parsed        %zu\n", msgs.size());
    std::printf("order events applied   %llu\n", static_cast<unsigned long long>(applied));
    std::printf("elapsed                %.3f s\n", secs);
    std::printf("throughput             %.2f M messages/s\n", static_cast<double>(msgs.size()) / secs / 1e6);
    std::printf("per-message latency    p50 %.0f ns   p99 %.0f ns   p99.9 %.0f ns   (averaged over batches of %zu)\n",
                pct(0.50), pct(0.99), pct(0.999), kBatch);
    std::printf("final book             bid %s  ask %s  orders %zu\n",
                book.best_bid() ? dollars(*book.best_bid()).c_str() : "-",
                book.best_ask() ? dollars(*book.best_ask()).c_str() : "-", book.order_count());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const Args a = parse_args(argc, argv);
    try {
        if (a.command == "gen") return cmd_gen(a);
        if (a.command == "replay") return cmd_replay(a);
        if (a.command == "backtest") return cmd_backtest(a);
        if (a.command == "bench") return cmd_bench(a);
        if (a.command == "demo") return cmd_demo();
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    std::cerr << "usage: tickbook gen|replay|backtest|bench|demo  (see README)\n";
    return 2;
}
