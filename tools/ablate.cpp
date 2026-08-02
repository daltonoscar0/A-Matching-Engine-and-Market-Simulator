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
//
// Non-Add events keep their exact ref, so cancels/executes still resolve; a
// distorted size is clamped to the standing order's remaining shares and the
// clamp is COUNTED and printed rather than hidden.
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
                     "--mode none|dt|size|pxadd|all --out FILE\n");
        return 2;
    }
    const bool q_dt    = mode == "dt"    || mode == "all";
    const bool q_size  = mode == "size"  || mode == "all";
    const bool q_pxadd = mode == "pxadd" || mode == "all";
    if (mode != "none" && !q_dt && !q_size && !q_pxadd) {
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
                if (size == 0 || price == 0) { ++skipped; continue; }
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
                const lob::Order* o = book.find(e.ref);
                if (!o) { ++skipped; continue; }
                uint32_t sh = size;
                if (sh > o->shares) { sh = o->shares; ++clamped; }
                if (sh == 0) { ++skipped; continue; }
                r = book.cancel(e.ref, sh);
                if (r == lob::Result::Ok) {
                    itch::OrderCancel oc;
                    oc.h.stock_locate = locate; oc.h.timestamp = ts;
                    oc.order_ref = e.ref; oc.shares = sh;
                    msg = oc; have_msg = true;
                }
                break;
            }
            case oftk::MsgType::Delete: {
                if (!book.find(e.ref)) { ++skipped; continue; }
                r = book.remove(e.ref);
                if (r == lob::Result::Ok) {
                    itch::OrderDelete od;
                    od.h.stock_locate = locate; od.h.timestamp = ts;
                    od.order_ref = e.ref;
                    msg = od; have_msg = true;
                }
                break;
            }
            case oftk::MsgType::ExecVisible: {
                const lob::Order* o = book.find(e.ref);
                if (!o) { ++skipped; continue; }
                uint32_t sh = size;
                if (sh > o->shares) { sh = o->shares; ++clamped; }
                if (sh == 0) { ++skipped; continue; }
                r = book.execute(e.ref, sh);
                if (r == lob::Result::Ok) {
                    itch::OrderExecuted oe;
                    oe.h.stock_locate = locate; oe.h.timestamp = ts;
                    oe.order_ref = e.ref; oe.shares = sh;
                    oe.match_num = match_seq++;
                    msg = oe; have_msg = true;
                }
                break;
            }
            default: ++skipped; continue;
        }
        if (r != lob::Result::Ok || !have_msg) { ++skipped; continue; }
        itch::encode_framed(msg, out_buf);
        ++emitted;
    }

    std::printf("emitted %" PRIu64 " msgs, skipped %" PRIu64 ", size-clamped %"
                PRIu64 ", add prices moved %" PRIu64 "\n",
                emitted, skipped, clamped, px_moved);

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
