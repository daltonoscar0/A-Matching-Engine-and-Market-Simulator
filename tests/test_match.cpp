// Property tests for the matching path (Book::match + match_submit), plus
// THE STRONG TEST: run generated flow through match(), capture the full
// emitted message stream, replay that stream through the validated
// RECONSTRUCTION path (encode -> decode -> feed.hpp apply) into a fresh
// Book, and require the two books end byte-for-byte identical in state.
// A matching engine whose output our own reconstruction cannot reproduce
// is wrong; this catches emit bugs that per-match assertions miss.
//
// Per-match oracle: before every submit, the expected fill sequence is
// derived independently through the public const API (walk opposite levels
// best-first, FIFO within level, stop at the limit) and compared exactly.
// That pins best-price-first, FIFO, fill sizes, and resting-price fills in
// one shot.
//
// Note on message types: the captured stream carries A/E/X/D/U. 'C' never
// appears by design - every fill in this engine executes at the resting
// order's display price, and ITCH reserves 'C' for executions at a price
// different from display (see src/match.hpp).
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <random>
#include <sstream>
#include <vector>

#include "../src/book.hpp"
#include "../src/feed.hpp"
#include "../src/itch.hpp"
#include "../src/match.hpp"
#include "../third_party/catch.hpp"

namespace {

constexpr uint32_t TICK = 100;
constexpr uint32_t MID0 = 1'000'000;  // $100.0000

// Canonical rendering of full book state: ledger counters, then every level
// best-first with its FIFO chain. Two books with equal fingerprints are
// equal in state; the comparison is byte-for-byte on this string.
std::string fingerprint(const lob::Book& b) {
    std::ostringstream os;
    os << "added=" << b.shares_added() << " executed=" << b.shares_executed()
       << " canceled=" << b.shares_canceled()
       << " resting=" << b.shares_resting() << " open=" << b.open_orders()
       << "\n";
    b.for_each_level([&](lob::Side s, const lob::Level& lvl) {
        os << (s == lob::Side::Buy ? 'B' : 'S') << ' ' << lvl.price << " n="
           << lvl.order_count << " sh=" << lvl.total_shares << " [";
        for (const lob::Order* o = lvl.head; o; o = o->next)
            os << o->ref << ':' << o->shares << ' ';
        os << "]\n";
        return true;
    });
    return os.str();
}

// Independent oracle: expected fills for an aggressive order, from const
// queries only. Opposite levels best-first, head-to-tail within a level,
// stopping at the limit (unless market).
std::vector<lob::Fill> expected_fills(const lob::Book& b, lob::Side side,
                                      uint32_t shares, uint32_t limit,
                                      bool market) {
    std::vector<lob::Fill> exp;
    lob::Side opp = side == lob::Side::Buy ? lob::Side::Sell : lob::Side::Buy;
    uint32_t rem = shares;
    b.for_each_level([&](lob::Side s, const lob::Level& lvl) {
        if (s != opp) return true;
        if (rem == 0) return false;
        if (!market) {
            if (side == lob::Side::Buy ? lvl.price > limit
                                       : lvl.price < limit)
                return false;
        }
        for (const lob::Order* o = lvl.head; o && rem; o = o->next) {
            uint32_t f = rem < o->shares ? rem : o->shares;
            exp.push_back({o->ref, f, o->price});
            rem -= f;
        }
        return true;
    });
    return exp;
}

lob::MatchRequest req(uint64_t ref, lob::Side side, uint32_t shares,
                      uint32_t limit, bool market) {
    lob::MatchRequest r;
    r.locate = 1;
    r.timestamp = 34'200'000'000'000ULL + ref;
    r.ref = ref; r.side = side; r.shares = shares; r.limit = limit;
    r.market = market;
    r.stock = {'M','A','T','C','H',' ',' ',' '};
    return r;
}

}  // namespace

TEST_CASE("match: fills walk best price first, FIFO within a level") {
    lob::Book b;
    // Asks: 100.01 x [ref 1: 100, ref 2: 50], 100.02 x [ref 3: 200].
    REQUIRE(b.add(1, lob::Side::Sell, 100, MID0 + 1 * TICK) == lob::Result::Ok);
    REQUIRE(b.add(2, lob::Side::Sell,  50, MID0 + 1 * TICK) == lob::Result::Ok);
    REQUIRE(b.add(3, lob::Side::Sell, 200, MID0 + 2 * TICK) == lob::Result::Ok);

    std::vector<lob::Fill> fills;
    auto mo = b.match(10, lob::Side::Buy, 250, MID0 + 2 * TICK, false, fills);
    REQUIRE(mo.result == lob::Result::Ok);
    CHECK(mo.filled == 250);
    CHECK(mo.rested == 0);
    REQUIRE(fills.size() == 3);
    // Best price first...
    CHECK(fills[0].resting_ref == 1);       // oldest at 100.01
    CHECK(fills[0].shares == 100);
    CHECK(fills[1].resting_ref == 2);       // then FIFO successor
    CHECK(fills[1].shares == 50);
    // ...then the next level, partially.
    CHECK(fills[2].resting_ref == 3);
    CHECK(fills[2].shares == 100);
    CHECK(b.find(3)->shares == 100);        // partial fill left resting
    CHECK(b.best_ask() == MID0 + 2 * TICK);
    CHECK(!b.crossed());
}

TEST_CASE("match: fills execute at the resting price, not the aggressor's") {
    lob::Book b;
    REQUIRE(b.add(1, lob::Side::Sell, 100, MID0 + 1 * TICK) == lob::Result::Ok);
    std::vector<lob::Fill> fills;
    // Buy limit 10 ticks through the ask: price improvement to the aggressor.
    auto mo = b.match(10, lob::Side::Buy, 100, MID0 + 10 * TICK, false, fills);
    REQUIRE(mo.result == lob::Result::Ok);
    REQUIRE(fills.size() == 1);
    CHECK(fills[0].price == MID0 + 1 * TICK);   // resting price, NOT the limit
    // Same on the other side: sell limit far below the bid fills at the bid.
    REQUIRE(b.add(2, lob::Side::Buy, 100, MID0 - 1 * TICK) == lob::Result::Ok);
    mo = b.match(11, lob::Side::Sell, 100, MID0 - 10 * TICK, false, fills);
    REQUIRE(mo.result == lob::Result::Ok);
    REQUIRE(fills.size() == 1);
    CHECK(fills[0].price == MID0 - 1 * TICK);
}

TEST_CASE("match: shares conserved across a match") {
    lob::Book b;
    REQUIRE(b.add(1, lob::Side::Sell, 100, MID0 + 1 * TICK) == lob::Result::Ok);
    REQUIRE(b.add(2, lob::Side::Sell, 300, MID0 + 2 * TICK) == lob::Result::Ok);
    uint64_t exec0 = b.shares_executed(), rest0 = b.shares_resting();

    std::vector<lob::Fill> fills;
    auto mo = b.match(10, lob::Side::Buy, 250, MID0 + 2 * TICK, false, fills);
    REQUIRE(mo.result == lob::Result::Ok);
    uint64_t sum = 0;
    for (auto& f : fills) sum += f.shares;
    CHECK(sum == mo.filled);                              // fills vs outcome
    CHECK(b.shares_executed() - exec0 == sum);            // removed from resting
    CHECK(rest0 - b.shares_resting() == sum);             // (nothing rested here)
    CHECK(mo.filled + mo.rested + mo.canceled == 250);    // aggressor conserved
    CHECK(b.invariants_fast());
    CHECK(b.audit().empty());
}

TEST_CASE("match: limit remainder rests at its own limit and does not cross") {
    lob::Book b;
    REQUIRE(b.add(1, lob::Side::Sell, 100, MID0 + 1 * TICK) == lob::Result::Ok);
    REQUIRE(b.add(2, lob::Side::Sell, 100, MID0 + 5 * TICK) == lob::Result::Ok);
    std::vector<lob::Fill> fills;
    // Crosses level 1 fully, cannot reach level 2, remainder rests at 100.03.
    auto mo = b.match(10, lob::Side::Buy, 400, MID0 + 3 * TICK, false, fills);
    REQUIRE(mo.result == lob::Result::Ok);
    CHECK(mo.filled == 100);
    CHECK(mo.rested == 300);
    const lob::Order* o = b.find(10);
    REQUIRE(o != nullptr);
    CHECK(o->shares == 300);
    CHECK(o->price == MID0 + 3 * TICK);       // its own limit price
    CHECK(o->side == lob::Side::Buy);
    CHECK(b.best_bid() == MID0 + 3 * TICK);
    CHECK(b.best_ask() == MID0 + 5 * TICK);
    CHECK(!b.crossed());
    CHECK(b.audit().empty());
}

TEST_CASE("match: market remainder is canceled, market orders never rest") {
    lob::Book b;
    REQUIRE(b.add(1, lob::Side::Sell, 100, MID0 + 1 * TICK) == lob::Result::Ok);
    std::vector<lob::Fill> fills;
    auto mo = b.match(10, lob::Side::Buy, 400, 0, true, fills);
    REQUIRE(mo.result == lob::Result::Ok);
    CHECK(mo.filled == 100);
    CHECK(mo.rested == 0);
    CHECK(mo.canceled == 300);
    CHECK(b.find(10) == nullptr);             // nothing rested
    CHECK(b.open_orders() == 0);              // book fully swept
    CHECK(b.audit().empty());
}

TEST_CASE("match: market order into an empty book vanishes, changes nothing") {
    lob::Book b;
    std::string before = fingerprint(b);
    std::vector<lob::Fill> fills;
    std::vector<itch::Message> out;
    uint64_t match_seq = 0;
    auto mo = lob::match_submit(b, req(10, lob::Side::Buy, 500, 0, true),
                                match_seq, fills, out);
    REQUIRE(mo.result == lob::Result::Ok);
    CHECK(mo.filled == 0);
    CHECK(mo.canceled == 500);
    CHECK(fills.empty());
    CHECK(out.empty());                       // emits nothing at all
    CHECK(match_seq == 0);
    CHECK(fingerprint(b) == before);
}

TEST_CASE("match: limit order into an empty opposite side rests entirely") {
    lob::Book b;
    std::vector<lob::Fill> fills;
    std::vector<itch::Message> out;
    uint64_t match_seq = 0;
    auto mo = lob::match_submit(b, req(10, lob::Side::Buy, 500, MID0, false),
                                match_seq, fills, out);
    REQUIRE(mo.result == lob::Result::Ok);
    CHECK(mo.filled == 0);
    CHECK(mo.rested == 500);
    REQUIRE(out.size() == 1);                 // exactly the A, no fills
    auto* a = std::get_if<itch::AddOrder>(&out[0]);
    REQUIRE(a != nullptr);
    CHECK(a->order_ref == 10);
    CHECK(a->shares == 500);
    CHECK(a->price == MID0);
}

TEST_CASE("match: validation rejects duplicate ref and zero shares") {
    lob::Book b;
    REQUIRE(b.add(1, lob::Side::Sell, 100, MID0 + TICK) == lob::Result::Ok);
    std::vector<lob::Fill> fills;
    auto mo = b.match(1, lob::Side::Buy, 100, MID0 + TICK, false, fills);
    CHECK(mo.result == lob::Result::DuplicateId);
    CHECK(fills.empty());
    mo = b.match(10, lob::Side::Buy, 0, MID0 + TICK, false, fills);
    CHECK(mo.result == lob::Result::ZeroShares);
    CHECK(b.find(1)->shares == 100);          // book untouched by rejects
    CHECK(b.audit().empty());
}

TEST_CASE("match: emitted E messages carry resting refs and fill shares") {
    lob::Book b;
    REQUIRE(b.add(1, lob::Side::Sell, 100, MID0 + 1 * TICK) == lob::Result::Ok);
    REQUIRE(b.add(2, lob::Side::Sell, 200, MID0 + 2 * TICK) == lob::Result::Ok);
    std::vector<lob::Fill> fills;
    std::vector<itch::Message> out;
    uint64_t match_seq = 0;
    auto mo = lob::match_submit(
        b, req(10, lob::Side::Buy, 250, MID0 + 2 * TICK, false), match_seq,
        fills, out);
    REQUIRE(mo.result == lob::Result::Ok);
    REQUIRE(out.size() == 2);                 // two fills, no remainder
    auto* e0 = std::get_if<itch::OrderExecuted>(&out[0]);
    auto* e1 = std::get_if<itch::OrderExecuted>(&out[1]);
    REQUIRE(e0 != nullptr);
    REQUIRE(e1 != nullptr);
    CHECK(e0->order_ref == 1);
    CHECK(e0->shares == 100);
    CHECK(e0->match_num == 1);
    CHECK(e1->order_ref == 2);
    CHECK(e1->shares == 150);
    CHECK(e1->match_num == 2);
}

// ---------------------------------------------------------------------------
// THE STRONG TEST. Raw aborts in the hot loop, same rationale as test_fuzz.
namespace {

constexpr size_t AUDIT_EVERY = 4096;

size_t fuzz_n() {
    if (const char* s = std::getenv("FUZZ_N")) return strtoull(s, nullptr, 10);
    return 200'000;
}

void run_match_fuzz(uint64_t seed, size_t n_matches) {
    std::mt19937_64 rng(seed);
    lob::Book mbook;                          // book under the matching path
    std::vector<uint8_t> stream;              // captured framed wire bytes
    std::vector<itch::Message> emitted;       // per-op scratch
    std::vector<lob::Fill> fills;
    std::vector<uint64_t> live;               // refs currently resting
    uint64_t match_seq = 0, next_ref = 0, ts = 34'200'000'000'000ULL;
    uint64_t n_market = 0, n_crossing = 0, n_passive = 0, total_fills = 0;

    auto forget = [&](uint64_t ref) {
        for (size_t i = 0; i < live.size(); ++i)
            if (live[i] == ref) {
                live[i] = live.back();
                live.pop_back();
                return;
            }
    };
    // A limit price around the opposite best: offset <= 0 crosses.
    auto price_for = [&](lob::Side s) -> uint32_t {
        int64_t off = int64_t(rng() % 21) - 5;      // [-5 .. +15] ticks
        int64_t p;
        if (s == lob::Side::Buy) {
            uint32_t ba = mbook.best_ask();
            p = int64_t(ba ? ba : MID0 + TICK) - off * TICK;  // off<=0: crosses
        } else {
            uint32_t bb = mbook.best_bid();
            p = int64_t(bb ? bb : MID0 - TICK) + off * TICK;
        }
        return p < TICK ? TICK : uint32_t(p);
    };

    for (size_t i = 0; i < n_matches; ++i) {
        // ---- one aggressive-or-resting order through the matching path ----
        lob::Side side = (rng() & 1) ? lob::Side::Buy : lob::Side::Sell;
        bool market = rng() % 100 < 15;
        uint32_t shares = 100 * (1 + rng() % 10);
        uint32_t limit = market ? 0 : price_for(side);
        uint64_t ref = ++next_ref;

        auto exp = expected_fills(mbook, side, shares, limit, market);
        uint64_t exec0 = mbook.shares_executed();
        uint64_t rest0 = mbook.shares_resting();
        uint64_t add0  = mbook.shares_added();
        uint64_t canc0 = mbook.shares_canceled();

        emitted.clear();
        auto r = req(ref, side, shares, limit, market);
        r.timestamp = ts += 1 + rng() % 5000;
        auto mo = lob::match_submit(mbook, r, match_seq, fills, emitted);
        if (mo.result != lob::Result::Ok) {
            std::fprintf(stderr, "match rejected at op %zu: %s\n", i,
                         lob::to_string(mo.result));
            std::abort();
        }

        // Oracle: exact fill sequence (price priority, FIFO, sizes, prices).
        if (fills.size() != exp.size()) std::abort();
        for (size_t k = 0; k < fills.size(); ++k)
            if (fills[k].resting_ref != exp[k].resting_ref ||
                fills[k].shares != exp[k].shares ||
                fills[k].price != exp[k].price)
                std::abort();
        // Conservation, both sides of the trade.
        uint64_t sum = 0;
        for (auto& f : fills) sum += f.shares;
        if (sum != mo.filled) std::abort();
        if (mo.filled + mo.rested + mo.canceled != shares) std::abort();
        if (mbook.shares_executed() - exec0 != sum) std::abort();
        if (mbook.shares_added() - add0 != mo.rested) std::abort();
        if (mbook.shares_canceled() != canc0) std::abort();
        if (rest0 + mo.rested - sum != mbook.shares_resting()) std::abort();
        // Never crossed; remainder handling.
        if (mbook.crossed()) std::abort();
        if (market && mo.rested != 0) std::abort();
        if (mo.rested) {
            const lob::Order* o = mbook.find(ref);
            if (!o || o->shares != mo.rested || o->price != limit ||
                o->side != side)
                std::abort();
            live.push_back(ref);
        }
        // Emission shape: one E per fill (in order), then A iff rested.
        size_t want = fills.size() + (mo.rested ? 1 : 0);
        if (emitted.size() != want) std::abort();
        for (size_t k = 0; k < fills.size(); ++k) {
            auto* e = std::get_if<itch::OrderExecuted>(&emitted[k]);
            if (!e || e->order_ref != fills[k].resting_ref ||
                e->shares != fills[k].shares)
                std::abort();
            if (!mbook.find(fills[k].resting_ref))    // fully consumed
                forget(fills[k].resting_ref);
        }
        if (mo.rested &&
            !std::get_if<itch::AddOrder>(&emitted[fills.size()]))
            std::abort();

        n_market += market;
        n_crossing += !fills.empty();
        total_fills += fills.size();
        for (auto& m : emitted) itch::encode_framed(m, stream);

        // ---- occasionally, one passive op on a resting order --------------
        if (!live.empty() && rng() % 100 < 25) {
            itch::Message pm;
            uint64_t pref = live[rng() % live.size()];
            const lob::Order* o = mbook.find(pref);
            if (!o) std::abort();
            switch (rng() % 3) {
                case 0: {                     // full delete
                    itch::OrderDelete d;
                    d.h.stock_locate = 1; d.h.timestamp = ts += 1;
                    d.order_ref = pref;
                    pm = d;
                    forget(pref);
                    break;
                }
                case 1: {                     // partial (or full) cancel
                    itch::OrderCancel x;
                    x.h.stock_locate = 1; x.h.timestamp = ts += 1;
                    x.order_ref = pref;
                    x.shares = 1 + rng() % o->shares;
                    if (x.shares == o->shares) forget(pref);
                    pm = x;
                    break;
                }
                default: {                    // replace, non-crossing price
                    // A crossing replace is out of scope for this path: the
                    // Phase 2 adapter decomposes it into cancel + aggressive
                    // order. Generator therefore stays non-crossing here.
                    itch::OrderReplace u;
                    u.h.stock_locate = 1; u.h.timestamp = ts += 1;
                    u.orig_order_ref = pref;
                    u.new_order_ref = ++next_ref;
                    u.shares = 100 * (1 + rng() % 10);
                    uint32_t np;
                    if (o->side == lob::Side::Buy) {
                        uint32_t ba = mbook.best_ask();
                        uint32_t base = ba ? ba : MID0 + TICK;
                        uint32_t k = 1 + rng() % 10;
                        np = base > k * TICK ? base - k * TICK : TICK;
                    } else {
                        uint32_t bb = mbook.best_bid();
                        np = (bb ? bb : MID0 - TICK) + (1 + rng() % 10) * TICK;
                    }
                    u.price = np;
                    pm = u;
                    forget(pref);
                    live.push_back(u.new_order_ref);
                    break;
                }
            }
            if (lob::apply(mbook, pm) != lob::Result::Ok) std::abort();
            itch::encode_framed(pm, stream);
            ++n_passive;
        }

        if (!mbook.invariants_fast()) {
            std::fprintf(stderr, "invariants broken at op %zu: %s\n", i,
                         mbook.audit().c_str());
            std::abort();
        }
        if (i % AUDIT_EVERY == 0) {
            std::string err = mbook.audit();
            if (!err.empty())
                FAIL("audit failed at op " << i << ": " << err);
        }
    }
    std::string err = mbook.audit();
    if (!err.empty()) FAIL("final audit: " << err);

    // ---- replay the captured stream through the RECONSTRUCTION path -------
    lob::Book rbook;
    itch::FrameReader rd{stream.data(), stream.size()};
    size_t applied = 0;
    while (auto m = rd.next()) {
        lob::Result r = lob::apply(rbook, *m);
        if (r != lob::Result::Ok)
            FAIL("reconstruction rejected emitted msg " << applied << ": "
                 << lob::to_string(r));
        ++applied;
    }
    REQUIRE(!rd.error);
    REQUIRE(rd.skipped == 0);
    std::string rerr = rbook.audit();
    if (!rerr.empty()) FAIL("reconstructed book audit: " << rerr);

    // Byte-for-byte identical end state.
    REQUIRE(fingerprint(mbook) == fingerprint(rbook));

    INFO("matches=" << n_matches << " market=" << n_market << " crossing="
         << n_crossing << " fills=" << total_fills << " passive=" << n_passive
         << " emitted_msgs=" << applied << " open=" << mbook.open_orders());
    CHECK(n_crossing > 0);
    CHECK(total_fills > 0);
}

}  // namespace

TEST_CASE("fuzz: match-emitted stream reconstructs to identical state") {
    run_match_fuzz(/*seed=*/7, fuzz_n());
}

TEST_CASE("fuzz: match, multiple seeds, smaller runs") {
    for (uint64_t seed = 30; seed < 34; ++seed)
        run_match_fuzz(seed, 20'000);
}
