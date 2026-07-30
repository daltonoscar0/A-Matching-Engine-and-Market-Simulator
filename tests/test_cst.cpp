// The CST null generator's contract: whatever the rates, the emitted stream
// is a valid ITCH stream that replays through the reconstruction path with
// zero rejects, exact conservation, and an uncrossed book - the same bar
// real data is held to. Statistical properties are Phase 3 material
// (RESULTS.md); these tests pin the mechanics.
#include "../third_party/catch.hpp"

#include <vector>

#include "../src/cst.hpp"
#include "../src/feed.hpp"
#include "../src/itch.hpp"

namespace {

cst::SymbolParams toy_params() {
    cst::SymbolParams sp;
    sp.name = "TOY";
    sp.p0 = 500000;   // $50.00
    sp.half_spread_ticks = 1;
    for (int s = 0; s < 2; ++s) {
        for (int d = 1; d <= 10; ++d) sp.lambda[s][d] = 2.0 / d;
        for (int d = 1; d <= cst::BT; ++d) sp.theta[s][d] = 0.05;
        sp.mu[s] = 0.5;
    }
    sp.add_sz.add(100, 80);
    sp.add_sz.add(200, 15);
    sp.add_sz.add(37, 5);
    sp.mkt_sz.add(100, 70);
    sp.mkt_sz.add(500, 30);
    return sp;
}

}  // namespace

TEST_CASE("cst stream replays with zero rejects and exact conservation",
          "[cst]") {
    auto sp = toy_params();
    std::vector<cst::TsMsg> msgs;
    cst::SimStats st =
        cst::simulate(sp, 7, 42, 1000000000ull, 601000000000ull, msgs);
    REQUIRE(msgs.size() > 1000);          // 10 sim-minutes of flow
    REQUIRE(st.mkts > 0);
    REQUIRE(st.cancels > 0);

    // Timestamps non-decreasing (merge order is what stylized will see).
    for (size_t i = 1; i < msgs.size(); ++i)
        REQUIRE(msgs[i].first >= msgs[i - 1].first);

    lob::Book b;
    for (auto& [ts, m] : msgs)
        REQUIRE(lob::apply(b, m) == lob::Result::Ok);
    REQUIRE(b.audit().empty());
    REQUIRE(b.shares_added() ==
            b.shares_resting() + b.shares_executed() + b.shares_canceled());
    REQUIRE_FALSE(b.crossed());
}

TEST_CASE("cst stream round-trips through encode/decode framing", "[cst]") {
    auto sp = toy_params();
    std::vector<cst::TsMsg> msgs;
    cst::simulate(sp, 7, 43, 1000000000ull, 61000000000ull, msgs);
    std::vector<uint8_t> buf;
    for (auto& [ts, m] : msgs) itch::encode_framed(m, buf);
    lob::Book b;
    size_t pos = 0, n = 0;
    while (pos < buf.size()) {
        uint16_t len = itch::get_u16(buf.data() + pos);
        auto m = itch::decode(buf.data() + pos + 2, len);
        REQUIRE(m.has_value());
        REQUIRE(lob::apply(b, *m) == lob::Result::Ok);
        pos += 2 + len;
        ++n;
    }
    REQUIRE(n == msgs.size());
    REQUIRE(b.audit().empty());
}

TEST_CASE("cst size sampler draws only calibrated sizes", "[cst]") {
    cst::SizeCdf cdf;
    cdf.add(100, 10);
    cdf.add(300, 1);
    std::mt19937_64 rng(1);
    for (int i = 0; i < 1000; ++i) {
        uint32_t s = cdf.sample(rng);
        REQUIRE((s == 100 || s == 300));
    }
    cst::SizeCdf empty;
    REQUIRE(empty.sample(rng) == 100);   // documented fallback
}
