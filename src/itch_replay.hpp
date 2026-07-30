// itch_replay.hpp - replay a framed real-ITCH buffer through a BookSet.
//
// The Phase 1 correctness contract for real exchange data: a correct book
// applying genuine ITCH produces ZERO rejects. Every reject is captured
// with enough context to diagnose which of our semantic assumptions broke;
// there is no tolerance and no allowlist. Shared by bench/replay_itch.cpp
// and tests/test_itch_replay.cpp so the tool and the gate run the same code.
#pragma once
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "bookset.hpp"
#include "itch.hpp"

namespace lob {

inline size_t body_len_of(char t) {
    switch (t) {
        case 'A': return itch::LEN_A;
        case 'F': return itch::LEN_F;
        case 'E': return itch::LEN_E;
        case 'C': return itch::LEN_C;
        case 'X': return itch::LEN_X;
        case 'D': return itch::LEN_D;
        case 'U': return itch::LEN_U;
        default:  return 0;
    }
}

// The order an execute/cancel/delete/replace refers to ('U': the original).
inline uint64_t ref_of(const itch::Message& m) {
    struct V {
        uint64_t operator()(const itch::AddOrder& x) const           { return x.order_ref; }
        uint64_t operator()(const itch::AddOrderMpid& x) const       { return x.add.order_ref; }
        uint64_t operator()(const itch::OrderExecuted& x) const      { return x.order_ref; }
        uint64_t operator()(const itch::OrderExecutedPrice& x) const { return x.exec.order_ref; }
        uint64_t operator()(const itch::OrderCancel& x) const        { return x.order_ref; }
        uint64_t operator()(const itch::OrderDelete& x) const        { return x.order_ref; }
        uint64_t operator()(const itch::OrderReplace& x) const       { return x.orig_order_ref; }
    };
    return std::visit(V{}, m);
}

// One-line human rendering of a decoded message, for diagnostics.
inline std::string describe(const itch::Message& m) {
    char b[192];
    struct V {
        char* b; size_t n;
        int operator()(const itch::AddOrder& x) const {
            return std::snprintf(b, n,
                "A loc=%u ts=%llu ref=%llu %c %u @ %u", x.h.stock_locate,
                (unsigned long long)x.h.timestamp, (unsigned long long)x.order_ref,
                x.side, x.shares, x.price);
        }
        int operator()(const itch::AddOrderMpid& x) const {
            return std::snprintf(b, n,
                "F loc=%u ts=%llu ref=%llu %c %u @ %u mpid=%.4s",
                x.add.h.stock_locate, (unsigned long long)x.add.h.timestamp,
                (unsigned long long)x.add.order_ref, x.add.side, x.add.shares,
                x.add.price, x.attribution.data());
        }
        int operator()(const itch::OrderExecuted& x) const {
            return std::snprintf(b, n,
                "E loc=%u ts=%llu ref=%llu exec %u match=%llu",
                x.h.stock_locate, (unsigned long long)x.h.timestamp,
                (unsigned long long)x.order_ref, x.shares,
                (unsigned long long)x.match_num);
        }
        int operator()(const itch::OrderExecutedPrice& x) const {
            return std::snprintf(b, n,
                "C loc=%u ts=%llu ref=%llu exec %u @ %u printable=%c",
                x.exec.h.stock_locate, (unsigned long long)x.exec.h.timestamp,
                (unsigned long long)x.exec.order_ref, x.exec.shares, x.price,
                x.printable);
        }
        int operator()(const itch::OrderCancel& x) const {
            return std::snprintf(b, n, "X loc=%u ts=%llu ref=%llu cancel %u",
                x.h.stock_locate, (unsigned long long)x.h.timestamp,
                (unsigned long long)x.order_ref, x.shares);
        }
        int operator()(const itch::OrderDelete& x) const {
            return std::snprintf(b, n, "D loc=%u ts=%llu ref=%llu",
                x.h.stock_locate, (unsigned long long)x.h.timestamp,
                (unsigned long long)x.order_ref);
        }
        int operator()(const itch::OrderReplace& x) const {
            return std::snprintf(b, n,
                "U loc=%u ts=%llu orig=%llu new=%llu %u @ %u",
                x.h.stock_locate, (unsigned long long)x.h.timestamp,
                (unsigned long long)x.orig_order_ref,
                (unsigned long long)x.new_order_ref, x.shares, x.price);
        }
    };
    std::visit(V{b, sizeof b}, m);
    return b;
}

// Scan every 'R' frame into a side table. The replayed file itself is the
// authoritative locate->ticker map for its day (self-consistency is the
// point; no external reference file). Locate 0 is reserved for system-wide
// messages and owns no book.
inline std::vector<itch::StockDirectory> scan_stock_directory(
        const uint8_t* data, size_t size, bool& framing_error) {
    std::vector<itch::StockDirectory> dir;
    framing_error = false;
    size_t pos = 0;
    while (pos != size) {
        if (size - pos < 2) { framing_error = true; break; }
        uint16_t len = itch::get_u16(data + pos);
        if (len == 0 || size - pos - 2 < len) { framing_error = true; break; }
        const uint8_t* body = data + pos + 2;
        if (char(body[0]) == 'R') {
            auto d = itch::parse_stock_directory(body, len);
            if (!d) { framing_error = true; break; }
            dir.push_back(*d);
        }
        pos += 2 + len;
    }
    return dir;
}

// Snapshot of one reject, taken at reject time (book state included).
struct RejectContext {
    uint64_t      msg_index   = 0;   // index among decoded book messages
    size_t        body_offset = 0;   // byte offset of the body in the buffer
    Result        result      = Result::Ok;
    uint16_t      locate      = 0;
    itch::Message msg;
    uint32_t best_bid = 0, best_ask = 0;
    size_t   open_orders = 0;
    bool     ref_found  = false;     // is ref_of(msg) resting right now?
    uint32_t ref_shares = 0, ref_price = 0;
    char     ref_side   = '?';
};

struct PerBook {
    uint64_t msgs = 0, rejects = 0;
    uint64_t peak_open = 0, peak_levels = 0;
};

struct ReplayStats {
    uint64_t decoded = 0, applied = 0, rejected = 0, skipped = 0;
    std::array<uint64_t, 256> decoded_by{};              // per book type
    std::array<uint64_t, 256> skips{};                   // per skipped type
    std::array<std::array<uint64_t, 8>, 256> rejects_by{};  // [type][Result]
    uint64_t peak_open_total = 0;    // peak simultaneous open orders, all books
    uint64_t books_touched   = 0;
    bool     stream_error     = false;
    size_t   stream_error_pos = 0;
    std::string invariant_failure;   // empty = clean
    std::vector<PerBook> per_book;   // indexed by locate
};

struct ReplayOptions {
    size_t max_messages     = size_t(-1);  // cap on decoded book messages
    bool   check_invariants = false;       // invariants_fast() per message
    size_t capture_rejects  = 20;
};

struct ReplayOutcome {
    ReplayStats stats;
    std::vector<RejectContext> reject_samples;
};

inline ReplayOutcome replay_itch(const uint8_t* data, size_t size,
                                 BookSet& set,
                                 const ReplayOptions& opt = {}) {
    ReplayOutcome out;
    ReplayStats& st = out.stats;
    st.per_book.resize(size_t(1) << 16);
    itch::FrameReader rd{data, size};
    uint64_t open_total = 0;
    while (st.decoded < opt.max_messages) {
        auto m = rd.next();
        if (!m) break;
        ++st.decoded;
        char type = itch::type_of(*m);
        ++st.decoded_by[uint8_t(type)];
        uint16_t loc = locate_of(*m);
        Book& b = set.book(loc);
        PerBook& pb = st.per_book[loc];
        ++pb.msgs;
        size_t open_before = b.open_orders();
        Result r = lob::apply(b, *m);
        if (r == Result::Ok) {
            ++st.applied;
            size_t open_after = b.open_orders();
            open_total += open_after;
            open_total -= open_before;
            if (open_total > st.peak_open_total) st.peak_open_total = open_total;
            if (open_after > pb.peak_open) pb.peak_open = open_after;
            size_t lv = b.bid_levels() + b.ask_levels();
            if (lv > pb.peak_levels) pb.peak_levels = lv;
        } else {
            ++st.rejected;
            ++pb.rejects;
            ++st.rejects_by[uint8_t(type)][size_t(r)];
            if (out.reject_samples.size() < opt.capture_rejects) {
                RejectContext c;
                c.msg_index   = st.decoded - 1;
                c.body_offset = rd.pos - body_len_of(type);
                c.result = r; c.locate = loc; c.msg = *m;
                c.best_bid = b.best_bid(); c.best_ask = b.best_ask();
                c.open_orders = b.open_orders();
                if (const Order* o = b.find(ref_of(*m))) {
                    c.ref_found  = true;
                    c.ref_shares = o->shares;
                    c.ref_price  = o->price;
                    c.ref_side   = o->side == Side::Buy ? 'B' : 'S';
                }
                out.reject_samples.push_back(c);
            }
        }
        if (opt.check_invariants && !b.invariants_fast() &&
            st.invariant_failure.empty()) {
            st.invariant_failure =
                "invariants_fast failed at msg " +
                std::to_string(st.decoded - 1) + " locate " +
                std::to_string(loc) + ": " + b.audit();
            break;
        }
    }
    st.skipped          = rd.skipped;
    st.skips            = rd.skips;
    st.stream_error     = rd.error;
    st.stream_error_pos = rd.pos;
    for (auto& pb : st.per_book) st.books_touched += pb.msgs != 0;
    return out;
}

}  // namespace lob
