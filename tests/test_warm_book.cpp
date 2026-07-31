// Warm-start snapshots (src/warm_book.hpp): a serialized resting book must
// reload into a book that is INDISTINGUISHABLE through everything the shim
// and the adapter can observe - same levels, same FIFO order, same
// resolution of every PRICE_OFF index. Refs differ by construction (the
// tokenizer drops refs); nothing else may.
//
// The last two cases pin the 2026-07-31 findings themselves: a deep level
// index resolves to a DIFFERENT price cold vs warm (so re-simplifying the
// seeding back to two orders changes measured behaviour and fails a test),
// and deep-index adds REBUILD depth rather than bouncing - the anti-ratchet
// property the shim repair exists for.
#include <sstream>
#include <vector>

#include "../src/token_shim.hpp"
#include "../src/warm_book.hpp"
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

// A book with depth on both sides and MULTIPLE orders per level, so FIFO
// order is a thing the snapshot can get wrong.
void build_deep(lob::Book& b) {
    uint64_t ref = 1;
    for (int k = 0; k < 6; ++k) {  // bids 999500..999000, 3 orders each
        for (int j = 0; j < 3; ++j)
            REQUIRE(b.add(ref++, lob::Side::Buy, uint32_t(100 * (j + 1)),
                          uint32_t(999500 - 100 * k)) == lob::Result::Ok);
    }
    for (int k = 0; k < 6; ++k) {  // asks 1000500..1001000
        for (int j = 0; j < 3; ++j)
            REQUIRE(b.add(ref++, lob::Side::Sell, uint32_t(100 * (j + 1)),
                          uint32_t(1000500 + 100 * k)) == lob::Result::Ok);
    }
}

// Level structure AND the FIFO share sequence within every level.
std::vector<std::tuple<int, uint32_t, uint64_t, uint32_t, std::vector<uint32_t>>>
shape_of(const lob::Book& b) {
    std::vector<std::tuple<int, uint32_t, uint64_t, uint32_t,
                           std::vector<uint32_t>>>
        out;
    b.for_each_level([&](lob::Side s, const lob::Level& lv) {
        std::vector<uint32_t> fifo;
        for (const lob::Order* o = lv.head; o; o = o->next)
            fifo.push_back(o->shares);
        out.emplace_back(int(s), lv.price, lv.total_shares, lv.order_count,
                         fifo);
        return true;
    });
    return out;
}

std::string dump(const lob::Book& b) {
    std::ostringstream os;
    std::string err;
    REQUIRE(warm::write(os, "SPY", "20191230", 34200000000000ull, b, &err));
    return os.str();
}

warm::Snapshot parse(const std::string& text) {
    std::istringstream is(text);
    warm::Snapshot s;
    std::string err;
    REQUIRE(warm::read(is, s, &err));
    return s;
}

constexpr uint16_t T_ADD = 7, T_DELETE = 9;
constexpr uint16_t S_BID = 13;
constexpr uint16_t PX0 = 16, PX3 = 19;
constexpr uint16_t SZ1 = 29;  // rep 100
constexpr uint16_t DT0 = 36;

}  // namespace

TEST_CASE("warm book: snapshot round-trips level structure and FIFO order") {
    lob::Book src;
    build_deep(src);

    warm::Snapshot s = parse(dump(src));
    REQUIRE(s.ticker == "SPY");
    REQUIRE(s.day == "20191230");
    REQUIRE(s.ts_ns == 34200000000000ull);
    REQUIRE(s.rows.size() == src.open_orders());

    lob::Book dst;
    std::string err;
    REQUIRE(warm::apply(s, dst, &err));

    REQUIRE(shape_of(dst) == shape_of(src));
    REQUIRE(dst.best_bid() == src.best_bid());
    REQUIRE(dst.best_ask() == src.best_ask());
    REQUIRE(dst.bid_levels() == src.bid_levels());
    REQUIRE(dst.ask_levels() == src.ask_levels());
    REQUIRE(dst.shares_resting() == src.shares_resting());
    REQUIRE(dst.invariants_fast());
    REQUIRE(dst.audit().empty());
    // Re-serializing the reloaded book reproduces the same text: the
    // snapshot is a fixed point, so a sweep can reuse one file forever.
    REQUIRE(dump(dst) == dump(src));
}

TEST_CASE("warm book: the shim resolves identically against the reload") {
    oftk::TickerBins bins = test_bins();
    lob::Book src;
    build_deep(src);
    lob::Book dst;
    std::string err;
    REQUIRE(warm::apply(parse(dump(src)), dst, &err));

    // Every PRICE_OFF slot, both sides, add and delete: same ok/reject, same
    // resolved price, same Why. Refs differ; nothing observable does.
    for (uint16_t type : {T_ADD, T_DELETE}) {
        for (uint16_t side : {uint16_t(13), uint16_t(14)}) {
            for (uint16_t px = 15; px <= 27; ++px) {
                const uint16_t t[5] = {type, side, px, SZ1, DT0};
                shim::Resolution a = shim::resolve(t, bins, src);
                shim::Resolution b = shim::resolve(t, bins, dst);
                REQUIRE(a.ok == b.ok);
                REQUIRE(a.why == b.why);
                REQUIRE(a.reject == b.reject);
                REQUIRE(a.action.price == b.action.price);
                REQUIRE(a.action.shares == b.action.shares);
            }
        }
    }
}

TEST_CASE("warm book: malformed snapshots are refused, not half-applied") {
    lob::Book b;
    warm::Snapshot s;
    std::string err;

    std::istringstream bad_magic("side,price,shares\nB,100,100\n");
    REQUIRE_FALSE(warm::read(bad_magic, s, &err));
    std::istringstream bad_hdr(std::string(warm::kMagic) + " ticker=X\nq\n");
    REQUIRE_FALSE(warm::read(bad_hdr, s, &err));
    std::istringstream bad_row(std::string(warm::kMagic) +
                               " ticker=X\nside,price,shares\nQ,1,1\n");
    REQUIRE_FALSE(warm::read(bad_row, s, &err));
    std::istringstream zero_sz(std::string(warm::kMagic) +
                               " ticker=X\nside,price,shares\nB,100,0\n");
    REQUIRE_FALSE(warm::read(zero_sz, s, &err));

    // A crossed snapshot must fail loudly rather than load a broken book.
    std::istringstream crossed(std::string(warm::kMagic) +
                               " ticker=X\nside,price,shares\n"
                               "B,1000000,100\nS,999900,100\n");
    warm::Snapshot cs;
    REQUIRE(warm::read(crossed, cs, &err));
    REQUIRE_FALSE(warm::apply(cs, b, &err));

    // apply() insists on an empty book: warm-starting over live state would
    // silently mix two initializations.
    lob::Book seeded;
    REQUIRE(seeded.add(1, lob::Side::Buy, 100, 999900) == lob::Result::Ok);
    warm::Snapshot ok;
    ok.rows.push_back({lob::Side::Buy, 999800, 100});
    REQUIRE_FALSE(warm::apply(ok, seeded, &err));
}

TEST_CASE("warm book: a deep index means different prices cold vs warm") {
    oftk::TickerBins bins = test_bins();

    // THE COLD START (what sim_health did before 2026-07-31): two resting
    // orders = one occupied level per side.
    lob::Book cold;
    REQUIRE(cold.add(1, lob::Side::Buy, 100, 999900) == lob::Result::Ok);
    REQUIRE(cold.add(2, lob::Side::Sell, 100, 1000100) == lob::Result::Ok);

    const uint16_t at_best[5] = {T_ADD, S_BID, PX0, SZ1, DT0};
    const uint16_t at_idx3[5] = {T_ADD, S_BID, PX3, SZ1, DT0};

    REQUIRE(shim::resolve(at_best, bins, cold).ok);
    // Under the anti-ratchet rule this now resolves, but only to the
    // NEAREST ACHIEVABLE index - one tick under the single occupied level,
    // three levels shallower than the token asked for. Warm-starting is
    // what makes the requested index mean what it meant when it was
    // recorded; the repair keeps the book alive, it does not make a
    // one-level book carry four levels of information.
    shim::Resolution deep_cold = shim::resolve(at_idx3, bins, cold);
    REQUIRE(deep_cold.ok);
    REQUIRE(deep_cold.opened_new_level);
    REQUIRE(deep_cold.action.price == 999800);

    lob::Book deep;
    build_deep(deep);
    lob::Book warmed;
    std::string err;
    REQUIRE(warm::apply(parse(dump(deep)), warmed, &err));
    shim::Resolution deep_warm = shim::resolve(at_idx3, bins, warmed);
    REQUIRE(deep_warm.ok);
    REQUIRE_FALSE(deep_warm.opened_new_level);
    REQUIRE(deep_warm.action.price == 999200);  // 4th occupied bid level
}

TEST_CASE("shim: deep-index adds REBUILD depth - the anti-ratchet property") {
    // The defect this pins (RESULTS.md 2026-07-31 Step 2): with the old
    // rule, level destruction was unrestricted while level creation only
    // happened at the touch, so occupied depth could only fall. Drive a
    // stream that alternates a deep-index add with a delete of the best
    // level - under the old rule the adds all rejected and the book
    // drained; the book must now hold depth instead.
    oftk::TickerBins bins = test_bins();
    lob::Adapter a;
    shim::Counts c;
    REQUIRE(a.submit({lob::ActionKind::Limit, lob::Side::Buy, 999900,
                      100}).applied());
    REQUIRE(a.submit({lob::ActionKind::Limit, lob::Side::Sell, 1000100,
                      100}).applied());

    const uint16_t deep_add[5] = {T_ADD, S_BID, PX3, SZ1, DT0};
    const uint16_t del_best[5] = {T_DELETE, S_BID, PX0, SZ1, DT0};
    for (int i = 0; i < 40; ++i) {
        REQUIRE(shim::step(a, deep_add, bins, c).applied());
        REQUIRE(shim::step(a, deep_add, bins, c).applied());
        shim::step(a, del_best, bins, c);  // may or may not resolve
        REQUIRE(a.book().invariants_fast());
    }
    REQUIRE(a.book().audit().empty());
    REQUIRE(a.book().bid_levels() > 1);       // depth was rebuilt, not lost
    REQUIRE(a.book().open_orders() > 2);      // and the book is not draining
    REQUIRE(c.rejects[size_t(lob::Reject::UnknownReference)] <
            c.applied);                       // adds are no longer bouncing
}
