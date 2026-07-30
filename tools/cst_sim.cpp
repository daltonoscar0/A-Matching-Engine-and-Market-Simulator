// cst_sim - generate one simulated "day" from the Cont-Stoikov-Talreja null
// model and write it as a BinaryFILE ITCH stream that tools/stylized replays
// unmodified (same pipeline, same collapse window, same everything as the
// real column).
//
// Inputs are one or more accumulator CSVs from tools/cst_calibrate (one per
// TRAIN day); counts and exposures are summed before rates are formed, so
// N days calibrate one parameter set. Every symbol simulates independently
// (the null has no cross-symbol structure); streams are merged by timestamp
// and prefixed with 'R' directory frames so stylized names the symbols.
//
// Timeline: warm-up 08:00-09:30 from a seeded two-order book (outside
// stylized's 09:30-16:00 window, so the transient is never sampled), then
// the trading day to 16:05.
//
// The output is self-verified before writing: the whole stream replays
// through the validated reconstruction path (BookSet/apply) and any reject
// or failed audit is fatal - the same bar real data is held to.
//
// Usage: cst_sim <out_itch_file> <seed> <calib_csv> [calib_csv ...]
#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../src/cst.hpp"
#include "../src/itch_replay.hpp"

static constexpr uint64_t NS = 1000000000ull;
static constexpr uint64_t T_WARM  = 28800 * NS;   // 08:00
static constexpr uint64_t T_END   = 57900 * NS;   // 16:05
static constexpr double   WINDOW_SEC = 23400.0;   // rates are per in-window s

struct Merged {
    uint64_t add[2][cst::BT + 1]    = {};
    uint64_t cancel[2][cst::BT + 1] = {};
    double   expo[2][cst::BT + 1]   = {};
    uint64_t mkt[2] = {};
    std::map<uint32_t, uint64_t> addsz, mktsz, spread;
    std::vector<uint32_t> open_mids;
};

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr,
                     "usage: %s <out_itch_file> <seed> <calib_csv>...\n",
                     argv[0]);
        return 2;
    }
    const char* out_path = argv[1];
    uint64_t base_seed = strtoull(argv[2], nullptr, 10);
    int ndays = argc - 3;

    std::map<std::string, Merged> merged;
    for (int f = 3; f < argc; ++f) {
        FILE* in = std::fopen(argv[f], "r");
        if (!in) { std::perror(argv[f]); return 1; }
        char line[512];
        if (!std::fgets(line, sizeof line, in)) { return 1; }   // header
        while (std::fgets(line, sizeof line, in)) {
            char kind[16], sym[16], side[4];
            unsigned long long key, a, b;
            double c;
            if (std::sscanf(line, "%15[^,],%15[^,],%3[^,],%llu,%llu,%llu,%lf",
                            kind, sym, side, &key, &a, &b, &c) != 7)
                continue;
            Merged& m = merged[sym];
            int s = side[0] == 'S' ? 1 : 0;
            if (!std::strcmp(kind, "flow") && key >= 1 && key <= cst::BT) {
                m.add[s][key]    += a;
                m.cancel[s][key] += b;
                m.expo[s][key]   += c;
            } else if (!std::strcmp(kind, "mkt")) {
                m.mkt[s] += a;
            } else if (!std::strcmp(kind, "addsz")) {
                m.addsz[uint32_t(key)] += a;
            } else if (!std::strcmp(kind, "mktsz")) {
                m.mktsz[uint32_t(key)] += a;
            } else if (!std::strcmp(kind, "spread")) {
                m.spread[uint32_t(key)] += a;
            } else if (!std::strcmp(kind, "meta")) {
                if (a) m.open_mids.push_back(uint32_t(a));
            }
        }
        std::fclose(in);
    }
    if (merged.empty()) { std::fprintf(stderr, "no symbols parsed\n"); return 1; }

    const double total_sec = WINDOW_SEC * ndays;
    std::vector<cst::TsMsg> all;
    std::vector<uint8_t> out_buf;
    uint16_t locate = 0;
    std::printf("%-6s %10s %10s %8s %8s %9s %7s\n", "symbol", "lam_add/s",
                "theta_max", "mu_B/s", "mu_S/s", "sim_msgs", "fills");
    for (auto& [name, m] : merged) {
        ++locate;
        cst::SymbolParams sp;
        sp.name = name;
        // p0 = median opening mid, tick-aligned
        std::sort(m.open_mids.begin(), m.open_mids.end());
        if (m.open_mids.empty()) { std::fprintf(stderr, "no mid\n"); return 1; }
        sp.p0 = m.open_mids[m.open_mids.size() / 2] / cst::TICK * cst::TICK;
        // median spread in ticks -> half spread
        uint64_t stot = 0, sacc = 0;
        for (auto& [tk, n] : m.spread) stot += n;
        uint32_t smed = 1;
        for (auto& [tk, n] : m.spread) {
            sacc += n;
            if (sacc * 2 >= stot) { smed = tk; break; }
        }
        sp.half_spread_ticks = std::max<uint32_t>(1, (smed + 1) / 2);
        double lam_sum = 0, theta_max = 0;
        for (int s = 0; s < 2; ++s) {
            for (int d = 1; d <= cst::B; ++d) {
                sp.lambda[s][d] = double(m.add[s][d]) / total_sec;
                lam_sum += sp.lambda[s][d];
            }
            for (int d = 1; d <= cst::BT; ++d) {
                // theta needs real exposure under it; buckets with less
                // than 1 order-second cannot support a rate estimate.
                sp.theta[s][d] = m.expo[s][d] >= 1.0
                                     ? double(m.cancel[s][d]) / m.expo[s][d]
                                     : 0.0;
                theta_max = std::max(theta_max, sp.theta[s][d]);
            }
            sp.mu[s] = double(m.mkt[s]) / total_sec;
        }
        for (auto& [sz, n] : m.addsz) sp.add_sz.add(sz, n);
        for (auto& [sz, n] : m.mktsz) sp.mkt_sz.add(sz, n);

        size_t before = all.size();
        cst::SimStats st = cst::simulate(sp, locate, base_seed * 1000 + locate,
                                         T_WARM, T_END, all);
        std::printf("%-6s %10.2f %10.3f %8.4f %8.4f %9zu %7" PRIu64 "\n",
                    name.c_str(), lam_sum, theta_max, sp.mu[0], sp.mu[1],
                    all.size() - before, st.fills);

        // 'R' directory frame so stylized can name the symbol.
        uint8_t body[itch::LEN_R] = {};
        body[0] = 'R';
        itch::put_u16(body + 1, locate);
        itch::put_u16(body + 3, 0);
        itch::put_u48(body + 5, 0);
        std::memset(body + 11, ' ', 8);
        std::memcpy(body + 11, name.data(), std::min<size_t>(8, name.size()));
        body[19] = 'Q'; body[20] = 'N';
        itch::put_u32(body + 21, 100);
        body[25] = 'N'; body[26] = ' ';
        size_t at = out_buf.size();
        out_buf.resize(at + 2 + itch::LEN_R);
        itch::put_u16(out_buf.data() + at, uint16_t(itch::LEN_R));
        std::memcpy(out_buf.data() + at + 2, body, itch::LEN_R);
    }

    std::stable_sort(all.begin(), all.end(),
                     [](const cst::TsMsg& x, const cst::TsMsg& y) {
                         return x.first < y.first;
                     });
    for (auto& [ts, msg] : all) itch::encode_framed(msg, out_buf);

    // Self-verification: the emitted stream must replay through the
    // validated reconstruction path with zero rejects and clean audits.
    {
        lob::BookSet set;
        itch::FrameReader rd{out_buf.data(), out_buf.size()};
        uint64_t applied = 0;
        while (auto msg = rd.next()) {
            if (set.apply(*msg) != lob::Result::Ok) {
                std::fprintf(stderr, "SELF-CHECK REJECT at msg %" PRIu64
                             " - cst_sim bug\n", applied);
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

    FILE* out = std::fopen(out_path, "wb");
    if (!out) { std::perror(out_path); return 1; }
    if (std::fwrite(out_buf.data(), 1, out_buf.size(), out) != out_buf.size()) {
        std::perror("fwrite"); return 1;
    }
    std::fclose(out);
    std::printf("wrote %s (%.1f MB, %zu msgs, %d calib days, seed %" PRIu64
                ")\n", out_path, out_buf.size() / 1e6, all.size(), ndays,
                base_seed);
    return 0;
}
