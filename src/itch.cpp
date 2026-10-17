#include "tickbook/itch.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace tickbook {
namespace {

// ---- big-endian readers
inline std::uint16_t rd16(const std::uint8_t* p) { return static_cast<std::uint16_t>((p[0] << 8) | p[1]); }
inline std::uint32_t rd32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}
inline std::uint64_t rd48(const std::uint8_t* p) {
    return (static_cast<std::uint64_t>(p[0]) << 40) | (static_cast<std::uint64_t>(p[1]) << 32) |
           (static_cast<std::uint64_t>(p[2]) << 24) | (static_cast<std::uint64_t>(p[3]) << 16) |
           (static_cast<std::uint64_t>(p[4]) << 8) | p[5];
}
inline std::uint64_t rd64(const std::uint8_t* p) {
    return (static_cast<std::uint64_t>(rd32(p)) << 32) | rd32(p + 4);
}

// ---- big-endian writers
inline void wr8(std::vector<std::uint8_t>& o, std::uint8_t v) { o.push_back(v); }
inline void wr16(std::vector<std::uint8_t>& o, std::uint16_t v) {
    o.push_back(static_cast<std::uint8_t>(v >> 8));
    o.push_back(static_cast<std::uint8_t>(v));
}
inline void wr32(std::vector<std::uint8_t>& o, std::uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) o.push_back(static_cast<std::uint8_t>(v >> s));
}
inline void wr48(std::vector<std::uint8_t>& o, std::uint64_t v) {
    for (int s = 40; s >= 0; s -= 8) o.push_back(static_cast<std::uint8_t>(v >> s));
}
inline void wr64(std::vector<std::uint8_t>& o, std::uint64_t v) {
    for (int s = 56; s >= 0; s -= 8) o.push_back(static_cast<std::uint8_t>(v >> s));
}
inline void wr_stock(std::vector<std::uint8_t>& o, const char* stock) {
    char buf[8];
    std::memset(buf, ' ', sizeof buf);
    std::size_t n = std::strlen(stock);
    std::memcpy(buf, stock, n > 8 ? 8 : n);
    for (char c : buf) o.push_back(static_cast<std::uint8_t>(c));
}

// Each message is written as: length(2) | type(1) | locate(2) | tracking(2) | timestamp(6) | body
void begin(std::vector<std::uint8_t>& o, std::size_t total_len, char type, std::uint16_t locate, Nanos ts,
           std::size_t* length_pos) {
    *length_pos = o.size();
    wr16(o, static_cast<std::uint16_t>(total_len));
    wr8(o, static_cast<std::uint8_t>(type));
    wr16(o, locate);
    wr16(o, 0);  // tracking number
    wr48(o, ts);
}

void trim_stock(const std::uint8_t* p, char out[9]) {
    std::memcpy(out, p, 8);
    out[8] = 0;
    for (int i = 7; i >= 0 && out[i] == ' '; --i) out[i] = 0;
}

}  // namespace

bool parse_message(const std::uint8_t* d, std::size_t len, Event& ev) {
    ev = Event{};
    if (len < 11) return false;  // common header is 11 bytes
    ev.msg_type = static_cast<char>(d[0]);
    ev.locate = rd16(d + 1);
    ev.ts = rd48(d + 5);

    switch (ev.msg_type) {
        case 'S':
            if (len < 12) return false;
            ev.type = EventType::SystemEvent;
            ev.event_code = static_cast<char>(d[11]);
            return true;
        case 'R':
            if (len < 19) return false;
            ev.type = EventType::StockDirectory;
            trim_stock(d + 11, ev.stock);
            return true;
        case 'A':
        case 'F':
            if (len < 36) return false;
            ev.type = EventType::Add;
            ev.ref = rd64(d + 11);
            ev.side = d[19] == 'B' ? Side::Buy : Side::Sell;
            ev.shares = rd32(d + 20);
            trim_stock(d + 24, ev.stock);
            ev.price = static_cast<Price>(rd32(d + 32));
            return true;
        case 'E':
            if (len < 31) return false;
            ev.type = EventType::Execute;
            ev.ref = rd64(d + 11);
            ev.shares = rd32(d + 19);
            return true;
        case 'C':
            if (len < 36) return false;
            ev.type = EventType::ExecuteWithPrice;
            ev.ref = rd64(d + 11);
            ev.shares = rd32(d + 19);
            ev.price = static_cast<Price>(rd32(d + 32));
            return true;
        case 'X':
            if (len < 23) return false;
            ev.type = EventType::Cancel;
            ev.ref = rd64(d + 11);
            ev.shares = rd32(d + 19);
            return true;
        case 'D':
            if (len < 19) return false;
            ev.type = EventType::Delete;
            ev.ref = rd64(d + 11);
            return true;
        case 'U':
            if (len < 35) return false;
            ev.type = EventType::Replace;
            ev.ref = rd64(d + 11);
            ev.new_ref = rd64(d + 19);
            ev.shares = rd32(d + 27);
            ev.price = static_cast<Price>(rd32(d + 31));
            return true;
        default:
            ev.type = EventType::Other;
            return true;
    }
}

std::size_t for_each_message(const std::uint8_t* data, std::size_t size,
                             const std::function<void(const Event&)>& handler) {
    std::size_t pos = 0, count = 0;
    Event ev;
    while (pos + 2 <= size) {
        const std::size_t len = rd16(data + pos);
        if (pos + 2 + len > size) break;  // truncated trailing message
        if (parse_message(data + pos + 2, len, ev)) {
            handler(ev);
            ++count;
        }
        pos += 2 + len;
    }
    return count;
}

std::vector<std::uint8_t> read_file(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) throw std::runtime_error("cannot open " + path);
    std::vector<std::uint8_t> buf;
    std::uint8_t chunk[1 << 16];
    std::size_t n;
    while ((n = std::fread(chunk, 1, sizeof chunk, f)) > 0) buf.insert(buf.end(), chunk, chunk + n);
    std::fclose(f);
    return buf;
}

void encode_system_event(std::vector<std::uint8_t>& o, Nanos ts, char code) {
    std::size_t pos;
    begin(o, 12, 'S', 0, ts, &pos);
    wr8(o, static_cast<std::uint8_t>(code));
}

void encode_stock_directory(std::vector<std::uint8_t>& o, std::uint16_t locate, Nanos ts, const char* stock) {
    std::size_t pos;
    begin(o, 39, 'R', locate, ts, &pos);
    wr_stock(o, stock);
    for (int i = 0; i < 20; ++i) wr8(o, 0);  // remaining directory fields, unused here
}

void encode_add(std::vector<std::uint8_t>& o, std::uint16_t locate, Nanos ts, OrderRef ref, Side side,
                Qty shares, const char* stock, Price price) {
    std::size_t pos;
    begin(o, 36, 'A', locate, ts, &pos);
    wr64(o, ref);
    wr8(o, side == Side::Buy ? 'B' : 'S');
    wr32(o, shares);
    wr_stock(o, stock);
    wr32(o, static_cast<std::uint32_t>(price));
}

void encode_execute(std::vector<std::uint8_t>& o, std::uint16_t locate, Nanos ts, OrderRef ref, Qty shares,
                    std::uint64_t match_number) {
    std::size_t pos;
    begin(o, 31, 'E', locate, ts, &pos);
    wr64(o, ref);
    wr32(o, shares);
    wr64(o, match_number);
}

void encode_cancel(std::vector<std::uint8_t>& o, std::uint16_t locate, Nanos ts, OrderRef ref, Qty shares) {
    std::size_t pos;
    begin(o, 23, 'X', locate, ts, &pos);
    wr64(o, ref);
    wr32(o, shares);
}

void encode_delete(std::vector<std::uint8_t>& o, std::uint16_t locate, Nanos ts, OrderRef ref) {
    std::size_t pos;
    begin(o, 19, 'D', locate, ts, &pos);
    wr64(o, ref);
}

void encode_replace(std::vector<std::uint8_t>& o, std::uint16_t locate, Nanos ts, OrderRef ref,
                    OrderRef new_ref, Qty shares, Price price) {
    std::size_t pos;
    begin(o, 35, 'U', locate, ts, &pos);
    wr64(o, ref);
    wr64(o, new_ref);
    wr32(o, shares);
    wr32(o, static_cast<std::uint32_t>(price));
}

}  // namespace tickbook
