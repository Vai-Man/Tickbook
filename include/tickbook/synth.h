// A synthetic ITCH 5.0 feed for testing, benchmarking and demos.
//
// The generator keeps its own order book, so every message it writes is valid for the
// state the book is in at that point: executions hit the oldest order at the best price,
// cancels and replaces refer to live orders, and the book never crosses.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "tickbook/types.h"

namespace tickbook {

struct SynthConfig {
    std::uint64_t events = 100'000;       // order-flow messages to write (excluding the two header messages)
    unsigned seed = 1;
    std::uint16_t locate = 1;
    std::string symbol = "TICK";
    Price start_mid = 1'000'000;          // $100.00
    Nanos start_ts = 34'200'000'000'000;  // 09:30:00
    double mean_gap_ns = 20'000.0;        // mean time between messages
};

// Returns a length-prefixed ITCH byte stream, ready for for_each_message().
std::vector<std::uint8_t> generate_feed(const SynthConfig& cfg);

}  // namespace tickbook
