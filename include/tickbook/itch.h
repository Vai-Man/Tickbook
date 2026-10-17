// NASDAQ TotalView-ITCH 5.0 parsing and encoding.
//
// A feed file is a sequence of messages, each preceded by a 2-byte big-endian length.
// Only the message types that change an order book are decoded; everything else is
// reported as EventType::Other.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "tickbook/types.h"

namespace tickbook {

enum class EventType : std::uint8_t {
    SystemEvent,       // 'S'
    StockDirectory,    // 'R'
    Add,               // 'A' and 'F'
    Execute,           // 'E'
    ExecuteWithPrice,  // 'C'
    Cancel,            // 'X' partial cancel
    Delete,            // 'D'
    Replace,           // 'U'
    Other
};

struct Event {
    EventType type = EventType::Other;
    char msg_type = 0;
    std::uint16_t locate = 0;  // stock locate code: identifies the instrument within a day
    Nanos ts = 0;
    OrderRef ref = 0;          // order reference (original ref for Replace)
    OrderRef new_ref = 0;      // Replace only
    Side side = Side::Buy;
    Qty shares = 0;            // added / executed / cancelled / new shares
    Price price = 0;
    char stock[9] = {0};       // NUL-terminated symbol, trailing spaces removed (Add, StockDirectory)
    char event_code = 0;       // SystemEvent only
};

// Decode one message body (without the length prefix). Returns false if it is too short
// for its type. Unknown types decode as EventType::Other and return true.
bool parse_message(const std::uint8_t* data, std::size_t len, Event& ev);

// Walk a buffer of length-prefixed messages. Stops at a truncated trailing message.
// Returns the number of messages visited.
std::size_t for_each_message(const std::uint8_t* data, std::size_t size,
                             const std::function<void(const Event&)>& handler);

// Read a whole feed file into memory. Throws std::runtime_error if it cannot be opened.
std::vector<std::uint8_t> read_file(const std::string& path);

// ---- encoding (used by the synthetic feed generator and the tests)
void encode_system_event(std::vector<std::uint8_t>& out, Nanos ts, char event_code);
void encode_stock_directory(std::vector<std::uint8_t>& out, std::uint16_t locate, Nanos ts, const char* stock);
void encode_add(std::vector<std::uint8_t>& out, std::uint16_t locate, Nanos ts, OrderRef ref, Side side,
                Qty shares, const char* stock, Price price);
void encode_execute(std::vector<std::uint8_t>& out, std::uint16_t locate, Nanos ts, OrderRef ref, Qty shares,
                    std::uint64_t match_number);
void encode_cancel(std::vector<std::uint8_t>& out, std::uint16_t locate, Nanos ts, OrderRef ref, Qty shares);
void encode_delete(std::vector<std::uint8_t>& out, std::uint16_t locate, Nanos ts, OrderRef ref);
void encode_replace(std::vector<std::uint8_t>& out, std::uint16_t locate, Nanos ts, OrderRef ref,
                    OrderRef new_ref, Qty shares, Price price);

}  // namespace tickbook
