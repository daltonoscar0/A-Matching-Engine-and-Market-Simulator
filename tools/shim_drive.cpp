// shim_drive: run a generated OFTK token stream through the token<->action
// shim into the adapter closed loop and report the merged rejection
// breakdown (shim-level + adapter-level, same categories). This is the
// measurement half of the Phase 6 pilot: "sampling produces a stream the
// adapter accepts" is quantified here, category by category.
//
// The book starts EMPTY and is seeded one of two ways:
//   default      two resting orders at a configurable mid/spread - ONE
//                occupied level per side, so PRICE_OFF 1..10 can never
//                resolve (this is the cold start the 2026-07-31 control
//                showed kills even the REAL token stream, at tuple 3);
//   --warm-start a real 09:30 resting book from tools/warm_book.
// See src/warm_book.hpp for why the default is a harness artifact.
//
// Usage:
//   shim_drive <tokens.bin> --manifest M [--ticker T] [--seed-mid 1000000]
//              [--warm-start snapshot.book]
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "../src/token_shim.hpp"
#include "../src/warm_book.hpp"

int main(int argc, char** argv) {
    const char* bin_path = nullptr;
    std::string manifest_path, ticker, warm_path;
    uint32_t seed_mid = 1'000'000;  // $100.00 ITCH fixed point
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
        else if (a == "--seed-mid")
            seed_mid = uint32_t(std::atoll(next("--seed-mid")));
        else if (a == "--warm-start") warm_path = next("--warm-start");
        else if (!bin_path) bin_path = argv[i];
        else {
            std::fprintf(stderr, "unexpected argument %s\n", argv[i]);
            return 2;
        }
    }
    if (!bin_path || manifest_path.empty()) {
        std::fprintf(stderr,
                     "usage: shim_drive <tokens.bin> --manifest M "
                     "[--ticker T] [--seed-mid PX] "
                     "[--warm-start snap.book]\n");
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
    const uint32_t tick = uint32_t(bins.tick_size > 0 ? bins.tick_size : 100);

    lob::Adapter a;
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
        std::printf("warm start: %s %s -> %zu orders, %zu/%zu levels\n",
                    snap.ticker.c_str(), snap.day.c_str(),
                    a.book().open_orders(), a.book().bid_levels(),
                    a.book().ask_levels());
    } else if (!a.submit({lob::ActionKind::Limit, lob::Side::Buy,
                          seed_mid - tick, 100}).applied() ||
               !a.submit({lob::ActionKind::Limit, lob::Side::Sell,
                          seed_mid + tick, 100}).applied()) {
        std::fprintf(stderr, "seeding failed\n");
        return 1;
    }

    shim::Counts c;
    shim::drive(a, tokens, bins, c);

    const uint64_t evald = c.tuples;
    std::printf("stream: %zu tokens (%s), %" PRIu64 " tuples, %" PRIu64
                " specials skipped, %" PRIu64 " resyncs\n",
                tokens.size(), ticker.c_str(), evald, c.specials, c.resyncs);
    std::printf("applied: %" PRIu64 " (%.2f%% of tuples)   rejected: %" PRIu64
                "\n",
                c.applied, evald ? 100.0 * double(c.applied) / double(evald)
                                 : 0.0,
                c.rejected());
    for (size_t r = 1; r < size_t(lob::Reject::kCount); ++r)
        std::printf("  %-18s %" PRIu64 " (%.2f%%)\n",
                    lob::to_string(lob::Reject(r)), c.rejects[r],
                    evald ? 100.0 * double(c.rejects[r]) / double(evald)
                          : 0.0);
    const lob::Book& b = a.book();
    std::printf("book end state: %zu open orders, best %u/%u, audit %s; "
                "invariants %s\n",
                b.open_orders(), b.best_bid(), b.best_ask(),
                b.audit().empty() ? "clean" : "DIRTY",
                b.invariants_fast() ? "ok" : "VIOLATED");
    return b.audit().empty() && b.invariants_fast() ? 0 : 1;
}
