// lm_sim: run a generated OFTK token stream through the token<->action shim
// into the adapter, and WRITE THE RESULT AS AN ITCH FILE that tools/stylized
// can replay. This is the LM column's entry into Phase 3.
//
// Why it must exist: the real column is a real ITCH day and the null column
// is cst_sim's emitted ITCH; both reach the stylized facts through
// tools/stylized. shim_drive and sim_health only MEASURE a token stream -
// neither produces anything stylized can read. Scoring the LM through a
// different path than real and null would make the headline table a
// comparison of pipelines as much as of models, so the LM goes through the
// identical one.
//
// The messages are the engine's own account of what it did (Adapter's opt-in
// ITCH journal), not a reconstruction: match_submit's emission is
// reconstruction-closed and fuzz-verified, and the resting-add / cancel cases
// are recorded where the refs are actually known.
//
// THE WARM BOOK IS JOURNALLED FIRST. warm_book applies its snapshot straight
// to the Book, bypassing the adapter, so those orders have no messages. They
// are emitted as opening 'A's at the start timestamp - otherwise the replay
// would rebuild a different book and every cancel touching a warm order would
// reject against a ref that was never added.
//
// TIMESTAMPS: the DT token's dt_ns advances a caller-owned clock. shim::drive
// reports each tuple AFTER it is applied, so a tuple is stamped with the
// clock as of the previous tuple's dt - a uniform one-event lag. Monotonic,
// total elapsed time exact, and the multiset of inter-arrival gaps unchanged,
// which is what the stylized estimators consume.
//
// Usage:
//   lm_sim <tokens.bin> --manifest M --out FILE [--ticker T]
//          [--warm-start snap.book] [--locate N] [--start-ns NS]
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "../src/adapter.hpp"
#include "../src/bookset.hpp"
#include "../src/itch.hpp"
#include "../src/itch_replay.hpp"
#include "../src/token_shim.hpp"
#include "../src/warm_book.hpp"

namespace {

// 'R' Stock Directory frame, hand-built exactly as cst_sim does, so stylized
// can name the symbol. The codec decodes only the 7 book-affecting types, so
// this record is assembled by hand rather than through itch::encode.
void put_directory(std::vector<uint8_t>& buf, uint16_t locate,
                   const itch::Stock& stock) {
    uint8_t body[39] = {0};
    body[0] = 'R';
    itch::put_u16(body + 1, locate);
    itch::put_u16(body + 3, 0);
    for (int i = 0; i < 6; ++i) body[5 + i] = 0;      // timestamp 0
    for (int i = 0; i < 8; ++i) body[11 + i] = uint8_t(stock[size_t(i)]);
    body[19] = 'Q';   // market category
    body[20] = ' ';   // financial status
    itch::put_u32(body + 21, 100);                    // round lot size
    body[25] = 'N';
    buf.push_back(uint8_t(sizeof(body) >> 8));
    buf.push_back(uint8_t(sizeof(body) & 0xFF));
    buf.insert(buf.end(), body, body + sizeof(body));
}

}  // namespace

int main(int argc, char** argv) {
    const char* bin_path = nullptr;
    std::string manifest_path, ticker, warm_path, out_path;
    uint16_t locate = 1;
    uint64_t start_ns = 34'200'000'000'000ull;  // 09:30:00.000000000
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", what);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--manifest") manifest_path = next("--manifest");
        else if (a == "--ticker") ticker = next("--ticker");
        else if (a == "--out") out_path = next("--out");
        else if (a == "--warm-start") warm_path = next("--warm-start");
        else if (a == "--locate") locate = uint16_t(std::atoi(next("--locate")));
        else if (a == "--start-ns")
            start_ns = uint64_t(std::strtoull(next("--start-ns"), nullptr, 10));
        else if (!bin_path) bin_path = argv[i];
        else {
            std::fprintf(stderr, "unexpected argument %s\n", argv[i]);
            return 2;
        }
    }
    if (!bin_path || manifest_path.empty() || out_path.empty()) {
        std::fprintf(stderr,
                     "usage: lm_sim <tokens.bin> --manifest M --out FILE "
                     "[--ticker T] [--warm-start snap.book] [--locate N] "
                     "[--start-ns NS]\n");
        return 2;
    }

    std::ifstream in(bin_path, std::ios::binary);
    std::string tk;
    std::vector<uint16_t> tokens;
    std::string err;
    if (!in || !oftk::read_token_bin(in, tk, tokens, &err)) {
        std::fprintf(stderr, "%s: %s\n", bin_path, err.c_str());
        return 1;
    }
    if (ticker.empty()) ticker = tk;

    std::ifstream mf(manifest_path);
    oftk::Manifest m;
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

    itch::Stock stock = {' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
    for (size_t i = 0; i < ticker.size() && i < 8; ++i)
        stock[i] = ticker[i];

    lob::Adapter a;
    std::vector<itch::Message> warm_msgs;
    if (!warm_path.empty()) {
        warm::Snapshot snap;
        std::ifstream ws(warm_path);
        if (!ws || !warm::read(ws, snap, &err) ||
            !warm::apply(snap, a.book(), &err)) {
            std::fprintf(stderr, "warm start %s: %s\n", warm_path.c_str(),
                         err.c_str());
            return 1;
        }
        if (!snap.ticker.empty() && snap.ticker != ticker) {
            std::fprintf(stderr,
                         "warm snapshot is %s but stream is %s - refused\n",
                         snap.ticker.c_str(), ticker.c_str());
            return 1;
        }
        // Journal the warm book as opening adds, in book order, under the
        // refs warm_book actually assigned.
        a.book().for_each_level([&](lob::Side s, const lob::Level& lv) {
            for (const lob::Order* o = lv.head; o; o = o->next) {
                itch::AddOrder ad;
                ad.h.stock_locate = locate;
                ad.h.timestamp    = start_ns;
                ad.order_ref      = o->ref;
                ad.side           = s == lob::Side::Buy ? 'B' : 'S';
                ad.shares         = o->shares;
                ad.stock          = stock;
                ad.price          = o->price;
                warm_msgs.push_back(ad);
            }
            return true;
        });
        std::printf("warm start: %s %s -> %zu orders, %zu/%zu levels\n",
                    snap.ticker.c_str(), snap.day.c_str(),
                    a.book().open_orders(), a.book().bid_levels(),
                    a.book().ask_levels());
    } else {
        std::fprintf(stderr,
                     "refusing to run cold: pass --warm-start (a cold two-order"
                     " seed kills even the REAL token stream - RESULTS.md "
                     "Step 1)\n");
        return 2;
    }

    a.journal_enable(locate, stock);
    uint64_t clock_ns = start_ns;
    a.journal_time(clock_ns);

    shim::Counts c;
    shim::drive(a, tokens, bins, c,
                [&](size_t, const shim::Resolution& r, const lob::Outcome&) {
                    int64_t dt = r.event.dt_ns;
                    if (dt > 0) clock_ns += uint64_t(dt);
                    a.journal_time(clock_ns);
                });

    std::printf("stream: %zu tokens (%s), %" PRIu64 " tuples, %" PRIu64
                " applied, %" PRIu64 " rejected; clock %.1f s\n",
                tokens.size(), ticker.c_str(), c.tuples, a.applied(),
                a.rejected(), double(clock_ns - start_ns) / 1e9);

    // ---- encode: directory, warm book, then the model's flow --------------
    std::vector<uint8_t> out_buf;
    put_directory(out_buf, locate, stock);
    for (const itch::Message& msg : warm_msgs) itch::encode_framed(msg, out_buf);
    for (const itch::Message& msg : a.journal()) itch::encode_framed(msg, out_buf);

    // ---- self-verification, the same bar cst_sim holds itself to ----------
    {
        lob::BookSet set;
        itch::FrameReader rd{out_buf.data(), out_buf.size()};
        uint64_t applied = 0;
        while (auto msg = rd.next()) {
            if (set.apply(*msg) != lob::Result::Ok) {
                std::fprintf(stderr, "SELF-CHECK REJECT at msg %" PRIu64
                             " - lm_sim bug\n", applied);
                return 1;
            }
            ++applied;
        }
        if (rd.error) {
            std::fprintf(stderr, "SELF-CHECK STREAM ERROR\n");
            return 1;
        }
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
        // The replayed book must also BE the book the adapter ended with.
        bool same = false;
        set.for_each_book([&](uint16_t, lob::Book& bk) {
            same = bk.open_orders() == a.book().open_orders() &&
                   bk.best_bid() == a.book().best_bid() &&
                   bk.best_ask() == a.book().best_ask() &&
                   bk.shares_resting() == a.book().shares_resting();
        });
        if (!same) {
            std::fprintf(stderr, "SELF-CHECK REPLAY != ADAPTER BOOK\n");
            return 1;
        }
        std::printf("self-check: %" PRIu64 " msgs replayed, 0 rejects, "
                    "conservation exact, book identical\n", applied);
    }

    FILE* out = std::fopen(out_path.c_str(), "wb");
    if (!out) { std::perror(out_path.c_str()); return 1; }
    if (std::fwrite(out_buf.data(), 1, out_buf.size(), out) != out_buf.size()) {
        std::perror("fwrite");
        std::fclose(out);
        return 1;
    }
    std::fclose(out);
    std::printf("wrote %s (%.1f MB, %zu warm + %zu flow msgs)\n",
                out_path.c_str(), double(out_buf.size()) / 1e6,
                warm_msgs.size(), a.journal().size());
    return 0;
}
