// itch.hpp - ITCH 5.0 message structs + binary codec (subset: A, F, E, C, X, D, U)
// Wire format: big-endian, per NASDAQ TotalView-ITCH 5.0. File framing follows
// the standard BinaryFILE convention: each message is preceded by a 2-byte
// big-endian length field.
#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <variant>
#include <vector>

namespace itch {

// ---------------------------------------------------------------- byte order
inline void put_u16(uint8_t* p, uint16_t v) {
    p[0] = uint8_t(v >> 8); p[1] = uint8_t(v);
}
inline void put_u32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);  p[3] = uint8_t(v);
}
inline void put_u48(uint8_t* p, uint64_t v) {  // low 48 bits
    p[0] = uint8_t(v >> 40); p[1] = uint8_t(v >> 32); p[2] = uint8_t(v >> 24);
    p[3] = uint8_t(v >> 16); p[4] = uint8_t(v >> 8);  p[5] = uint8_t(v);
}
inline void put_u64(uint8_t* p, uint64_t v) {
    put_u32(p, uint32_t(v >> 32)); put_u32(p + 4, uint32_t(v));
}
inline uint16_t get_u16(const uint8_t* p) {
    return uint16_t(p[0]) << 8 | p[1];
}
inline uint32_t get_u32(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 |
           uint32_t(p[2]) << 8  | uint32_t(p[3]);
}
inline uint64_t get_u48(const uint8_t* p) {
    return uint64_t(get_u16(p)) << 32 | get_u32(p + 2);
}
inline uint64_t get_u64(const uint8_t* p) {
    return uint64_t(get_u32(p)) << 32 | get_u32(p + 4);
}

// ---------------------------------------------------------------- messages
// Common 10-byte header after the type byte:
//   stock_locate(2) tracking(2) timestamp(6, ns since midnight, 48-bit)
struct Header {
    uint16_t stock_locate = 0;
    uint16_t tracking     = 0;
    uint64_t timestamp    = 0;  // only low 48 bits are encoded
};

using Stock = std::array<char, 8>;  // right-padded with spaces
using Mpid  = std::array<char, 4>;

struct AddOrder {                    // 'A', 36 bytes
    Header   h;
    uint64_t order_ref = 0;
    char     side      = 'B';        // 'B' or 'S'
    uint32_t shares    = 0;
    Stock    stock     = {' ',' ',' ',' ',' ',' ',' ',' '};
    uint32_t price     = 0;          // fixed-point, 4 implied decimals
};

struct AddOrderMpid {                // 'F', 40 bytes = AddOrder + attribution
    AddOrder add;
    Mpid     attribution = {' ',' ',' ',' '};
};

struct OrderExecuted {               // 'E', 31 bytes
    Header   h;
    uint64_t order_ref = 0;
    uint32_t shares    = 0;
    uint64_t match_num = 0;
};

struct OrderExecutedPrice {          // 'C', 36 bytes = E + printable + price
    OrderExecuted exec;
    char     printable = 'Y';
    uint32_t price     = 0;
};

struct OrderCancel {                 // 'X', 23 bytes (partial cancel)
    Header   h;
    uint64_t order_ref = 0;
    uint32_t shares    = 0;          // shares being removed
};

struct OrderDelete {                 // 'D', 19 bytes
    Header   h;
    uint64_t order_ref = 0;
};

struct OrderReplace {                // 'U', 35 bytes
    Header   h;
    uint64_t orig_order_ref = 0;
    uint64_t new_order_ref  = 0;
    uint32_t shares         = 0;
    uint32_t price          = 0;
};

using Message = std::variant<AddOrder, AddOrderMpid, OrderExecuted,
                             OrderExecutedPrice, OrderCancel, OrderDelete,
                             OrderReplace>;

constexpr size_t LEN_A = 36, LEN_F = 40, LEN_E = 31, LEN_C = 36,
                 LEN_X = 23, LEN_D = 19, LEN_U = 35;
constexpr size_t MAX_MSG_LEN = LEN_F;

// True for the 7 book-affecting types this codec decodes. A real ITCH day
// carries ~20 types; everything else is metadata/trade-report traffic that
// FrameReader skips (counted) rather than decodes.
inline bool is_book_type(char t) {
    switch (t) {
        case 'A': case 'F': case 'E': case 'C': case 'X': case 'D': case 'U':
            return true;
        default:
            return false;
    }
}

inline char type_of(const Message& m) {
    struct V {
        char operator()(const AddOrder&) const           { return 'A'; }
        char operator()(const AddOrderMpid&) const       { return 'F'; }
        char operator()(const OrderExecuted&) const      { return 'E'; }
        char operator()(const OrderExecutedPrice&) const { return 'C'; }
        char operator()(const OrderCancel&) const        { return 'X'; }
        char operator()(const OrderDelete&) const        { return 'D'; }
        char operator()(const OrderReplace&) const       { return 'U'; }
    };
    return std::visit(V{}, m);
}

// ------------------------------------------------------------------ encode
// Encodes the message body (type byte + payload, no length prefix).
// Returns number of bytes written. `out` must hold >= MAX_MSG_LEN bytes.
size_t encode(const Message& m, uint8_t* out);

// Appends 2-byte big-endian length prefix + body to `buf` (file framing).
void encode_framed(const Message& m, std::vector<uint8_t>& buf);

// ------------------------------------------------------------------ decode
// Decodes one message body of exactly `len` bytes. nullopt on unknown type,
// bad enum value, or length mismatch.
std::optional<Message> decode(const uint8_t* p, size_t len);

// ---------------------------------------------------------- stock directory
// 'R' stock directory, 39 bytes on the wire. Not part of Message: it names
// instruments (locate -> ticker) instead of mutating a book, so it is parsed
// on demand rather than decoded in the hot path. Subset of fields we use.
struct StockDirectory {
    Header   h;                       // h.stock_locate is the locate code
    Stock    stock = {' ',' ',' ',' ',' ',' ',' ',' '};
    char     market_category  = ' ';
    char     financial_status = ' ';
    uint32_t round_lot        = 0;
    char     round_lots_only  = ' ';
    char     issue_class      = ' ';
};
constexpr size_t LEN_R = 39;
// Parses one 'R' message body. nullopt on type or length mismatch.
std::optional<StockDirectory> parse_stock_directory(const uint8_t* p,
                                                    size_t len);

// Streaming reader over a framed buffer. next() returns the next decoded
// book-type message and transparently skips frames whose type byte is not
// one of the 7 we implement, counting them per type byte in `skips`.
//
// Two failure modes, deliberately NOT collapsed:
//   unknown type, sane length  -> skip and continue (expected on real data)
//   malformed frame            -> fatal: zero length, length past the end
//     of the buffer, or a KNOWN type that fails decode (wrong length / bad
//     enum). That is how a desynced stream shows up; skipping past it would
//     let us "successfully" replay garbage.
// Returns nullopt at clean EOF; `error` distinguishes EOF from fatal.
struct FrameReader {
    const uint8_t* data;
    size_t size;
    size_t pos   = 0;
    bool   error = false;
    uint64_t skipped = 0;                  // total frames skipped
    std::array<uint64_t, 256> skips{};     // skipped frames per type byte
    std::optional<Message> next();
};

}  // namespace itch
