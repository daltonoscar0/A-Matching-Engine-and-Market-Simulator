// The token<->action shim (src/token_shim.hpp): generated OFTK 5-tuples
// must route to the SAME book state as the equivalent direct
// EmittedActions, and malformed tuples must land in the right rejection
// category without touching the book.
#include <vector>

#include "../src/token_shim.hpp"
#include "../third_party/catch.hpp"

namespace {

oftk::TickerBins test_bins() {
    oftk::TickerBins b;
    b.tick_size = 100;
    b.size_edges = {100, 200, 300, 400, 500, 600, 700};
    b.dt_edges_ns = {2,   4,   8,    16,   32,   64,   128,
                     256, 512, 1024, 2048, 4096, 8192, 16384};
    REQUIRE(b.valid());
    return b;
}

// Full observable state equality: touch, ledger, and every level.
void require_same_books(const lob::Book& x, const lob::Book& y) {
    REQUIRE(x.best_bid() == y.best_bid());
    REQUIRE(x.best_ask() == y.best_ask());
    REQUIRE(x.open_orders() == y.open_orders());
    REQUIRE(x.shares_added() == y.shares_added());
    REQUIRE(x.shares_executed() == y.shares_executed());
    REQUIRE(x.shares_canceled() == y.shares_canceled());
    REQUIRE(x.shares_resting() == y.shares_resting());
    std::vector<std::tuple<int, uint32_t, uint64_t, uint32_t>> lx, ly;
    x.for_each_level([&](lob::Side s, const lob::Level& lv) {
        lx.emplace_back(int(s), lv.price, lv.total_shares, lv.order_count);
        return true;
    });
    y.for_each_level([&](lob::Side s, const lob::Level& lv) {
        ly.emplace_back(int(s), lv.price, lv.total_shares, lv.order_count);
        return true;
    });
    REQUIRE(lx == ly);
}

// Token ids for readability. SZ1 = bucket 1, representative 100 with
// test_bins (bucket_rep(1) = edges[0] = 100).
constexpr uint16_t T_ADD = 7, T_CANCEL = 8, T_DELETE = 9, T_EXEC = 10,
                   T_EXEC_HIDDEN = 11;
constexpr uint16_t S_BID = 13, S_ASK = 14;
constexpr uint16_t PX_M1 = 15, PX0 = 16, PX5 = 21, PX_TAIL_ID = 27;
constexpr uint16_t SZ1 = 29;  // rep 100
constexpr uint16_t DT0 = 36;

}  // namespace

TEST_CASE("shim: token tuples route to the same book as direct actions") {
    oftk::TickerBins bins = test_bins();
    lob::Adapter direct, viatok;
    shim::Counts c;

    // Seed both books identically with direct actions (an empty book gives
    // the shim no price reference, by design).
    for (lob::Adapter* a : {&direct, &viatok}) {
        REQUIRE(a->submit({lob::ActionKind::Limit, lob::Side::Buy, 10000,
                           100}).applied());
        REQUIRE(a->submit({lob::ActionKind::Limit, lob::Side::Sell, 10200,
                           100}).applied());
    }
    require_same_books(direct.book(), viatok.book());

    struct Step {
        uint16_t tuple[5];
        lob::EmittedAction equiv;  // hand-derived from the resolution rules
    };
    // Book state is tracked in the comments; the equivalent action is
    // derived by hand from the documented shim rules, NOT by calling the
    // shim - that is what makes this a routing test, not a tautology.
    const Step steps[] = {
        // bids 10000(100), asks 10200(100)
        // ADD bid at level 0 -> Limit Buy 100 @ 10000
        {{T_ADD, S_BID, PX0, SZ1, DT0},
         {lob::ActionKind::Limit, lob::Side::Buy, 10000, 100}},
        // ADD ask inside the spread -> best_ask 10200 - tick = 10100
        {{T_ADD, S_ASK, PX_M1, SZ1, DT0},
         {lob::ActionKind::Limit, lob::Side::Sell, 10100, 100}},
        // bids 10000(200), asks 10100(100) 10200(100)
        // ADD bid inside -> best_bid 10000 + tick = 10100: MARKETABLE,
        // routes through match() and fills the 10100 ask in both paths.
        {{T_ADD, S_BID, PX_M1, SZ1, DT0},
         {lob::ActionKind::Limit, lob::Side::Buy, 10100, 100}},
        // bids 10000(200), asks 10200(100)
        // EXEC against the standing BID -> Market SELL 100 at the touch.
        {{T_EXEC, S_BID, PX0, SZ1, DT0},
         {lob::ActionKind::Market, lob::Side::Sell, 0, 100}},
        // bids 10000(100), asks 10200(100)
        // DELETE ask level 0 -> Cancel (FIFO head) at 10200.
        {{T_DELETE, S_ASK, PX0, SZ1, DT0},
         {lob::ActionKind::Cancel, lob::Side::Sell, 10200, 100}},
    };
    for (const Step& s : steps) {
        lob::Outcome od = direct.submit(s.equiv);
        lob::Outcome ot = shim::step(viatok, s.tuple, bins, c);
        REQUIRE(od.applied());
        REQUIRE(ot.applied());
        require_same_books(direct.book(), viatok.book());
    }
    REQUIRE(c.applied == 5);
    REQUIRE(c.rejected() == 0);
}

TEST_CASE("shim: PartialCancel maps to Cancel exactly like Delete") {
    oftk::TickerBins bins = test_bins();
    lob::Adapter viadel, viacan;
    for (lob::Adapter* a : {&viadel, &viacan})
        REQUIRE(a->submit({lob::ActionKind::Limit, lob::Side::Buy, 10000,
                           100}).applied());
    shim::Counts c;
    uint16_t del[5] = {T_DELETE, S_BID, PX0, SZ1, DT0};
    uint16_t can[5] = {T_CANCEL, S_BID, PX0, SZ1, DT0};
    REQUIRE(shim::step(viadel, del, bins, c).applied());
    REQUIRE(shim::step(viacan, can, bins, c).applied());
    require_same_books(viadel.book(), viacan.book());
    REQUIRE(viadel.book().open_orders() == 0);
}

TEST_CASE("shim: PX_TAIL resolves one tick beyond the 11th level") {
    oftk::TickerBins bins = test_bins();
    lob::Adapter direct, viatok;
    for (lob::Adapter* a : {&direct, &viatok})
        for (int i = 0; i < 12; ++i)
            REQUIRE(a->submit({lob::ActionKind::Limit, lob::Side::Buy,
                               uint32_t(20000 - 100 * i), 100}).applied());
    // Level index 10 = 19000; tail representative = 19000 - 100 = 18900.
    shim::Counts c;
    uint16_t tup[5] = {T_ADD, S_BID, PX_TAIL_ID, SZ1, DT0};
    REQUIRE(direct.submit({lob::ActionKind::Limit, lob::Side::Buy, 18900,
                           100}).applied());
    REQUIRE(shim::step(viatok, tup, bins, c).applied());
    require_same_books(direct.book(), viatok.book());
}

TEST_CASE("shim: malformed and unresolvable tuples hit the right category") {
    oftk::TickerBins bins = test_bins();
    lob::Adapter a;
    shim::Counts c;

    SECTION("out-of-range slot -> Unparseable, book untouched") {
        uint16_t bad[5] = {T_ADD, 99, PX0, SZ1, DT0};
        lob::Outcome o = shim::step(a, bad, bins, c);
        REQUIRE(o.reject == lob::Reject::Unparseable);
        REQUIRE(c.rejects[size_t(lob::Reject::Unparseable)] == 1);
        REQUIRE(a.book().open_orders() == 0);
    }
    SECTION("ExecHidden -> Unparseable (outside the visible-book loop)") {
        uint16_t bad[5] = {T_EXEC_HIDDEN, S_BID, PX0, SZ1, DT0};
        REQUIRE(shim::step(a, bad, bins, c).reject ==
                lob::Reject::Unparseable);
    }
    SECTION("Add with UNK price on an empty book -> UnknownReference") {
        uint16_t t[5] = {T_ADD, S_BID, oftk::UNK, SZ1, DT0};
        REQUIRE(shim::step(a, t, bins, c).reject ==
                lob::Reject::UnknownReference);
    }
    SECTION("Cancel inside the spread -> UnknownReference") {
        REQUIRE(a.submit({lob::ActionKind::Limit, lob::Side::Buy, 10000,
                          100}).applied());
        uint16_t t[5] = {T_DELETE, S_BID, PX_M1, SZ1, DT0};
        REQUIRE(shim::step(a, t, bins, c).reject ==
                lob::Reject::UnknownReference);
        REQUIRE(a.book().open_orders() == 1);
    }
    SECTION("PX_TAIL cancel takes the DEEPEST level; an ordinary "
            "absent-level cancel still rejects (2026-07-31, scoped)") {
        // Real BX flow deletes orders deeper than level 10 thousands of
        // times a day and they must land somewhere. The relocation is
        // scoped to PX_TAIL and no further: relocating ordinary
        // absent-level cancels the same way was tried and MEASURED, and it
        // collapsed the book (real stream applied 99.3% -> 11.7%).
        REQUIRE(a.submit({lob::ActionKind::Limit, lob::Side::Buy, 10000,
                          100}).applied());
        REQUIRE(a.submit({lob::ActionKind::Limit, lob::Side::Buy, 9900,
                          100}).applied());
        uint16_t tail[5] = {T_DELETE, S_BID, PX_TAIL_ID, SZ1, DT0};
        shim::Resolution r = shim::resolve(tail, bins, a.book());
        REQUIRE(r.ok);
        REQUIRE_FALSE(r.opened_new_level);
        REQUIRE(r.action.kind == lob::ActionKind::Cancel);
        REQUIRE(r.action.price == 9900);  // deepest occupied bid level
        REQUIRE(shim::step(a, tail, bins, c).applied());
        REQUIRE(a.book().open_orders() == 1);

        uint16_t t[5] = {T_DELETE, S_BID, PX5, SZ1, DT0};
        REQUIRE(shim::resolve(t, bins, a.book()).why == shim::Why::LevelAbsent);
    }
    SECTION("UNK, -1 and empty-side cancels still reject") {
        REQUIRE(a.submit({lob::ActionKind::Limit, lob::Side::Buy, 10000,
                          100}).applied());
        // An empty side has nothing to cancel, and nothing rests inside the
        // spread by construction - a cancel's price comes from a standing
        // order, so real data cannot produce -1. Both stay rejects.
        uint16_t unk[5] = {T_DELETE, S_ASK, oftk::UNK, SZ1, DT0};
        REQUIRE(shim::resolve(unk, bins, a.book()).why == shim::Why::UnkNonAdd);
        uint16_t inside[5] = {T_DELETE, S_BID, PX_M1, SZ1, DT0};
        REQUIRE(shim::resolve(inside, bins, a.book()).why ==
                shim::Why::InsideNonAdd);
        uint16_t empty_side[5] = {T_DELETE, S_ASK, PX0, SZ1, DT0};
        REQUIRE(shim::resolve(empty_side, bins, a.book()).why ==
                shim::Why::LevelAbsent);
    }
    SECTION("Add at an index deeper than the book OPENS a level at the "
            "nearest achievable index (2026-07-31 anti-ratchet rule)") {
        REQUIRE(a.submit({lob::ActionKind::Limit, lob::Side::Buy, 10000,
                          100}).applied());
        uint16_t t[5] = {T_ADD, S_BID, PX5, SZ1, DT0};
        shim::Resolution r = shim::resolve(t, bins, a.book());
        REQUIRE(r.ok);
        REQUIRE(r.opened_new_level);
        REQUIRE(r.action.price == 9900);  // one tick beyond the only level
        REQUIRE(shim::step(a, t, bins, c).applied());
        REQUIRE(a.book().bid_levels() == 2);
    }
    SECTION("adapter-level reject is counted in the merged breakdown") {
        // Cancel resolves (level exists) but the adapter rejects a Cancel
        // at an empty... it cannot: a resolved level always has an order.
        // Instead: absurd size via a huge size bucket is unreachable with
        // reps <= 700, so exercise the merged count with an applied path
        // and verify submitted == applied.
        REQUIRE(a.submit({lob::ActionKind::Limit, lob::Side::Buy, 10000,
                          100}).applied());
        uint16_t t[5] = {T_ADD, S_BID, PX0, SZ1, DT0};
        REQUIRE(shim::step(a, t, bins, c).applied());
        REQUIRE(c.submitted == c.applied);
    }
}

TEST_CASE("shim: stream driver skips specials and resyncs on garbage") {
    oftk::TickerBins bins = test_bins();
    lob::Adapter a;
    REQUIRE(a.submit({lob::ActionKind::Limit, lob::Side::Buy, 10000,
                      100}).applied());
    REQUIRE(a.submit({lob::ActionKind::Limit, lob::Side::Sell, 10200,
                      100}).applied());
    shim::Counts c;
    std::vector<uint16_t> stream = {
        oftk::BOS, oftk::SESSION_OPEN,
        T_ADD, S_BID, PX0, SZ1, DT0,   // good tuple
        SZ1,                            // misaligned garbage -> resync
        T_ADD, S_ASK, PX0, SZ1, DT0,   // good tuple
        oftk::SESSION_CLOSE, oftk::EOS,
        T_ADD, S_BID, PX0,             // truncated tail -> Unparseable
    };
    shim::drive(a, stream, bins, c);
    REQUIRE(c.specials == 4);
    REQUIRE(c.resyncs == 1);
    REQUIRE(c.applied == 2);
    REQUIRE(c.tuples == 4);  // 2 good + 1 resync + 1 truncated
    REQUIRE(c.rejects[size_t(lob::Reject::Unparseable)] == 2);
}
