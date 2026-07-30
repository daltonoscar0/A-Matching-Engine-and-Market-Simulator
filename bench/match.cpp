// bench_match - throughput and latency of the MATCHING path (Book::match via
// match_submit), benched separately from replay: matching does real work per
// message (walking levels, deciding fills, emitting) that reconstruction
// does not, so these rows are NOT comparable to the replay rows in BENCH.md.
//
// Workload: online generated mix against one book, steady-state-ish depth:
//   60% passive limit orders (rest via match, replenish the book)
//   25% aggressive orders (1/3 market, 2/3 crossing limits, sized to sweep
//       roughly one to a few levels)
//   15% deletes of random resting orders (bounds book growth)
// Same two-pass methodology as bench_replay: pass 1 untimed per op (one
// clock read total, best of `repeat`), pass 2 steady_clock around every
// match_submit call (clock overhead included in each sample). Identical
// seeds per pass, so both passes see the same op sequence.
//
// Usage: bench_match [n_ops=5000000] [repeat=3] [seed=42]
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

#include "../src/book.hpp"
#include "../src/match.hpp"

using Clock = std::chrono::steady_clock;

namespace {

constexpr uint32_t TICK = 100;
constexpr uint32_t MID0 = 1'000'000;

struct Mix {
    uint64_t matches = 0, crossing = 0, market = 0, fills = 0, deletes = 0;
};

// One benchmark run. If `lat_ns` is non-null, every match_submit call is
// timed individually and `crossing_idx` records which samples had fills.
double run(size_t n_ops, uint64_t seed, Mix& mix,
           std::vector<uint32_t>* lat_ns,
           std::vector<uint8_t>* crossing_flag) {
    std::mt19937_64 rng(seed);
    lob::Book book;
    book.reserve(1 << 20);
    std::vector<lob::Fill> fills;
    std::vector<itch::Message> emitted;
    std::vector<uint64_t> live;
    uint64_t match_seq = 0, next_ref = 0, ts = 34'200'000'000'000ULL;

    auto price_for = [&](lob::Side s, bool aggressive) -> uint32_t {
        int64_t off = aggressive ? -int64_t(rng() % 4)      // 0..3 through
                                 : int64_t(1 + rng() % 15); // 1..15 behind
        int64_t p;
        if (s == lob::Side::Buy) {
            uint32_t ba = book.best_ask();
            p = int64_t(ba ? ba : MID0 + TICK) - off * TICK;
        } else {
            uint32_t bb = book.best_bid();
            p = int64_t(bb ? bb : MID0 - TICK) + off * TICK;
        }
        return p < TICK ? TICK : uint32_t(p);
    };

    auto t0 = Clock::now();
    for (size_t i = 0; i < n_ops; ++i) {
        uint32_t roll = uint32_t(rng() % 100);
        if (roll < 15 && !live.empty()) {              // delete, untimed:
            // `live` may hold refs already consumed by fills (cleaned lazily
            // here rather than with an O(n) scan per fill, which would
            // dominate the bench and measure the harness, not the book).
            for (int tries = 0; tries < 8 && !live.empty(); ++tries) {
                size_t k = rng() % live.size();
                uint64_t ref = live[k];
                live[k] = live.back();
                live.pop_back();
                if (book.remove(ref) == lob::Result::Ok) {
                    ++mix.deletes;
                    break;
                }
            }
            continue;
        }
        bool aggressive = roll < 40;                   // 25% of all ops
        bool market = aggressive && rng() % 3 == 0;
        lob::Side side = (rng() & 1) ? lob::Side::Buy : lob::Side::Sell;
        uint32_t shares = 100 * (1 + rng() % (aggressive ? 30 : 10));
        lob::MatchRequest r;
        r.locate = 1;
        r.timestamp = ts += 1 + rng() % 5000;
        r.ref = ++next_ref;
        r.side = side;
        r.shares = shares;
        r.limit = market ? 0 : price_for(side, aggressive);
        r.market = market;
        emitted.clear();

        lob::MatchOutcome mo;
        if (lat_ns) {
            auto a = Clock::now();
            mo = lob::match_submit(book, r, match_seq, fills, emitted);
            auto b = Clock::now();
            lat_ns->push_back(uint32_t(
                std::chrono::duration_cast<std::chrono::nanoseconds>(b - a)
                    .count()));
            crossing_flag->push_back(!fills.empty());
        } else {
            mo = lob::match_submit(book, r, match_seq, fills, emitted);
        }
        if (mo.result != lob::Result::Ok) {
            std::fprintf(stderr, "match rejected at op %zu\n", i);
            std::exit(1);
        }
        ++mix.matches;
        mix.fills += fills.size();
        mix.crossing += !fills.empty();
        mix.market += market;
        if (mo.rested) live.push_back(r.ref);
    }
    auto t1 = Clock::now();
    if (!book.audit().empty()) {
        std::fprintf(stderr, "AUDIT FAIL\n");
        std::exit(1);
    }
    return std::chrono::duration<double>(t1 - t0).count();
}

}  // namespace

int main(int argc, char** argv) {
    size_t n_ops = argc > 1 ? strtoull(argv[1], nullptr, 10) : 5'000'000;
    int repeat   = argc > 2 ? atoi(argv[2]) : 3;
    uint64_t seed = argc > 3 ? strtoull(argv[3], nullptr, 10) : 42;

    // ---- pass 1: throughput ------------------------------------------------
    double best_rate = 0;
    Mix mix;
    for (int r = 0; r < repeat; ++r) {
        Mix m;
        double secs = run(n_ops, seed, m, nullptr, nullptr);
        double rate = double(m.matches) / secs;
        best_rate = std::max(best_rate, rate);
        mix = m;
        std::printf("pass1[%d]: %llu matches in %.3fs -> %.2fM matches/sec "
                    "(crossing=%llu market=%llu fills=%llu deletes=%llu)\n",
                    r, (unsigned long long)m.matches, secs, rate / 1e6,
                    (unsigned long long)m.crossing,
                    (unsigned long long)m.market,
                    (unsigned long long)m.fills,
                    (unsigned long long)m.deletes);
    }

    // ---- pass 2: per-match latency ----------------------------------------
    std::vector<uint32_t> lat;
    std::vector<uint8_t> crossing;
    lat.reserve(n_ops);
    crossing.reserve(n_ops);
    Mix m2;
    run(n_ops, seed, m2, &lat, &crossing);
    std::vector<uint32_t> lat_cross;
    for (size_t i = 0; i < lat.size(); ++i)
        if (crossing[i]) lat_cross.push_back(lat[i]);
    auto report = [](const char* name, std::vector<uint32_t>& v) {
        std::sort(v.begin(), v.end());
        auto pct = [&](double p) {
            return v[size_t(p * double(v.size() - 1))];
        };
        std::printf("%s (n=%zu, incl ~clock overhead): p50=%uns p90=%uns "
                    "p99=%uns p99.9=%uns max=%uns\n",
                    name, v.size(), pct(0.50), pct(0.90), pct(0.99),
                    pct(0.999), v.back());
    };
    report("latency all match calls", lat);
    report("latency crossing only  ", lat_cross);
    std::printf("throughput (best of %d): %.2fM matches/sec, "
                "%.2f fills/crossing-match avg\n",
                repeat, best_rate / 1e6,
                mix.crossing ? double(mix.fills) / double(mix.crossing) : 0.0);
    return 0;
}
