// sim_health: the stationarity diagnostic for generated streams (Step 2,
// 2026-07-31). The pilot showed the failure mode that training does not
// automatically cure: a slight delete bias compounds over a long
// generation, drains the book, and every later tuple rejects
// UnknownReference - while the stylized facts need thousands of market
// orders from a book that is still alive. This tool drives a generated
// OFTK stream through the token<->action shim into the adapter and
// records book health over generation time.
//
// VIABILITY BAR (recorded in RESULTS.md BEFORE any model was run against
// it): a generated stream is VIABLE for stylized-fact measurement iff
//   (V1) it produces >= 500 collapsed market-order signs (the project's
//        measurability threshold), 1ms same-sign collapse on the stream's
//        PSEUDO-CLOCK (accumulated decoded DT representatives - the
//        stream has no real timestamps; choice logged);
//   (V2) the book is two-sided at >= 90% of sampled checkpoints; and
//   (V3) open_orders never reaches 0 (dead book) before the 500th sign.
// Also reported: open_orders drift (OLS slope per 1k tuples) - a stream
// can pass the bar while drifting toward collapse, and the drift number
// is what says whether a LONGER generation would survive.
//
// Usage:
//   sim_health <tokens.bin> --manifest M [--ticker T] [--csv out.csv]
//              [--interval 100] [--seed-mid 1000000]
// Exit: 0 viable, 2 not viable, 1 error.
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "../src/token_shim.hpp"

int main(int argc, char** argv) {
    const char* bin_path = nullptr;
    std::string manifest_path, ticker, csv_path;
    uint32_t seed_mid = 1'000'000;
    size_t interval = 100;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", what);
                std::exit(1);
            }
            return argv[++i];
        };
        if (a == "--manifest") manifest_path = next("--manifest");
        else if (a == "--ticker") ticker = next("--ticker");
        else if (a == "--csv") csv_path = next("--csv");
        else if (a == "--interval")
            interval = size_t(std::atoll(next("--interval")));
        else if (a == "--seed-mid")
            seed_mid = uint32_t(std::atoll(next("--seed-mid")));
        else if (!bin_path) bin_path = argv[i];
        else {
            std::fprintf(stderr, "unexpected argument %s\n", argv[i]);
            return 1;
        }
    }
    if (!bin_path || manifest_path.empty() || interval == 0) {
        std::fprintf(stderr,
                     "usage: sim_health <tokens.bin> --manifest M "
                     "[--ticker T] [--csv f] [--interval N] "
                     "[--seed-mid PX]\n");
        return 1;
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
    if (!a.submit({lob::ActionKind::Limit, lob::Side::Buy, seed_mid - tick,
                   100}).applied() ||
        !a.submit({lob::ActionKind::Limit, lob::Side::Sell, seed_mid + tick,
                   100}).applied()) {
        std::fprintf(stderr, "seeding failed\n");
        return 1;
    }

    std::ofstream csv;
    if (!csv_path.empty()) {
        csv.open(csv_path);
        csv << "tuple,pseudo_t_ns,open_orders,bid_levels,ask_levels,"
               "best_bid,best_ask,spread,two_sided,applied,unparseable,"
               "unknown_ref,invariant,absurd,signs\n";
    }

    // Health series + sign collapse on the pseudo-clock.
    shim::Counts c;
    uint64_t pseudo_t = 0;
    uint64_t signs = 0;
    int8_t last_sign = 0;
    uint64_t last_sign_t = 0;
    bool have_sign = false;
    uint64_t checkpoints = 0, two_sided_at = 0;
    uint64_t dead_at_tuple = 0;  // first tuple index with open_orders == 0
    bool died_before_500 = false;
    // OLS accumulators for open_orders vs tuple index (per checkpoint).
    double sx = 0, sy = 0, sxx = 0, sxy = 0;

    auto hook = [&](size_t idx, const shim::Resolution& r,
                    const lob::Outcome& o) {
        if (r.ok || r.reject != lob::Reject::Unparseable)
            pseudo_t += uint64_t(r.event.dt_ns > 0 ? r.event.dt_ns : 0);
        if (o.applied() && o.filled > 0 &&
            (r.action.kind == lob::ActionKind::Market ||
             r.action.kind == lob::ActionKind::Limit)) {
            const int8_t s =
                r.action.side == lob::Side::Buy ? int8_t(1) : int8_t(-1);
            if (!have_sign || s != last_sign ||
                pseudo_t - last_sign_t > 1'000'000ull)
                ++signs;
            have_sign = true;
            last_sign = s;
            last_sign_t = pseudo_t;
        }
        const lob::Book& b = a.book();
        if (b.open_orders() == 0 && dead_at_tuple == 0) {
            dead_at_tuple = idx + 1;
            if (signs < 500) died_before_500 = true;
        }
        if ((idx + 1) % interval == 0) {
            ++checkpoints;
            const bool two = b.best_bid() != 0 && b.best_ask() != 0;
            if (two) ++two_sided_at;
            const double x = double(idx + 1), y = double(b.open_orders());
            sx += x; sy += y; sxx += x * x; sxy += x * y;
            if (csv.is_open()) {
                uint32_t spread = two ? b.best_ask() - b.best_bid() : 0;
                csv << (idx + 1) << ',' << pseudo_t << ','
                    << b.open_orders() << ',' << b.bid_levels() << ','
                    << b.ask_levels() << ',' << b.best_bid() << ','
                    << b.best_ask() << ',' << spread << ',' << (two ? 1 : 0)
                    << ',' << c.applied << ','
                    << c.rejects[size_t(lob::Reject::Unparseable)] << ','
                    << c.rejects[size_t(lob::Reject::UnknownReference)] << ','
                    << c.rejects[size_t(lob::Reject::InvariantViolation)]
                    << ',' << c.rejects[size_t(lob::Reject::EconomicallyAbsurd)]
                    << ',' << signs << '\n';
            }
        }
    };
    shim::drive(a, tokens, bins, c, hook);

    if (csv.is_open()) {
        csv.flush();
        if (!csv) {
            std::fprintf(stderr, "cannot write %s\n", csv_path.c_str());
            return 1;
        }
    }

    const double n = double(checkpoints);
    const double denom = n * sxx - sx * sx;
    const double slope_per_tuple =
        (n >= 2 && denom != 0) ? (n * sxy - sx * sy) / denom : 0.0;
    const double two_frac = checkpoints ? double(two_sided_at) / n : 0.0;
    const bool v1 = signs >= 500;
    const bool v2 = two_frac >= 0.90;
    const bool v3 = !died_before_500;
    const bool viable = v1 && v2 && v3;

    std::printf("stream: %zu tokens, %" PRIu64 " tuples, applied %" PRIu64
                " (%.2f%%), signs %" PRIu64 "\n",
                tokens.size(), c.tuples, c.applied,
                c.tuples ? 100.0 * double(c.applied) / double(c.tuples) : 0.0,
                signs);
    std::printf("rejects: unparseable %" PRIu64 "  unknown_ref %" PRIu64
                "  invariant %" PRIu64 "  absurd %" PRIu64 "\n",
                c.rejects[size_t(lob::Reject::Unparseable)],
                c.rejects[size_t(lob::Reject::UnknownReference)],
                c.rejects[size_t(lob::Reject::InvariantViolation)],
                c.rejects[size_t(lob::Reject::EconomicallyAbsurd)]);
    std::printf("book: two-sided at %.1f%% of %" PRIu64
                " checkpoints; open_orders end %zu, drift %+.2f per 1k "
                "tuples%s\n",
                100.0 * two_frac, checkpoints, a.book().open_orders(),
                slope_per_tuple * 1000.0,
                dead_at_tuple ? (" (book DEAD at tuple " +
                                 std::to_string(dead_at_tuple) + ")")
                                    .c_str()
                              : "");
    std::printf("viability: %s  (V1 signs>=500: %s, V2 two-sided>=90%%: "
                "%s, V3 alive-through-500th-sign: %s)\n",
                viable ? "VIABLE" : "NOT VIABLE", v1 ? "pass" : "FAIL",
                v2 ? "pass" : "FAIL", v3 ? "pass" : "FAIL");
    return viable ? 0 : 2;
}
