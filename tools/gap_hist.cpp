// Measure the gap distribution between consecutive same-sign E/C fills per
// symbol, to choose the 2c collapse window. If one aggressive order walking
// the book produces fills ns-to-us apart while independent decisions arrive
// ms+ apart, the log10(gap) histogram is bimodal and the valley is the window.
#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "dataset.hpp"
#include "itch_replay.hpp"

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
static constexpr uint64_t T_OPEN  = 34200 * NS;
static constexpr uint64_t T_CLOSE = 57600 * NS;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <itch_file> [%s]\n", argv[0],
                     dataset::kOverrideFlag);
        return 2;
    }
    bool final_comparison = false;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], dataset::kOverrideFlag))
            final_comparison = true;
        else { std::fprintf(stderr, "bad arg: %s\n", argv[i]); return 2; }
    }
    dataset::enforce(argv[1], final_comparison);
    std::vector<uint8_t> wire = slurp(argv[1]);

    // last fill ts+sign per locate; histogram of log10(gap ns) same-sign pairs
    std::vector<uint64_t> last_ts(size_t(1) << 16, 0);
    std::vector<int8_t>   last_sg(size_t(1) << 16, 0);
    // buckets: log10(gap) 0..12 in 0.5 steps -> 24 buckets; gap==0 separate
    uint64_t zero_gap = 0, opp_sign = 0;
    uint64_t hist[26] = {0};
    // also collect all same-sign gaps for quantiles
    std::vector<uint64_t> gaps;

    lob::BookSet set;
    itch::FrameReader rd{wire.data(), wire.size()};
    while (auto m = rd.next()) {
        uint16_t loc = lob::locate_of(*m);
        char type = itch::type_of(*m);
        uint64_t ts = std::visit([](const auto& x) -> uint64_t {
            if constexpr (std::is_same_v<std::decay_t<decltype(x)>, itch::AddOrderMpid>)
                return x.add.h.timestamp;
            else if constexpr (std::is_same_v<std::decay_t<decltype(x)>, itch::OrderExecutedPrice>)
                return x.exec.h.timestamp;
            else
                return x.h.timestamp;
        }, *m);
        if ((type == 'E' || type == 'C') && ts >= T_OPEN && ts < T_CLOSE) {
            lob::Book& b = set.book(loc);
            if (const lob::Order* o = b.find(lob::ref_of(*m))) {
                int8_t s = o->side == lob::Side::Sell ? int8_t(1) : int8_t(-1);
                if (last_sg[loc] != 0) {
                    if (s == last_sg[loc]) {
                        uint64_t gap = ts - last_ts[loc];
                        if (gap == 0) ++zero_gap;
                        else {
                            double lg = std::log10(double(gap));
                            int b2 = int(lg * 2.0);
                            if (b2 < 0) b2 = 0;
                            if (b2 > 25) b2 = 25;
                            ++hist[b2];
                            gaps.push_back(gap);
                        }
                    } else ++opp_sign;
                }
                last_ts[loc] = ts;
                last_sg[loc] = s;
            }
        }
        if (set.apply(*m) != lob::Result::Ok) {
            std::fprintf(stderr, "REJECT\n"); return 1;
        }
    }
    if (rd.error) { std::fprintf(stderr, "STREAM ERROR\n"); return 1; }

    std::printf("same-sign consecutive fill pairs: %zu nonzero-gap, %" PRIu64
                " zero-gap, %" PRIu64 " sign-flips\n",
                gaps.size(), zero_gap, opp_sign);
    std::printf("\nlog10(gap ns) histogram (0.5-decade buckets):\n");
    static const char* unit[] = {"1ns","3ns","10ns","32ns","100ns","316ns",
        "1us","3us","10us","32us","100us","316us","1ms","3ms","10ms","32ms",
        "100ms","316ms","1s","3s","10s","32s","100s","316s","1000s","3162s"};
    for (int i = 0; i < 26; ++i)
        if (hist[i])
            std::printf("  >=%-6s %10" PRIu64 " %s\n", unit[i], hist[i],
                        std::string(std::min<uint64_t>(60, hist[i] / 50), '#').c_str());
    std::sort(gaps.begin(), gaps.end());
    auto q = [&](double p) { return gaps[size_t(p * double(gaps.size() - 1))]; };
    if (!gaps.empty())
        std::printf("\nquantiles: p10 %" PRIu64 "ns p25 %" PRIu64 "ns p50 %" PRIu64
                    "ns p75 %" PRIu64 "ns p90 %" PRIu64 "ns\n",
                    q(0.10), q(0.25), q(0.50), q(0.75), q(0.90));
    return 0;
}
