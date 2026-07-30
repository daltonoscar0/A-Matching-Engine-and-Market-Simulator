// bbo_trace - does the reconstruction reproduce an actual market? (Step 1b)
//
// A sparsely-populated book also never crosses and also conserves shares;
// internal checks cannot catch a reconstruction that is missing liquidity.
// The external check: for the most-active symbols, the book should be
// two-sided essentially all session with spreads of cents. This tool picks
// the N symbols with the most book messages, replays the day, samples
// best bid / best ask / spread / depth-at-touch every 60s from 09:30 to
// 16:00 ET, writes one CSV per symbol to an output directory, and prints a
// per-symbol summary (two-sided fraction, spread stats, longest one-sided
// or empty stretch).
//
// Sampling: the book only changes on messages, so state at a boundary is
// the state after the last message with timestamp < boundary. The real feed
// is timestamp-monotonic, so one cursor over the merged stream suffices;
// all tracked books are sampled whenever the stream clock crosses a
// boundary. All messages are applied (books must be correct all day);
// only sampling is windowed.
//
// Usage: bbo_trace <itch_file> <out_dir> [--top N]
#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../src/itch_replay.hpp"

static std::vector<uint8_t> slurp(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) { std::perror("fopen"); std::exit(1); }
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    if (std::fread(buf.data(), 1, buf.size(), f) != buf.size()) {
        std::perror("fread"); std::exit(1);
    }
    std::fclose(f);
    return buf;
}

static constexpr uint64_t NS = 1000000000ull;
static constexpr uint64_t T_OPEN  = 34200 * NS;   // 09:30:00 ET
static constexpr uint64_t T_CLOSE = 57600 * NS;   // 16:00:00 ET
static constexpr uint64_t STEP    = 60 * NS;

static std::string hhmmss(uint64_t ts_ns) {
    uint64_t s = ts_ns / NS;
    char b[16];
    std::snprintf(b, sizeof b, "%02llu:%02llu:%02llu",
                  (unsigned long long)(s / 3600),
                  (unsigned long long)(s / 60 % 60),
                  (unsigned long long)(s % 60));
    return b;
}

// Best level's price and resting shares on each side (0/0 if side empty).
struct Touch {
    uint32_t bid = 0, ask = 0;
    uint64_t bid_depth = 0, ask_depth = 0;
};
static Touch touch_of(const lob::Book& b) {
    Touch t;
    bool have_bid = false, have_ask = false;
    b.for_each_level([&](lob::Side s, const lob::Level& lv) {
        if (s == lob::Side::Buy && !have_bid) {
            t.bid = lv.price; t.bid_depth = lv.total_shares; have_bid = true;
        } else if (s == lob::Side::Sell && !have_ask) {
            t.ask = lv.price; t.ask_depth = lv.total_shares; have_ask = true;
        }
        return !(have_bid && have_ask);
    });
    return t;
}

struct Tracked {
    uint16_t locate = 0;
    std::string name;
    FILE* csv = nullptr;
    // summary accumulators
    uint64_t samples = 0, two_sided = 0;
    double   spread_sum = 0;
    double   spread_max = 0;
    std::vector<double> spreads;         // for the median
    uint64_t cur_bad_run = 0, max_bad_run = 0;   // consecutive not-two-sided
};

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <itch_file> <out_dir> [--top N]\n",
                     argv[0]);
        return 2;
    }
    const char* out_dir = argv[2];
    size_t top_n = 5;
    for (int i = 3; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--top") && i + 1 < argc)
            top_n = strtoull(argv[++i], nullptr, 10);
        else { std::fprintf(stderr, "bad arg: %s\n", argv[i]); return 2; }
    }

    std::vector<uint8_t> wire = slurp(argv[1]);
    std::printf("file: %s (%.1f MB)\n", argv[1], wire.size() / 1e6);

    bool dir_err = false;
    auto dir = lob::scan_stock_directory(wire.data(), wire.size(), dir_err);
    if (dir_err) { std::fprintf(stderr, "directory scan failed\n"); return 1; }
    std::vector<std::string> name_of(size_t(1) << 16, "?");
    for (auto& d : dir) {
        std::string s(d.stock.data(), 8);
        while (!s.empty() && s.back() == ' ') s.pop_back();
        name_of[d.h.stock_locate] = s;
    }

    // ---- pass 1: book-message counts per locate ---------------------------
    std::vector<uint64_t> msgs(size_t(1) << 16, 0);
    {
        itch::FrameReader rd{wire.data(), wire.size()};
        while (auto m = rd.next()) ++msgs[lob::locate_of(*m)];
        if (rd.error) { std::fprintf(stderr, "STREAM ERROR\n"); return 1; }
    }
    std::vector<uint16_t> order(size_t(1) << 16);
    for (size_t i = 0; i < order.size(); ++i) order[i] = uint16_t(i);
    std::sort(order.begin(), order.end(), [&](uint16_t a, uint16_t b) {
        return msgs[a] != msgs[b] ? msgs[a] > msgs[b] : a < b;
    });
    std::printf("\ntop 20 symbols by book-message count:\n");
    for (size_t i = 0; i < 20 && msgs[order[i]]; ++i)
        std::printf("  %2zu. %-8s loc=%-5u %10" PRIu64 " msgs\n", i + 1,
                    name_of[order[i]].c_str(), order[i], msgs[order[i]]);

    std::vector<Tracked> tracked;
    for (size_t i = 0; i < top_n && msgs[order[i]]; ++i) {
        Tracked t;
        t.locate = order[i];
        t.name   = name_of[order[i]];
        std::string path = std::string(out_dir) + "/bbo_" + t.name + ".csv";
        t.csv = std::fopen(path.c_str(), "w");
        if (!t.csv) { std::perror(path.c_str()); return 1; }
        std::fprintf(t.csv, "time,bid,ask,spread,bid_depth,ask_depth\n");
        tracked.push_back(t);
    }

    // ---- pass 2: replay + sample ------------------------------------------
    lob::BookSet set;
    itch::FrameReader rd{wire.data(), wire.size()};
    uint64_t next_sample = T_OPEN;
    auto sample_all = [&](uint64_t at) {
        for (auto& t : tracked) {
            const lob::Book* b = set.find(t.locate);
            Touch tc = b ? touch_of(*b) : Touch{};
            bool two = tc.bid != 0 && tc.ask != 0;
            double spread = two ? (tc.ask - tc.bid) / 1e4 : 0;
            std::fprintf(t.csv, "%s,%.4f,%.4f,%.4f,%" PRIu64 ",%" PRIu64 "\n",
                         hhmmss(at).c_str(), tc.bid / 1e4, tc.ask / 1e4,
                         spread, tc.bid_depth, tc.ask_depth);
            ++t.samples;
            if (two) {
                ++t.two_sided;
                t.spread_sum += spread;
                t.spread_max = std::max(t.spread_max, spread);
                t.spreads.push_back(spread);
                t.cur_bad_run = 0;
            } else {
                ++t.cur_bad_run;
                t.max_bad_run = std::max(t.max_bad_run, t.cur_bad_run);
            }
        }
    };
    while (auto m = rd.next()) {
        uint64_t ts = std::visit([](const auto& x) -> uint64_t {
            if constexpr (std::is_same_v<std::decay_t<decltype(x)>,
                                         itch::AddOrderMpid>)
                return x.add.h.timestamp;
            else if constexpr (std::is_same_v<std::decay_t<decltype(x)>,
                                              itch::OrderExecutedPrice>)
                return x.exec.h.timestamp;
            else
                return x.h.timestamp;
        }, *m);
        while (ts >= next_sample && next_sample <= T_CLOSE) {
            sample_all(next_sample);
            next_sample += STEP;
        }
        if (set.apply(*m) != lob::Result::Ok) {
            std::fprintf(stderr, "REJECT during replay - reconstruction bug\n");
            return 1;
        }
    }
    if (rd.error) { std::fprintf(stderr, "STREAM ERROR\n"); return 1; }
    while (next_sample <= T_CLOSE) { sample_all(next_sample); next_sample += STEP; }

    // ---- summary -----------------------------------------------------------
    std::printf("\nper-symbol session summary (09:30-16:00, 60s samples):\n");
    std::printf("  %-8s %8s %10s %10s %10s %10s %14s\n", "symbol", "samples",
                "two-sided", "med sprd", "mean sprd", "max sprd",
                "worst 1-sided");
    for (auto& t : tracked) {
        double med = 0;
        if (!t.spreads.empty()) {
            std::sort(t.spreads.begin(), t.spreads.end());
            med = t.spreads[t.spreads.size() / 2];
        }
        std::printf("  %-8s %8" PRIu64 " %9.2f%% %9.4f %10.4f %10.4f %11" PRIu64
                    " min\n",
                    t.name.c_str(), t.samples,
                    100.0 * double(t.two_sided) / double(t.samples), med,
                    t.spread_sum / double(t.two_sided ? t.two_sided : 1),
                    t.spread_max, t.max_bad_run);
        std::fclose(t.csv);
    }
    return 0;
}
