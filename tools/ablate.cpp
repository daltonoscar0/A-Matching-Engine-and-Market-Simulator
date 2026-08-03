// ablate: WHICH quantization destroys the mid-price stylized facts?
//
// Step 8 measured that the real token stream, pushed through the LM pipeline,
// keeps flow-sign memory intact but flattens volatility clustering to ~0 and
// inflates kurtosis ~1000x. That is an end-to-end fact about the pipeline; it
// does not say which stage is responsible, and the vocab decision hangs on the
// answer. This tool replays the REAL day's exact event stream with ONE
// quantization applied at a time and writes ITCH for tools/stylized.
//
//   --mode none   exact prices, sizes and timestamps. THE CONTROL: it must
//                 reproduce the real column. If it does not, this harness is
//                 wrong and no other mode means anything.
//   --mode dt     timestamps rebuilt from bucketed dt (bucket_rep), exact
//                 prices/sizes. Calendar-time facts only; tick-time facts are
//                 timestamp-free and must be UNCHANGED from none.
//   --mode size   sizes replaced by their bucket representative.
//   --mode pxadd  ADD prices replaced by the PRICE_OFF decode against the
//                 reconstructed book (the shim's inverse, nearest achievable
//                 index). THE DECISIVE ONE for the vocabulary question:
//                 PRICE_OFF cannot express an interior level, and this is
//                 exactly that loss, with everything else exact.
//   --mode all    every quantization at once; should approach the Step 8 row.
//   --mode noref  ORDER IDENTITY DROPPED, everything else exact: a cancel /
//                 delete / execute takes the FIFO HEAD of the level the real
//                 event named, not the order it named. Prices, sizes and
//                 timestamps stay exact, so this isolates ONE variable.
//                 Added 2026-08-03 to test the standing hypothesis in
//                 RESULTS.md: every other mode keeps exact refs, and only the
//                 generation path does not, so if losing identity alone
//                 flattens tick-time volatility clustering then the loss is
//                 structural to the shim and NOT a vocabulary property - no
//                 corpus rebuild can address it. The level is resolved by the
//                 event's EXACT price, deliberately: resolving by PRICE_OFF
//                 index instead would confound identity loss with price
//                 quantization, which --mode pxadd already measures.
//
// Non-Add events keep their exact ref (EXCEPT under --mode noref, whose whole
// point is that they do not), so cancels/executes still resolve; a distorted
// size is clamped to the standing order's remaining shares and the clamp is
// COUNTED and printed rather than hidden.
//
// Usage:
//   ablate <itch_day> --ticker T --manifest M --mode M --out FILE
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "../src/bookset.hpp"
#include "../src/dataset.hpp"
#include "../src/itch.hpp"
#include "../src/itch_replay.hpp"
#include "../src/itch_tokenize.hpp"
#include "../src/token_shim.hpp"

namespace {

std::vector<uint8_t> slurp(const char* path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) { std::perror(path); std::exit(1); }
    std::streamsize n = f.tellg();
    f.seekg(0);
    std::vector<uint8_t> v(static_cast<size_t>(n));
    if (!f.read(reinterpret_cast<char*>(v.data()), n)) {
        std::fprintf(stderr, "%s: short read\n", path);
        std::exit(1);
    }
    return v;
}

void put_directory(std::vector<uint8_t>& buf, uint16_t locate,
                   const itch::Stock& stock) {
    uint8_t body[39] = {0};
    body[0] = 'R';
    itch::put_u16(body + 1, locate);
    itch::put_u16(body + 3, 0);
    for (int i = 0; i < 8; ++i) body[11 + i] = uint8_t(stock[size_t(i)]);
    body[19] = 'Q';
    body[20] = ' ';
    itch::put_u32(body + 21, 100);
    body[25] = 'N';
    buf.push_back(uint8_t(sizeof(body) >> 8));
    buf.push_back(uint8_t(sizeof(body) & 0xFF));
    buf.insert(buf.end(), body, body + sizeof(body));
}

// The shim owns no order identity: it can name a LEVEL but not an order, so
// a cancel takes whatever is at the front of that level's queue. Book exposes
// levels read-only via for_each_level, which is enough to model exactly that.
// NEAREST occupied level on that side, not the exact price. The exact-price
// version was tried first and was DEGENERATE: once refs move the book
// diverges, so a cancel names a level that no longer exists, and skipping
// those dropped 113,237 of 273,078 events (41%). That gutted the mid series
// (n_tick 41,517 -> 2,031, vartop10 0.169 -> 0.9966), which is UNMEASURED by
// the project's own rule - the tick ACF collapse it appeared to show was
// indistinguishable from estimator degeneracy. Falling back to the nearest
// occupied level keeps the event stream intact AND is closer to the shim,
// which resolves by nearest-achievable-index rather than rejecting.
const lob::Order* fifo_head_at(const lob::Book& b, lob::Side side,
                               uint32_t price) {
    const lob::Order* head = nullptr;
    const lob::Order* best = nullptr;
    uint64_t best_d = UINT64_MAX;
    b.for_each_level([&](lob::Side s, const lob::Level& lvl) {
        if (s != side || lvl.head == nullptr) return true;
        if (lvl.price == price) { head = lvl.head; return false; }
        const uint64_t d = lvl.price > price ? lvl.price - price
                                             : price - lvl.price;
        if (d < best_d) { best_d = d; best = lvl.head; }
        return true;
    });
    return head ? head : best;
}

}  // namespace

int main(int argc, char** argv) {
    const char* day_path = nullptr;
    std::string ticker, manifest_path, mode = "none", out_path;
    bool override_flag = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", what);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--ticker") ticker = next("--ticker");
        else if (a == "--manifest") manifest_path = next("--manifest");
        else if (a == "--mode") mode = next("--mode");
        else if (a == "--out") out_path = next("--out");
        else if (a == dataset::kOverrideFlag) override_flag = true;
        else if (!day_path) day_path = argv[i];
        else { std::fprintf(stderr, "unexpected argument %s\n", argv[i]); return 2; }
    }
    if (!day_path || ticker.empty() || manifest_path.empty() || out_path.empty()) {
        std::fprintf(stderr,
                     "usage: ablate <itch_day> --ticker T --manifest M "
                     "--mode none|dt|size|pxadd|all|noref --out FILE\n");
        return 2;
    }
    const bool q_dt    = mode == "dt"    || mode == "all";
    const bool q_size  = mode == "size"  || mode == "all";
    const bool q_pxadd = mode == "pxadd" || mode == "all";
    // noref is deliberately NOT part of "all": "all" names the quantizations,
    // and prior RESULTS rows were produced with it. Folding a new distortion
    // into an existing mode would silently re-date those rows.
    const bool q_noref = mode == "noref";
    if (mode != "none" && !q_dt && !q_size && !q_pxadd && !q_noref) {
        std::fprintf(stderr, "unknown --mode %s\n", mode.c_str());
        return 2;
    }
    dataset::enforce(day_path, override_flag);

    std::vector<uint8_t> wire = slurp(day_path);
    bool dir_err = false;
    auto dir = lob::scan_stock_directory(wire.data(), wire.size(), dir_err);
    uint16_t locate = 0;
    itch::Stock stock = {' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
    for (const auto& d : dir) {
        std::string s(d.stock.data(), 8);
        while (!s.empty() && s.back() == ' ') s.pop_back();
        if (s == ticker) { locate = d.h.stock_locate; stock = d.stock; break; }
    }
    if (!locate) {
        std::fprintf(stderr, "ticker %s not in stock directory\n", ticker.c_str());
        return 1;
    }

    std::ifstream mf(manifest_path);
    oftk::Manifest m;
    std::string err;
    if (!mf || !oftk::load_manifest(mf, m, &err)) {
        std::fprintf(stderr, "manifest: %s\n", err.c_str());
        return 1;
    }
    auto it = m.tickers.find(ticker);
    if (it == m.tickers.end()) {
        std::fprintf(stderr, "no bins for %s in manifest\n", ticker.c_str());
        return 1;
    }
    const oftk::TickerBins& bins = it->second.bins;

    ingest::IngestResult res;
    if (!ingest::ingest_day(wire.data(), wire.size(), locate, &bins, res, &err)) {
        std::fprintf(stderr, "ingest FAILED: %s\n", err.c_str());
        return 1;
    }
    std::printf("%s %s locate %u: %zu events (%" PRIu64 " in-window), mode %s\n",
                day_path, ticker.c_str(), locate, res.events.size(),
                res.events_inwindow, mode.c_str());

    // ---- replay with the selected quantization, emitting ITCH -------------
    lob::Book book;
    std::vector<uint8_t> out_buf;
    put_directory(out_buf, locate, stock);
    uint64_t clock_ns = 0;
    bool have_clock = false;
    uint64_t match_seq = 1;
    uint64_t emitted = 0, skipped = 0, clamped = 0, px_moved = 0;
    uint64_t ref_moved = 0;  // noref: events that hit a DIFFERENT order
    // Skip provenance: a 38% skip rate is only interpretable if we know WHY.
    uint64_t sk_noorder = 0, sk_zero = 0, sk_reject = 0;

    for (const ingest::Event& e : res.events) {
        const lob::Side side = e.direction > 0 ? lob::Side::Buy : lob::Side::Sell;

        // ---- timestamp ----
        uint64_t ts = e.ts;
        if (q_dt) {
            if (!have_clock) { clock_ns = e.ts; have_clock = true; }
            else if (e.in_win && e.dt_ns > 0) {
                // Quantize the true gap the way the tokenizer does, then
                // invert it - the same lossy path a generated stream takes.
                uint16_t tok[5];
                oftk::ApproxEvent probe;
                probe.type = e.type; probe.direction = e.direction;
                probe.has_ref = e.has_px; probe.lvl_off = e.lvl_off;
                probe.size = e.size; probe.dt_ns = e.dt_ns;
                if (oftk::encode_event(probe, bins, tok)) {
                    oftk::ApproxEvent back;
                    if (oftk::decode_event(tok, bins, back) && back.dt_ns > 0)
                        clock_ns += uint64_t(back.dt_ns);
                }
            }
            ts = clock_ns;
        }

        // ---- price ----
        uint32_t price = e.price;
        if (q_pxadd && e.type == oftk::MsgType::Add) {
            // The shim's inverse: PRICE_OFF names an occupied-level index, so
            // "open a new interior level" is not expressible and lands on the
            // level below. Only the ADD side is distorted here.
            if (e.has_px) {
                uint32_t lp = 0;
                int64_t k = e.lvl_off;
                if (k >= 0 && shim::detail::kth_level_price(book, side, k, lp))
                    price = lp;
            }
            if (price != e.price) ++px_moved;
        }

        // ---- size ----
        uint32_t size = e.size;
        if (q_size) {
            oftk::ApproxEvent probe;
            probe.type = e.type; probe.direction = e.direction;
            probe.has_ref = e.has_px; probe.lvl_off = e.lvl_off;
            probe.size = e.size; probe.dt_ns = e.dt_ns;
            uint16_t tok[5];
            oftk::ApproxEvent back;
            if (oftk::encode_event(probe, bins, tok) &&
                oftk::decode_event(tok, bins, back) && back.size > 0)
                size = uint32_t(back.size);
        }

        lob::Result r = lob::Result::Ok;
        itch::Message msg;
        bool have_msg = false;
        switch (e.type) {
            case oftk::MsgType::Add: {
                if (size == 0 || price == 0) { ++skipped; ++sk_zero; continue; }
                r = book.add(e.ref, side, size, price);
                if (r == lob::Result::Ok) {
                    itch::AddOrder ad;
                    ad.h.stock_locate = locate; ad.h.timestamp = ts;
                    ad.order_ref = e.ref;
                    ad.side = side == lob::Side::Buy ? 'B' : 'S';
                    ad.shares = size; ad.stock = stock; ad.price = price;
                    msg = ad; have_msg = true;
                }
                break;
            }
            case oftk::MsgType::PartialCancel: {
                const lob::Order* o = q_noref
                        ? fifo_head_at(book, side, e.price)
                        : book.find(e.ref);
                if (!o) { ++skipped; ++sk_noorder; continue; }
                const uint64_t ref = o->ref;
                if (q_noref && ref != e.ref) ++ref_moved;
                uint32_t sh = size;
                if (sh > o->shares) { sh = o->shares; ++clamped; }
                if (sh == 0) { ++skipped; ++sk_zero; continue; }
                r = book.cancel(ref, sh);
                if (r == lob::Result::Ok) {
                    itch::OrderCancel oc;
                    oc.h.stock_locate = locate; oc.h.timestamp = ts;
                    oc.order_ref = ref; oc.shares = sh;
                    msg = oc; have_msg = true;
                }
                break;
            }
            case oftk::MsgType::Delete: {
                const lob::Order* o = q_noref
                        ? fifo_head_at(book, side, e.price)
                        : book.find(e.ref);
                if (!o) { ++skipped; ++sk_noorder; continue; }
                const uint64_t ref = o->ref;
                if (q_noref && ref != e.ref) ++ref_moved;
                r = book.remove(ref);
                if (r == lob::Result::Ok) {
                    itch::OrderDelete od;
                    od.h.stock_locate = locate; od.h.timestamp = ts;
                    od.order_ref = ref;
                    msg = od; have_msg = true;
                }
                break;
            }
            case oftk::MsgType::ExecVisible: {
                const lob::Order* o = q_noref
                        ? fifo_head_at(book, side, e.price)
                        : book.find(e.ref);
                if (!o) { ++skipped; ++sk_noorder; continue; }
                const uint64_t ref = o->ref;
                if (q_noref && ref != e.ref) ++ref_moved;
                uint32_t sh = size;
                if (sh > o->shares) { sh = o->shares; ++clamped; }
                if (sh == 0) { ++skipped; ++sk_zero; continue; }
                r = book.execute(ref, sh);
                if (r == lob::Result::Ok) {
                    itch::OrderExecuted oe;
                    oe.h.stock_locate = locate; oe.h.timestamp = ts;
                    oe.order_ref = ref; oe.shares = sh;
                    oe.match_num = match_seq++;
                    msg = oe; have_msg = true;
                }
                break;
            }
            default: ++skipped; continue;
        }
        if (r != lob::Result::Ok || !have_msg) { ++skipped; ++sk_reject; continue; }
        itch::encode_framed(msg, out_buf);
        ++emitted;
    }

    std::printf("emitted %" PRIu64 " msgs, skipped %" PRIu64 ", size-clamped %"
                PRIu64 ", add prices moved %" PRIu64 ", refs moved %" PRIu64
                "\n", emitted, skipped, clamped, px_moved, ref_moved);
    if (skipped)
        std::printf("  skip provenance: no-order %" PRIu64 ", zero-qty %"
                    PRIu64 ", engine-reject %" PRIu64 "\n",
                    sk_noorder, sk_zero, sk_reject);

    // ---- self-verification ------------------------------------------------
    {
        lob::BookSet set;
        itch::FrameReader rd{out_buf.data(), out_buf.size()};
        uint64_t applied = 0;
        while (auto msg = rd.next()) {
            if (set.apply(*msg) != lob::Result::Ok) {
                std::fprintf(stderr, "SELF-CHECK REJECT at msg %" PRIu64 "\n",
                             applied);
                return 1;
            }
            ++applied;
        }
        if (rd.error) { std::fprintf(stderr, "SELF-CHECK STREAM ERROR\n"); return 1; }
        bool clean = true;
        uint64_t added = 0, executed = 0, canceled = 0, resting = 0;
        set.for_each_book([&](uint16_t, lob::Book& bk) {
            if (!bk.audit().empty()) clean = false;
            added += bk.shares_added();       executed += bk.shares_executed();
            canceled += bk.shares_canceled(); resting += bk.shares_resting();
        });
        if (!clean || added != executed + canceled + resting) {
            std::fprintf(stderr, "SELF-CHECK AUDIT/CONSERVATION FAIL\n");
            return 1;
        }
        std::printf("self-check: %" PRIu64 " msgs replayed, 0 rejects, "
                    "conservation exact\n", applied);
    }

    FILE* out = std::fopen(out_path.c_str(), "wb");
    if (!out) { std::perror(out_path.c_str()); return 1; }
    if (std::fwrite(out_buf.data(), 1, out_buf.size(), out) != out_buf.size()) {
        std::perror("fwrite"); std::fclose(out); return 1;
    }
    std::fclose(out);
    std::printf("wrote %s (%.1f MB)\n", out_path.c_str(),
                double(out_buf.size()) / 1e6);
    return 0;
}
