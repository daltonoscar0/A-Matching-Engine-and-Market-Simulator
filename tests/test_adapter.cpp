// The Phase 2 adapter's contract: garbage is rejected and counted by reason
// before it can touch the book, valid flow routes to the right engine
// primitive, rejection is bit-identical-total, and a long generated loop
// holds every invariant (the Phase 2 milestone).
#include "../third_party/catch.hpp"

#include <random>
#include <vector>

#include "../src/adapter.hpp"
#include "../src/bookset.hpp"

using namespace lob;

namespace {

// A structural fingerprint of a Book: two books with equal fingerprints have
// identical ledgers, touch, level counts, and per-level (price, shares,
// count). Used for the "rejection is total" and round-trip checks.
struct FP {
    uint64_t added = 0, executed = 0, canceled = 0, resting = 0;
    size_t open = 0, blev = 0, alev = 0;
    uint32_t bb = 0, ba = 0;
    uint64_t lhash = 1469598103934665603ull;   // FNV-1a
    bool operator==(const FP& o) const {
        return added == o.added && executed == o.executed &&
               canceled == o.canceled && resting == o.resting &&
               open == o.open && blev == o.blev && alev == o.alev &&
               bb == o.bb && ba == o.ba && lhash == o.lhash;
    }
};

FP fingerprint(const Book& b) {
    FP f;
    f.added = b.shares_added(); f.executed = b.shares_executed();
    f.canceled = b.shares_canceled(); f.resting = b.shares_resting();
    f.open = b.open_orders(); f.blev = b.bid_levels(); f.alev = b.ask_levels();
    f.bb = b.best_bid(); f.ba = b.best_ask();
    auto mix = [&](uint64_t x) {
        f.lhash = (f.lhash ^ x) * 1099511628211ull;
    };
    b.for_each_level([&](Side s, const Level& lv) {
        mix(s == Side::Buy ? 1 : 2); mix(lv.price);
        mix(lv.total_shares); mix(lv.order_count);
        return true;
    });
    return f;
}

}  // namespace

TEST_CASE("every rejection category fires on a hand-built bad action",
          "[adapter]") {
    Adapter ad;
    // Seed one resting level each side so touch-relative checks have context.
    REQUIRE(ad.submit({ActionKind::Limit, Side::Buy,  100000, 100}).applied());
    REQUIRE(ad.submit({ActionKind::Limit, Side::Sell, 100100, 100}).applied());

    // Unparseable: zero size, and a limit with no price.
    REQUIRE(ad.submit({ActionKind::Limit, Side::Buy, 99900, 0}).reject
            == Reject::Unparseable);
    REQUIRE(ad.submit({ActionKind::Limit, Side::Buy, 0, 100}).reject
            == Reject::Unparseable);

    // UnknownReference: cancel a level with nothing resting.
    REQUIRE(ad.submit({ActionKind::Cancel, Side::Buy, 55555, 100}).reject
            == Reject::UnknownReference);

    // EconomicallyAbsurd: size over cap, and price a mile from the touch.
    REQUIRE(ad.submit({ActionKind::Market, Side::Buy, 0, 5'000'000}).reject
            == Reject::EconomicallyAbsurd);
    REQUIRE(ad.submit({ActionKind::Limit, Side::Buy, 300000, 100}).reject
            == Reject::EconomicallyAbsurd);   // 3x the ask

    REQUIRE(ad.reject_count(Reject::Unparseable) == 2);
    REQUIRE(ad.reject_count(Reject::UnknownReference) == 1);
    REQUIRE(ad.reject_count(Reject::EconomicallyAbsurd) == 2);
    REQUIRE(ad.rejected() == 5);
    REQUIRE(ad.applied() == 2);
}

TEST_CASE("rejection is total: a rejected action leaves the book bit-identical",
          "[adapter]") {
    Adapter ad;
    ad.submit({ActionKind::Limit, Side::Buy,  100000, 200});
    ad.submit({ActionKind::Limit, Side::Sell, 100100, 300});
    ad.submit({ActionKind::Limit, Side::Buy,   99900, 150});
    FP before = fingerprint(ad.book());

    const EmittedAction bad[] = {
        {ActionKind::Limit,  Side::Buy,  99800, 0},          // Unparseable
        {ActionKind::Cancel, Side::Sell, 42424, 100},        // UnknownReference
        {ActionKind::Market, Side::Buy,  0, 9'000'000},      // EconomicallyAbsurd
        {ActionKind::Limit,  Side::Sell, 1, 100},            // EconomicallyAbsurd (far below bid)
    };
    for (const auto& a : bad) {
        Outcome o = ad.submit(a);
        REQUIRE_FALSE(o.applied());
        REQUIRE(fingerprint(ad.book()) == before);   // untouched
    }
}

TEST_CASE("valid stream routes correctly and matches a direct construction",
          "[adapter]") {
    // Marketable limits and markets should fill; away limits should rest;
    // cancels should remove. Build the same book directly and compare.
    Adapter ad;
    std::vector<EmittedAction> stream = {
        {ActionKind::Limit,  Side::Buy,  100000, 500},   // rest bid
        {ActionKind::Limit,  Side::Sell, 100200, 400},   // rest ask
        {ActionKind::Limit,  Side::Buy,  100100, 100},   // rest inside
        {ActionKind::Limit,  Side::Sell, 100100, 250},   // crosses the 100100 bid -> fills 100
        {ActionKind::Market, Side::Buy,  0, 150},        // hits ask 100200
        {ActionKind::Cancel, Side::Buy,  100000, 0},     // remove resting bid
    };
    for (const auto& a : stream) REQUIRE(ad.submit(a).applied());

    // Direct reference: same routing, refs from 1, no validation layer.
    Book ref;
    uint64_t seq = 0, next = 1;
    std::vector<Fill> fills;
    std::vector<itch::Message> emit;
    auto limit = [&](Side s, uint32_t px, uint32_t sh) {
        uint32_t opp = s == Side::Buy ? ref.best_ask() : ref.best_bid();
        bool mk = opp && (s == Side::Buy ? px >= opp : px <= opp);
        if (mk) {
            MatchRequest r; r.ref = next++; r.side = s; r.shares = sh;
            r.limit = px; r.market = false;
            emit.clear(); match_submit(ref, r, seq, fills, emit);
        } else {
            ref.add(next++, s, sh, px);
        }
    };
    limit(Side::Buy, 100000, 500);
    limit(Side::Sell, 100200, 400);
    limit(Side::Buy, 100100, 100);
    limit(Side::Sell, 100100, 250);
    { MatchRequest r; r.ref = next++; r.side = Side::Buy; r.shares = 150;
      r.market = true; emit.clear(); match_submit(ref, r, seq, fills, emit); }
    // cancel resting bid at 100000: head ref there is 1
    ref.remove(1);

    REQUIRE(fingerprint(ad.book()) == fingerprint(ref));
    REQUIRE(ad.book().audit().empty());
}

TEST_CASE("state feedback reflects the top-N book", "[adapter]") {
    AdapterConfig cfg; cfg.feedback_levels = 3;
    Adapter ad(cfg);
    ad.submit({ActionKind::Limit, Side::Buy, 100000, 100});
    ad.submit({ActionKind::Limit, Side::Buy,  99900, 200});
    ad.submit({ActionKind::Limit, Side::Buy,  99800, 300});
    ad.submit({ActionKind::Limit, Side::Buy,  99700, 400});   // 4th, beyond N
    ad.submit({ActionKind::Limit, Side::Sell, 100100, 500});
    BookView v = ad.state();
    REQUIRE(v.bids.size() == 3);                 // capped at feedback_levels
    REQUIRE(v.bids[0].price == 100000);          // best first
    REQUIRE(v.bids[2].price == 99800);
    REQUIRE(v.asks.size() == 1);
    REQUIRE(v.two_sided);
    REQUIRE(v.spread == 100);
}

TEST_CASE("sampler honours top-k and temperature deterministically",
          "[adapter]") {
    std::vector<double> logits = {0.1, 5.0, 0.2, 4.0, 0.3};
    AdapterConfig cfg;

    // top_k = 1 always picks the argmax regardless of draw.
    cfg.top_k = 1;
    std::mt19937_64 rng(1);
    for (int i = 0; i < 100; ++i) REQUIRE(sample_index(logits, cfg, rng) == 1);

    // Low temperature concentrates on the argmax.
    cfg.top_k = 0; cfg.temperature = 0.01;
    std::mt19937_64 rng2(7);
    int argmax = 0;
    for (int i = 0; i < 200; ++i) if (sample_index(logits, cfg, rng2) == 1) ++argmax;
    REQUIRE(argmax >= 195);

    // Same seed -> same sequence (determinism / sweepability).
    cfg.temperature = 1.0;
    std::mt19937_64 a(42), b(42);
    for (int i = 0; i < 50; ++i)
        REQUIRE(sample_index(logits, cfg, a) == sample_index(logits, cfg, b));

    // top_k >= n is a no-op; empty logits is handled.
    cfg.top_k = 99;
    std::mt19937_64 rng3(3);
    REQUIRE(sample_index(logits, cfg, rng3) < logits.size());
    std::mt19937_64 rng4(3);
    REQUIRE(sample_index({}, cfg, rng4) == 0);
}

TEST_CASE("the loop runs 50k generated steps with zero invariant violations",
          "[adapter]") {
    AdapterConfig cfg; cfg.seed = 12345;
    Adapter ad(cfg);
    std::mt19937_64 g(999);
    std::uniform_int_distribution<int> kind(0, 9);
    std::uniform_int_distribution<int> sidebit(0, 1);
    std::uniform_int_distribution<int> off(-30, 30);      // ticks from base
    std::uniform_int_distribution<int> sz(1, 2000);
    uint32_t base = 100000;

    const int N = 50000;
    for (int i = 0; i < N; ++i) {
        base += uint32_t(off(g)) - 0;                     // let the mid drift
        if (base < 5000) base = 5000;
        Side s = sidebit(g) ? Side::Buy : Side::Sell;
        EmittedAction a;
        a.side = s;
        int k = kind(g);
        if (k < 6) {                                      // limit near touch
            a.kind = ActionKind::Limit;
            a.price = base + uint32_t(off(g)) * 100;
            a.shares = uint32_t(sz(g));
        } else if (k < 8) {                               // market
            a.kind = ActionKind::Market;
            a.shares = uint32_t(sz(g));
        } else {                                          // cancel a real level
            BookView v = ad.state();
            const auto& lvls = s == Side::Buy ? v.bids : v.asks;
            a.kind = ActionKind::Cancel;
            a.price = lvls.empty() ? 12345
                                   : lvls[size_t(g()) % lvls.size()].price;
            a.shares = 1;
        }
        ad.submit(a);
        // The engine must never be left in a bad state, applied or rejected.
        REQUIRE(ad.book().invariants_fast());
        if ((i & 1023) == 0) REQUIRE(ad.book().audit().empty());
    }
    REQUIRE(ad.book().audit().empty());
    REQUIRE(ad.applied() + ad.rejected() == uint64_t(N));
    REQUIRE(ad.applied() > 0);
}

// ---------------------------------------------------------------- journal
// 2026-08-02. Phase 3's LM column is produced by replaying the adapter's ITCH
// journal through tools/stylized, so the journal must be RECONSTRUCTION-CLOSED:
// replaying it into a fresh book must reproduce the adapter's book exactly,
// with zero rejects and exact share conservation. If this property is wrong
// the LM column is measuring a book nobody ever traded, so it is pinned here
// over a long randomized run rather than on a couple of hand cases.
TEST_CASE("adapter journal replays to an identical book", "[adapter]") {
    AdapterConfig cfg;
    Adapter ad(cfg);
    itch::Stock stock = {'L','M','S','I','M',' ',' ',' '};
    ad.journal_enable(7, stock);

    std::mt19937_64 g(20260802);
    std::uniform_int_distribution<int> kind(0, 9);
    std::uniform_int_distribution<uint32_t> sz(1, 500);
    std::uniform_int_distribution<uint32_t> px(999'000, 1'001'000);

    // Seed both sides so marketable flow has something to hit.
    REQUIRE(ad.submit({ActionKind::Limit, Side::Buy,  999'900, 100}).applied());
    REQUIRE(ad.submit({ActionKind::Limit, Side::Sell, 1'000'100, 100}).applied());

    const int N = 20000;
    uint64_t clock_ns = 34'200'000'000'000ull;  // 09:30:00
    for (int i = 0; i < N; ++i) {
        clock_ns += uint64_t(g() % 1'000'000);
        ad.journal_time(clock_ns);
        EmittedAction a;
        a.side = (g() & 1) ? Side::Buy : Side::Sell;
        int k = kind(g);
        if (k < 6) {                                      // resting or crossing
            a.kind = ActionKind::Limit;
            a.price = px(g);
            a.shares = sz(g);
        } else if (k < 8) {                               // market
            a.kind = ActionKind::Market;
            a.shares = sz(g);
        } else {                                          // cancel a real level
            BookView v = ad.state();
            const auto& lvls = a.side == Side::Buy ? v.bids : v.asks;
            a.kind = ActionKind::Cancel;
            a.price = lvls.empty() ? 12345
                                   : lvls[size_t(g()) % lvls.size()].price;
            a.shares = 1;
        }
        ad.submit(a);
    }
    REQUIRE(ad.applied() > 0);
    REQUIRE(ad.book().audit().empty());
    REQUIRE(!ad.journal().empty());

    // Replay the journal into a fresh book through the validated feed path.
    BookSet set;
    for (const itch::Message& m : ad.journal())
        REQUIRE(set.apply(m) == Result::Ok);

    size_t books = 0;
    set.for_each_book([&](uint16_t locate, Book& bk) {
        ++books;
        REQUIRE(locate == 7);
        REQUIRE(bk.audit().empty());
        REQUIRE(bk.shares_added() ==
                bk.shares_executed() + bk.shares_canceled() + bk.shares_resting());
        REQUIRE(fingerprint(bk) == fingerprint(ad.book()));
    });
    REQUIRE(books == 1);
}
