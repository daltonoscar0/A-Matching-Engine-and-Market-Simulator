#include "itch.hpp"

namespace itch {
namespace {

inline void put_header(uint8_t* p, char type, const Header& h) {
    p[0] = uint8_t(type);
    put_u16(p + 1, h.stock_locate);
    put_u16(p + 3, h.tracking);
    put_u48(p + 5, h.timestamp);
}
inline Header get_header(const uint8_t* p) {
    Header h;
    h.stock_locate = get_u16(p + 1);
    h.tracking     = get_u16(p + 3);
    h.timestamp    = get_u48(p + 5);
    return h;
}
inline bool valid_side(char c) { return c == 'B' || c == 'S'; }

size_t enc_add(const AddOrder& m, uint8_t* p, bool mpid, const Mpid* attr) {
    put_header(p, mpid ? 'F' : 'A', m.h);
    put_u64(p + 11, m.order_ref);
    p[19] = uint8_t(m.side);
    put_u32(p + 20, m.shares);
    std::memcpy(p + 24, m.stock.data(), 8);
    put_u32(p + 32, m.price);
    if (mpid) { std::memcpy(p + 36, attr->data(), 4); return LEN_F; }
    return LEN_A;
}

}  // namespace

size_t encode(const Message& m, uint8_t* out) {
    struct V {
        uint8_t* p;
        size_t operator()(const AddOrder& m) const {
            return enc_add(m, p, false, nullptr);
        }
        size_t operator()(const AddOrderMpid& m) const {
            return enc_add(m.add, p, true, &m.attribution);
        }
        size_t operator()(const OrderExecuted& m) const {
            put_header(p, 'E', m.h);
            put_u64(p + 11, m.order_ref);
            put_u32(p + 19, m.shares);
            put_u64(p + 23, m.match_num);
            return LEN_E;
        }
        size_t operator()(const OrderExecutedPrice& m) const {
            put_header(p, 'C', m.exec.h);
            put_u64(p + 11, m.exec.order_ref);
            put_u32(p + 19, m.exec.shares);
            put_u64(p + 23, m.exec.match_num);
            p[31] = uint8_t(m.printable);
            put_u32(p + 32, m.price);
            return LEN_C;
        }
        size_t operator()(const OrderCancel& m) const {
            put_header(p, 'X', m.h);
            put_u64(p + 11, m.order_ref);
            put_u32(p + 19, m.shares);
            return LEN_X;
        }
        size_t operator()(const OrderDelete& m) const {
            put_header(p, 'D', m.h);
            put_u64(p + 11, m.order_ref);
            return LEN_D;
        }
        size_t operator()(const OrderReplace& m) const {
            put_header(p, 'U', m.h);
            put_u64(p + 11, m.orig_order_ref);
            put_u64(p + 19, m.new_order_ref);
            put_u32(p + 27, m.shares);
            put_u32(p + 31, m.price);
            return LEN_U;
        }
    };
    return std::visit(V{out}, m);
}

void encode_framed(const Message& m, std::vector<uint8_t>& buf) {
    uint8_t body[MAX_MSG_LEN];
    size_t n = encode(m, body);
    size_t at = buf.size();
    buf.resize(at + 2 + n);
    put_u16(buf.data() + at, uint16_t(n));
    std::memcpy(buf.data() + at + 2, body, n);
}

std::optional<Message> decode(const uint8_t* p, size_t len) {
    if (len == 0) return std::nullopt;
    switch (char(p[0])) {
        case 'A': {
            if (len != LEN_A) return std::nullopt;
            AddOrder m;
            m.h         = get_header(p);
            m.order_ref = get_u64(p + 11);
            m.side      = char(p[19]);
            if (!valid_side(m.side)) return std::nullopt;
            m.shares    = get_u32(p + 20);
            std::memcpy(m.stock.data(), p + 24, 8);
            m.price     = get_u32(p + 32);
            return m;
        }
        case 'F': {
            if (len != LEN_F) return std::nullopt;
            AddOrderMpid m;
            m.add.h         = get_header(p);
            m.add.order_ref = get_u64(p + 11);
            m.add.side      = char(p[19]);
            if (!valid_side(m.add.side)) return std::nullopt;
            m.add.shares    = get_u32(p + 20);
            std::memcpy(m.add.stock.data(), p + 24, 8);
            m.add.price     = get_u32(p + 32);
            std::memcpy(m.attribution.data(), p + 36, 4);
            return m;
        }
        case 'E': {
            if (len != LEN_E) return std::nullopt;
            OrderExecuted m;
            m.h         = get_header(p);
            m.order_ref = get_u64(p + 11);
            m.shares    = get_u32(p + 19);
            m.match_num = get_u64(p + 23);
            return m;
        }
        case 'C': {
            if (len != LEN_C) return std::nullopt;
            OrderExecutedPrice m;
            m.exec.h         = get_header(p);
            m.exec.order_ref = get_u64(p + 11);
            m.exec.shares    = get_u32(p + 19);
            m.exec.match_num = get_u64(p + 23);
            m.printable      = char(p[31]);
            if (m.printable != 'Y' && m.printable != 'N') return std::nullopt;
            m.price          = get_u32(p + 32);
            return m;
        }
        case 'X': {
            if (len != LEN_X) return std::nullopt;
            OrderCancel m;
            m.h         = get_header(p);
            m.order_ref = get_u64(p + 11);
            m.shares    = get_u32(p + 19);
            return m;
        }
        case 'D': {
            if (len != LEN_D) return std::nullopt;
            OrderDelete m;
            m.h         = get_header(p);
            m.order_ref = get_u64(p + 11);
            return m;
        }
        case 'U': {
            if (len != LEN_U) return std::nullopt;
            OrderReplace m;
            m.h              = get_header(p);
            m.orig_order_ref = get_u64(p + 11);
            m.new_order_ref  = get_u64(p + 19);
            m.shares         = get_u32(p + 27);
            m.price          = get_u32(p + 31);
            return m;
        }
        default:
            return std::nullopt;
    }
}

std::optional<Message> FrameReader::next() {
    if (pos == size) return std::nullopt;          // clean EOF
    if (size - pos < 2) { error = true; return std::nullopt; }
    uint16_t len = get_u16(data + pos);
    if (size - pos - 2 < len) { error = true; return std::nullopt; }
    auto m = decode(data + pos + 2, len);
    if (!m) { error = true; return std::nullopt; }
    pos += 2 + len;
    return m;
}

}  // namespace itch
