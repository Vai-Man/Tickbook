# Tickbook

A C++17 limit order book and matching engine that replays NASDAQ TotalView-ITCH 5.0 market data, with a market-making backtester that models queue position. Written to look at the microstructure questions a quant developer cares about: how fast can a book be rebuilt from the raw feed, and what does a passive strategy actually earn once fills depend on queue position and latency.

## What it does

- **ITCH 5.0 parser.** Decodes the order-flow messages (`A`, `F`, `E`, `C`, `X`, `D`, `U`) plus system events and the stock directory. Messages are read straight from the length-prefixed byte stream.
- **Order book.** Price-time priority per instrument. Orders sit in a pooled array linked into a FIFO list per price level, with a hash map from order reference to slot. Add, cancel, execute, delete and replace.
- **Matching engine.** Limit and market orders with price-time priority, partial fills, trades at the resting order's price.
- **Fill model.** A simulated passive order joins the back of the queue and fills only after enough volume trades through the orders ahead of it. Cancels ahead of it move it forward in proportion to how much of the level was cancelled.
- **Market-making backtest.** An Avellaneda-Stoikov quoter with an inventory limit and an order-latency model. Reports PnL split into spread capture and inventory, and the adverse selection of its fills.
- **Synthetic feed generator.** Writes valid ITCH 5.0 so the whole pipeline runs without a data download.

## Build

```bash
cmake -S . -B build
cmake --build build
./build/tickbook_tests
```

No dependencies beyond a C++17 compiler.

## Usage

```bash
# Run the backtest on a generated feed
./build/tickbook demo

# Generate a feed, then replay it
./build/tickbook gen --out synth.itch --events 1000000 --seed 7
./build/tickbook replay --file synth.itch --symbol TICK

# Backtest a symbol from a real feed
./build/tickbook backtest --file 01302019.NASDAQ_ITCH50 --symbol AAPL

# Time parsing and book updates
./build/tickbook bench --events 2000000
./build/tickbook bench --file 01302019.NASDAQ_ITCH50 --symbol AAPL
```

Sample NASDAQ ITCH 5.0 files are published at <https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/>. They are large (several GB uncompressed), so they are not included here. Decompress the `.gz` first.

## How the backtest works

1. Every ITCH event for the chosen instrument is passed through the backtester in order.
2. Orders the strategy sent earlier reach the book after the configured latency.
3. Before an event is applied to the book, the fill model checks whether it executes or removes liquidity at a price where the strategy has a resting order.
4. After the event, the strategy updates its volatility estimate and, if the midpoint has moved or the requote interval has passed, schedules new quotes.

Quotes are post-only and never cross the book. By default they are held at or inside the touch (`max_ticks_from_touch`), because on a one-tick-wide book wider quotes are never reached.

Results are reported in dollars: total PnL, spread capture (price versus midpoint at the time of each fill), inventory PnL (the remainder), and adverse selection (how far the midpoint moved against each fill within one second).

## Layout

| Path | Contents |
|---|---|
| `include/tickbook/itch.h`, `src/itch.cpp` | ITCH parsing and encoding |
| `include/tickbook/order_book.h`, `src/order_book.cpp` | Order book and `apply()` for feed events |
| `include/tickbook/matching_engine.h`, `src/matching_engine.cpp` | Price-time matching |
| `include/tickbook/fill_model.h`, `src/fill_model.cpp` | Queue-position fills |
| `include/tickbook/backtest.h`, `src/backtest.cpp` | Strategy, accounting, metrics |
| `include/tickbook/synth.h`, `src/synth.cpp` | Synthetic feed generator |
| `src/main.cpp` | Command line tool, including the benchmark |
| `tests/test_tickbook.cpp` | Tests for each component |

## Benchmark notes

`bench` indexes the message boundaries first and then times parsing plus book updates only. It reports throughput and per-message latency averaged over batches of 32 messages (a per-message clock read would cost more than the work it measures). Numbers depend on the machine and on the feed: the synthetic feed has a shallower, tamer book than a real trading day, so measure on real data before drawing conclusions.

## Limitations

- The fill model assumes cancels are spread evenly across the queue. Real queues are not uniform, so passive fills are approximate.
- The strategy does not affect the replayed market: its orders never change what other participants do, and its fills do not remove liquidity from the historical book.
- The backtest handles one instrument at a time and trades only passively.
- Replay supports the order-flow messages above; trade, cross and imbalance messages are skipped.
- Single-threaded.

## License

MIT
