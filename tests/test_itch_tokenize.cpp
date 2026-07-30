// The ITCH -> OFTK ingest and its round-trip test (the match()-equivalent
// for the tokenizer pipeline). Layer 1: the exact expanded event stream
// (U -> Delete+Add) drives a fresh Book whose per-event running
// fingerprint must equal the raw-ITCH-driven Book's, with PRICE_OFF
// independently recomputed from the fresh book. Layer 2: the tokens must
// be the exact quantization of that stream and invert per tape's
// roundtrip contract. Both layers are mutation-verified here: an
// adjacent-event swap must break layer 1, a PRICE_OFF off-by-one must
// break the recompute (event side) and layer 2 (token side).
// Synthetic streams (src/synth.hpp) keep this in the always-on ctest
// gate; a real-day slice runs when data/ is present.
#include <cstdio>
#include <sstream>
#include <vector>

#include "../src/itch_tokenize.hpp"
#include "../src/oftk_fit.hpp"
#include "../src/synth.hpp"
#include "../third_party/catch.hpp"

namespace {

// A valid synthetic stream, framed like a day file. Generator timestamps
// start at 09:30 and step 1..5000ns, so events land in-window.
std::vector<uint8_t> synth_wire(size_t n_msgs, uint64_t seed,
                                uint16_t locate) {
    synth::Config cfg;
    cfg.seed = seed;
    cfg.locate = locate;
    synth::Generator gen(cfg);
    std::vector<uint8_t> wire;
    wire.reserve(n_msgs * 40);
    for (size_t i = 0; i < n_msgs; ++i) {
        bool ok = true;
        itch::Message m = gen.next(ok);
        REQUIRE(ok);
        itch::encode_framed(m, wire);
    }
    return wire;
}

oftk::TickerBins fit_bins_from(const ingest::IngestResult& r) {
    std::vector<int64_t> sizes, dts;
    for (const ingest::Event& e : r.events) {
        if (!e.in_win) continue;
        sizes.push_back(int64_t(e.size));
        if (e.dt_ns > 0) dts.push_back(e.dt_ns);
    }
    REQUIRE(!sizes.empty());
    oftk::TickerBins b;
    b.size_edges = oftk::fit::fit_size_edges(sizes);
    b.dt_edges_ns = oftk::fit::fit_dt_edges(dts);
    REQUIRE(b.valid());
    return b;
}

bool layer1_pass(const std::vector<ingest::Event>& evs,
                 const std::vector<uint8_t>& wire, uint16_t locate,
                 std::string* why, size_t max_msgs = 0) {
    ingest::detail::Fingerprint fe, fr;
    lob::Book fresh, raw;
    std::string err;
    if (!ingest::replay_events(evs, fresh, true, fe, &err)) {
        if (why) *why = "replay: " + err;
        return false;
    }
    if (!ingest::replay_raw_symbol(wire.data(), wire.size(), locate, raw, fr,
                                   &err, max_msgs)) {
        if (why) *why = "raw: " + err;
        return false;
    }
    if (fe.folds != fr.folds) {
        if (why) *why = "fold count";
        return false;
    }
    if (fe.h != fr.h) {
        if (why) *why = "fingerprint";
        return false;
    }
    return true;
}

const char* find_data_file() {
    static const char* candidates[] = {
        "data/20190730.BX_ITCH_50",
        "../data/20190730.BX_ITCH_50",
    };
    for (const char* p : candidates) {
        if (FILE* f = std::fopen(p, "rb")) {
            std::fclose(f);
            return p;
        }
    }
    return nullptr;
}

}  // namespace

TEST_CASE("tokenize: level_index follows the tape rule") {
    lob::Book b;
    // Bids: 10100 (best), 10000, 9900. Asks: 10200 (best), 10400.
    REQUIRE(b.add(1, lob::Side::Buy, 100, 10100) == lob::Result::Ok);
    REQUIRE(b.add(2, lob::Side::Buy, 100, 10000) == lob::Result::Ok);
    REQUIRE(b.add(3, lob::Side::Buy, 100, 9900) == lob::Result::Ok);
    REQUIRE(b.add(4, lob::Side::Sell, 100, 10200) == lob::Result::Ok);
    REQUIRE(b.add(5, lob::Side::Sell, 100, 10400) == lob::Result::Ok);

    int64_t off = 99;
    // At same-side best -> 0.
    REQUIRE(ingest::detail::level_index(b, lob::Side::Buy, 10100, off));
    CHECK(off == 0);
    // One occupied level strictly better -> +1 (both at-level and between).
    REQUIRE(ingest::detail::level_index(b, lob::Side::Buy, 10000, off));
    CHECK(off == 1);
    REQUIRE(ingest::detail::level_index(b, lob::Side::Buy, 10050, off));
    CHECK(off == 1);
    // Deeper than every level -> insertion rank == level count.
    REQUIRE(ingest::detail::level_index(b, lob::Side::Buy, 9000, off));
    CHECK(off == 3);
    // Better than the best -> -1 (inside the spread), both sides.
    REQUIRE(ingest::detail::level_index(b, lob::Side::Buy, 10150, off));
    CHECK(off == -1);
    REQUIRE(ingest::detail::level_index(b, lob::Side::Sell, 10150, off));
    CHECK(off == -1);
    // Ask side ranks by ascending price.
    REQUIRE(ingest::detail::level_index(b, lob::Side::Sell, 10400, off));
    CHECK(off == 1);
    // Empty side -> no index (PRICE_OFF = UNK).
    lob::Book empty;
    REQUIRE(empty.add(9, lob::Side::Buy, 100, 10000) == lob::Result::Ok);
    CHECK_FALSE(ingest::detail::level_index(empty, lob::Side::Sell, 10000,
                                            off));
}

TEST_CASE("tokenize: every legal 5-tuple round-trips through decode") {
    oftk::TickerBins b;
    b.size_edges = {100, 200, 201, 387, 500, 501, 1000};
    b.dt_edges_ns = {266,     769,      2226,     6438,     18623,
                     53869,   155821,   450731,   1303792,  3771368,
                     10909121, 31555901, 91279112, 264035437};
    REQUIRE(b.valid());
    size_t checked = 0;
    for (uint16_t ty = oftk::TYPE_BASE; ty < oftk::SIDE_BASE; ++ty)
        for (uint16_t sd = oftk::SIDE_BASE; sd <= oftk::SIDE_BASE + 1; ++sd)
            for (uint16_t px = 0; px <= oftk::PX_TAIL;
                 px = (px == 0 ? oftk::PX_INSIDE : px + 1))
                for (uint16_t sz = oftk::SZ_BASE; sz < oftk::DT_BASE; ++sz)
                    for (uint16_t dt = oftk::DT_ZERO; dt <= oftk::DT_TAIL;
                         ++dt) {
                        uint16_t t[5] = {ty, sd, px, sz, dt};
                        REQUIRE(oftk::roundtrip_ok(t, b));
                        ++checked;
                    }
    CHECK(checked == 6u * 2 * 14 * 8 * 16);
}

TEST_CASE("tokenize: OFTK binary and manifest round-trip") {
    std::vector<uint16_t> toks = {oftk::BOS, oftk::SESSION_OPEN, 7,  13,
                                  16,        30,                 40, oftk::SESSION_CLOSE,
                                  oftk::EOS};
    std::stringstream ss;
    REQUIRE(oftk::write_token_bin(ss, "SPY", toks));
    std::string ticker;
    std::vector<uint16_t> back;
    std::string err;
    REQUIRE(oftk::read_token_bin(ss, ticker, back, &err));
    CHECK(ticker == "SPY");
    CHECK(back == toks);

    oftk::Manifest m;
    oftk::TickerEntry e;
    e.bins.tick_size = 100;
    e.bins.size_edges = {100, 200, 201, 387, 500, 501, 1000};
    e.bins.dt_edges_ns = {266,      769,      2226,     6438,     18623,
                          53869,    155821,   450731,   1303792,  3771368,
                          10909121, 31555901, 91279112, 264035437};
    e.fit.messages_file = "20190130.BX_ITCH_50";
    e.fit.train_frac = 1.0;
    e.fit.rows_total = 42;
    e.fit.train_end_index = 42;
    e.fit.last_train_time_ns = 57599000000000;
    e.fit.first_eval_time_ns = 57599000000001;
    m.tickers["SPY"] = e;
    std::stringstream j1, j2;
    oftk::write_manifest(j1, m);
    oftk::Manifest m2;
    REQUIRE(oftk::load_manifest(j1, m2, &err));
    oftk::write_manifest(j2, m2);
    // write -> load -> write is byte-identical: the loader consumes exactly
    // what tape's strict parser will see.
    CHECK(j1.str() == j2.str());
    REQUIRE(m2.tickers.count("SPY") == 1);
    CHECK(m2.tickers["SPY"].bins.size_edges == e.bins.size_edges);
}

TEST_CASE("tokenize: U expands to Delete+Add and replays identically") {
    const uint16_t locate = 1;
    std::vector<uint8_t> wire = synth_wire(50'000, 42, locate);
    ingest::IngestResult res;
    std::string err;
    REQUIRE(ingest::ingest_day(wire.data(), wire.size(), locate, nullptr, res,
                               &err));
    REQUIRE(res.u_expanded > 0);  // synth mixes ~12% U
    CHECK(res.events.size() == res.msgs_symbol + res.u_expanded);

    // Every expanded pair is Delete then Add, same side, same timestamp.
    for (size_t i = 0; i < res.events.size(); ++i) {
        if (!res.events[i].pair_first) continue;
        REQUIRE(i + 1 < res.events.size());
        const ingest::Event& d = res.events[i];
        const ingest::Event& a = res.events[i + 1];
        REQUIRE(d.type == oftk::MsgType::Delete);
        REQUIRE(a.type == oftk::MsgType::Add);
        REQUIRE(d.direction == a.direction);
        REQUIRE(d.ts == a.ts);
        if (a.in_win) REQUIRE(a.dt_ns == 0);
    }

    // Layer 1: expanded stream == atomic-replace raw stream, state-for-state.
    std::string why;
    CHECK(layer1_pass(res.events, wire, locate, &why));
    if (!why.empty()) WARN(why);
}

TEST_CASE("tokenize: synthetic round-trip, both layers, plus fit/apply") {
    const uint16_t locate = 7;
    std::vector<uint8_t> wire = synth_wire(80'000, 1234, locate);

    // Fit-mode pass collects the stream; bins are fitted from it, then the
    // apply pass tokenizes - the same fit -> freeze -> apply order the
    // tools enforce.
    ingest::IngestResult fitpass;
    std::string err;
    REQUIRE(ingest::ingest_day(wire.data(), wire.size(), locate, nullptr,
                               fitpass, &err));
    oftk::TickerBins bins = fit_bins_from(fitpass);

    ingest::IngestResult res;
    REQUIRE(ingest::ingest_day(wire.data(), wire.size(), locate, &bins, res,
                               &err));
    REQUIRE(res.events_inwindow > 0);
    REQUIRE(res.tokens.size() ==
            4 + res.events_inwindow * oftk::kEventTokens);

    std::string why;
    CHECK(layer1_pass(res.events, wire, locate, &why));
    if (!why.empty()) WARN(why);
    REQUIRE(ingest::verify_tokens(res, bins, &err));
}

TEST_CASE("tokenize: the round-trip test fails under deliberate mutation") {
    const uint16_t locate = 3;
    std::vector<uint8_t> wire = synth_wire(60'000, 777, locate);
    ingest::IngestResult fitpass;
    std::string err;
    REQUIRE(ingest::ingest_day(wire.data(), wire.size(), locate, nullptr,
                               fitpass, &err));
    oftk::TickerBins bins = fit_bins_from(fitpass);
    ingest::IngestResult res;
    REQUIRE(ingest::ingest_day(wire.data(), wire.size(), locate, &bins, res,
                               &err));
    std::string why;
    REQUIRE(layer1_pass(res.events, wire, locate, &why));
    REQUIRE(ingest::verify_tokens(res, bins, &err));

    SECTION("swap two adjacent events -> layer 1 fails") {
        std::vector<ingest::Event> mut = res.events;
        size_t at = mut.size();
        for (size_t i = 0; i + 1 < mut.size(); ++i)
            if (!mut[i].pair_first && !mut[i + 1].pair_first &&
                mut[i].price != mut[i + 1].price) {
                std::swap(mut[i], mut[i + 1]);
                at = i;
                break;
            }
        REQUIRE(at < mut.size());
        CHECK_FALSE(layer1_pass(mut, wire, locate, nullptr));
    }

    SECTION("PRICE_OFF off by one level (event side) -> recompute fails") {
        std::vector<ingest::Event> mut = res.events;
        size_t at = mut.size();
        for (size_t i = 0; i < mut.size(); ++i)
            if (mut[i].has_px && mut[i].lvl_off >= 0 &&
                mut[i].lvl_off < oftk::kPxMax) {
                mut[i].lvl_off += 1;
                at = i;
                break;
            }
        REQUIRE(at < mut.size());
        ingest::detail::Fingerprint fp;
        lob::Book fresh;
        CHECK_FALSE(ingest::replay_events(mut, fresh, true, fp, nullptr));
    }

    SECTION("PRICE_OFF off by one level (token side) -> layer 2 fails") {
        ingest::IngestResult mut = res;
        size_t k = 0, tok_at = SIZE_MAX;
        for (const ingest::Event& e : mut.events) {
            if (!e.in_win) continue;
            if (e.has_px && e.lvl_off >= 0 && e.lvl_off < oftk::kPxMax) {
                tok_at = k;
                break;
            }
            ++k;
        }
        REQUIRE(tok_at != SIZE_MAX);
        mut.tokens[2 + tok_at * oftk::kEventTokens + 2] += 1;
        CHECK_FALSE(ingest::verify_tokens(mut, bins, nullptr));
    }
}

TEST_CASE("tokenize: real BX slice round-trip (skips when data absent)") {
    const char* path = find_data_file();
    if (!path) {
        WARN("data/20190730.BX_ITCH_50 not present; skipping real-data "
             "tokenize test");
        return;
    }
    constexpr size_t SLICE_BYTES = 64u << 20;
    constexpr size_t MAX_MSGS = 200'000;
    std::vector<uint8_t> wire;
    {
        FILE* f = std::fopen(path, "rb");
        REQUIRE(f != nullptr);
        wire.resize(SLICE_BYTES);
        size_t n = std::fread(wire.data(), 1, wire.size(), f);
        std::fclose(f);
        wire.resize(n);
    }
    bool framing_error = false;
    auto dir =
        lob::scan_stock_directory(wire.data(), wire.size(), framing_error);
    REQUIRE(!dir.empty());
    // Busiest locate in the slice = most decoded book messages.
    std::vector<uint32_t> counts(1 << 16, 0);
    {
        itch::FrameReader rd{wire.data(), wire.size()};
        size_t seen = 0;
        while (auto m = rd.next()) {
            if (++seen > MAX_MSGS) break;
            ++counts[lob::locate_of(*m)];
        }
    }
    uint16_t locate = 0;
    for (size_t i = 1; i < counts.size(); ++i)
        if (counts[i] > counts[locate]) locate = uint16_t(i);
    REQUIRE(counts[locate] > 1000);

    ingest::IngestResult fitpass;
    std::string err;
    REQUIRE(ingest::ingest_day(wire.data(), wire.size(), locate, nullptr,
                               fitpass, &err, MAX_MSGS));
    // Layer 1 needs no bins and no window; always run it.
    std::string why;
    CHECK(layer1_pass(fitpass.events, wire, locate, &why, MAX_MSGS));
    if (!why.empty()) WARN(why);
    // Layer 2 needs in-window events to fit bins on; the slice may sit
    // pre-open, in which case there is nothing to tokenize.
    if (fitpass.events_inwindow < 1000) {
        WARN("slice has <1000 in-window events; layer 2 skipped");
        return;
    }
    oftk::TickerBins bins = fit_bins_from(fitpass);
    ingest::IngestResult res;
    REQUIRE(ingest::ingest_day(wire.data(), wire.size(), locate, &bins, res,
                               &err, MAX_MSGS));
    REQUIRE(ingest::verify_tokens(res, bins, &err));
}
