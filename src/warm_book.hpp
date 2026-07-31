// warm_book.hpp - a resting-book SNAPSHOT: one symbol's book at one instant,
// written as text and reloadable into any lob::Book.
//
// WHY THIS EXISTS (2026-07-31 cold-start control). The shim resolves
// PRICE_OFF as an occupied-LEVEL INDEX, so an add at index k needs k+1
// occupied levels on that side to already exist. A generation seeded with
// two resting orders has ONE occupied level per side, so only indices 0 and
// -1 can ever resolve and every deeper add rejects UnknownReference - which
// drains the book, after which nothing resolves at all (an empty book is an
// absorbing state for this shim). That is a property of the INITIALIZATION,
// not of the model: the REAL token stream, which is by construction what a
// perfect model emits, dies on that seeding within 3 tuples. Warm-starting
// gives generation the same courtesy the CST null already had from its
// 08:00-09:30 warm-up.
//
// The snapshot records every resting order in book order: levels best-first
// per side (bids then asks), FIFO order within a level. Re-adding them in
// that order reproduces the level structure and the FIFO queues exactly.
// Only the order REFS differ, and refs are dropped by the tokenizer and are
// unobservable through the shim (which cancels FIFO heads by side+price).
// Warm refs live at kWarmRefBase so they cannot collide with the refs the
// Adapter allocates from 1.
#ifndef EXCHANGE_WARM_BOOK_HPP
#define EXCHANGE_WARM_BOOK_HPP

#include <cstdint>
#include <cstdio>
#include <istream>
#include <ostream>
#include <string>
#include <vector>

#include "book.hpp"

namespace warm {

constexpr uint64_t kWarmRefBase = 1ull << 60;
constexpr const char* kMagic = "# oftk-warm-book v1";

struct Row {
    lob::Side side = lob::Side::Buy;
    uint32_t price = 0;
    uint32_t shares = 0;
};

struct Snapshot {
    std::string ticker;
    std::string day;
    uint64_t ts_ns = 0;
    std::vector<Row> rows;  // book order: bids best-first, then asks
};

// Serialize `b` (every resting order, in book order).
inline bool write(std::ostream& os, const std::string& ticker,
                  const std::string& day, uint64_t ts_ns, const lob::Book& b,
                  std::string* err) {
    os << kMagic << " ticker=" << ticker << " day=" << day
       << " ts_ns=" << ts_ns << " orders=" << b.open_orders() << '\n';
    os << "side,price,shares\n";
    size_t n = 0;
    b.for_each_level([&](lob::Side s, const lob::Level& lv) {
        for (const lob::Order* o = lv.head; o; o = o->next) {
            os << (s == lob::Side::Buy ? 'B' : 'S') << ',' << o->price << ','
               << o->shares << '\n';
            ++n;
        }
        return true;
    });
    os.flush();
    if (!os) {
        if (err) *err = "write failed";
        return false;
    }
    if (n != b.open_orders()) {
        if (err) *err = "level walk missed orders (book corrupt)";
        return false;
    }
    return true;
}

inline bool read(std::istream& is, Snapshot& out, std::string* err) {
    auto fail = [&](const std::string& what) {
        if (err) *err = what;
        return false;
    };
    std::string line;
    if (!std::getline(is, line) || line.rfind(kMagic, 0) != 0)
        return fail("not a warm-book snapshot (bad magic)");
    auto field = [&](const char* key, std::string& v) {
        const std::string k = std::string(" ") + key + "=";
        size_t p = line.find(k);
        if (p == std::string::npos) return;
        p += k.size();
        size_t e = line.find(' ', p);
        v = line.substr(p, e == std::string::npos ? e : e - p);
    };
    std::string ts;
    field("ticker", out.ticker);
    field("day", out.day);
    field("ts_ns", ts);
    out.ts_ns = ts.empty() ? 0 : std::strtoull(ts.c_str(), nullptr, 10);
    if (!std::getline(is, line) || line != "side,price,shares")
        return fail("missing column header");
    while (std::getline(is, line)) {
        if (line.empty()) continue;
        char sd = 0;
        unsigned long long px = 0, sh = 0;
        if (std::sscanf(line.c_str(), "%c,%llu,%llu", &sd, &px, &sh) != 3 ||
            (sd != 'B' && sd != 'S') || sh == 0 || px == 0 ||
            px > 0xFFFFFFFFull || sh > 0xFFFFFFFFull)
            return fail("malformed row: " + line);
        out.rows.push_back({sd == 'B' ? lob::Side::Buy : lob::Side::Sell,
                            uint32_t(px), uint32_t(sh)});
    }
    return true;
}

// Add every row into `b` in snapshot order. `b` must be empty; any engine
// rejection (a crossed or malformed snapshot) is fatal and reported - a
// warm start that silently dropped orders would be a worse initialization
// than the cold one it replaces.
inline bool apply(const Snapshot& s, lob::Book& b, std::string* err) {
    if (b.open_orders() != 0) {
        if (err) *err = "warm start needs an empty book";
        return false;
    }
    uint64_t ref = kWarmRefBase;
    for (size_t i = 0; i < s.rows.size(); ++i) {
        const Row& r = s.rows[i];
        const lob::Result res = b.add(ref++, r.side, r.shares, r.price);
        if (res != lob::Result::Ok) {
            if (err)
                *err = "row " + std::to_string(i) + " rejected: " +
                       lob::to_string(res);
            return false;
        }
    }
    return true;
}

}  // namespace warm

#endif  // EXCHANGE_WARM_BOOK_HPP
