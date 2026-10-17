#pragma once

#include <cstdint>

namespace tickbook {

using Price = std::int32_t;    // 1/10000 of a dollar, as in NASDAQ ITCH
using Qty = std::uint32_t;     // shares
using OrderRef = std::uint64_t;
using Nanos = std::uint64_t;   // nanoseconds since midnight

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };

inline Side opposite(Side s) { return s == Side::Buy ? Side::Sell : Side::Buy; }
inline int sign(Side s) { return s == Side::Buy ? +1 : -1; }

constexpr Price kTick = 100;  // $0.01 in price units

}  // namespace tickbook
