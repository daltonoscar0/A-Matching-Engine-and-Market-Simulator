// stylized - empirical stylized-facts baseline from a real ITCH day (Step 2,
// Phase 3 "real" column). Computed BEFORE any generative model exists, so the
// baseline is not under pressure to agree with anything.
//
// Per symbol, for the top N symbols by book-message count, continuous market
// hours only (09:30-16:00 ET; this BX day carries no cross ('Q') frames, so
// the time filter IS the auction exclusion):
//
//   - mid = (best_bid + best_ask)/2, observed in EVENT time (every applied
//     book message on that symbol while two-sided) and on calendar grids
//     (1s / 10s / 60s, last-observation-carried-forward). Log returns.
//   - excess kurtosis per scale (fat tails + aggregational Gaussianity);
//     zero returns are KEPT in all primary series (they are what sampling a
//     thin book produces) with the zero fraction reported alongside, plus a
//     mid-change-only ("tick") variant so the distortion is visible.
//   - Hill tail-index on the top 5% of nonzero |r| per scale.
//   - ACF of r (lags 1-50) and of |r| (lags 1-100), at event scale and 1s
//     scale. Bid-ask bounce / quote flicker makes ACF(r) lag-1 negative at
//     event scale; that is microstructure, reported not smoothed away.
//   - ACF of |r| ALSO on the tick series (mid-change-only): with ~70% zero
//     returns at 1s, slow ACF(|r|) decay on the calendar grid can be bursty
//     ACTIVITY clustering (quiet stretches cluster in time), which is
//     indistinguishable from volatility clustering in that statistic. The
//     tick series has no zeros by construction, so surviving slow decay
//     there is volatility clustering proper.
//   - aggressive order-flow signs from E/C fills: the resting order's side
//     names the aggressor (resting ask hit -> buy = +1). Consecutive fills
//     with the same sign separated by <= collapse_ns (default 1ms, flag
//     --collapse-ns) collapse into one market order: one aggressive order
//     sweeping several price levels produces fills with DISTINCT nanosecond
//     timestamps (measured on this day: machine-scale gap mode at ~32us
//     running to ~1ms, decision-scale mass from ~100ms up, valley at
//     10-32ms), so an exact-timestamp rule splits single sweeps. ACF over
//     lags 1-1000 plus a log-log slope fit (Lillo-Farmer long memory).
//
// Outputs (out_dir): per symbol stylized_<SYM>_event.csv (ts_ns,mid),
// stylized_<SYM>_1s.csv (t_s,mid; the 10s/60s grids are subsets of this),
// stylized_<SYM>_acf.csv, stylized_<SYM>_flowacf.csv, and a cross-symbol
// stylized_summary.csv.
//
// Usage: stylized <itch_file> <out_dir> [--top N] [--collapse-ns NS]
#include <algorithm>
#include <cinttypes>
#include <cmath>
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

// ------------------------------------------------------------------ stats
static std::vector<double> log_returns(const std::vector<double>& x) {
    std::vector<double> r;
    if (x.size() < 2) return r;
    r.reserve(x.size() - 1);
    for (size_t i = 1; i < x.size(); ++i) r.push_back(std::log(x[i] / x[i-1]));
    return r;
}

static double excess_kurtosis(const std::vector<double>& r) {
    size_t n = r.size();
    if (n < 4) return NAN;
    double mean = 0;
    for (double v : r) mean += v;
    mean /= double(n);
    double m2 = 0, m4 = 0;
    for (double v : r) {
        double d = v - mean;
        m2 += d * d; m4 += d * d * d * d;
    }
    m2 /= double(n); m4 /= double(n);
    if (m2 == 0) return NAN;
    return m4 / (m2 * m2) - 3.0;
}

static double zero_fraction(const std::vector<double>& r) {
    if (r.empty()) return NAN;
    size_t z = 0;
    for (double v : r) z += v == 0.0;
    return double(z) / double(r.size());
}

// ACF with mean subtracted, biased normalization (divide by N*var).
static std::vector<double> acf(const std::vector<double>& x, size_t max_lag) {
    std::vector<double> out(max_lag + 1, NAN);
    size_t n = x.size();
    if (n < 3) return out;
    double mean = 0;
    for (double v : x) mean += v;
    mean /= double(n);
    double var = 0;
    for (double v : x) var += (v - mean) * (v - mean);
    if (var == 0) return out;
    out[0] = 1.0;
    for (size_t k = 1; k <= max_lag && k < n; ++k) {
        double s = 0;
        for (size_t i = k; i < n; ++i) s += (x[i] - mean) * (x[i-k] - mean);
        out[k] = s / var;
    }
    return out;
}

// Hill tail-index alpha on the largest `frac` of the nonzero |r|.
// alpha = k / sum_{i=1..k} ln(x_(i) / x_(k)), order stats descending.
static double hill_alpha(const std::vector<double>& r, double frac) {
    std::vector<double> a;
    a.reserve(r.size());
    for (double v : r) if (v != 0.0) a.push_back(std::fabs(v));
    if (a.size() < 20) return NAN;
    size_t k = std::max<size_t>(10, size_t(frac * double(a.size())));
    if (k >= a.size()) return NAN;
    std::nth_element(a.begin(), a.begin() + k, a.end(), std::greater<>());
    double xk = a[k];
    if (xk <= 0) return NAN;
    double s = 0;
    for (size_t i = 0; i < k; ++i) s += std::log(a[i] / xk);
    return s > 0 ? double(k) / s : NAN;
}

// Least-squares slope of ln(y) vs ln(lag) over lags where y > 0.
static double loglog_slope(const std::vector<double>& acfv, size_t lo,
                           size_t hi, size_t* used = nullptr) {
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    size_t n = 0;
    for (size_t k = lo; k <= hi && k < acfv.size(); ++k) {
        if (!(acfv[k] > 0)) continue;
        double x = std::log(double(k)), y = std::log(acfv[k]);
        sx += x; sy += y; sxx += x * x; sxy += x * y;
        ++n;
    }
    if (used) *used = n;
    if (n < 5) return NAN;
    double den = double(n) * sxx - sx * sx;
    return den != 0 ? (double(n) * sxy - sx * sy) / den : NAN;
}

// Grid-sample (last observation carried forward). Returns mid per grid point
// starting from the first grid point with an observation.
static std::vector<double> grid_sample(const std::vector<uint64_t>& ts,
                                       const std::vector<double>& mid,
                                       uint64_t step_ns) {
    std::vector<double> out;
    if (ts.empty()) return out;
    size_t i = 0;
    double last = NAN;
    for (uint64_t t = T_OPEN; t <= T_CLOSE; t += step_ns) {
        while (i < ts.size() && ts[i] <= t) last = mid[i++];
        if (!std::isnan(last)) out.push_back(last);
    }
    return out;
}

struct Tracked {
    uint16_t locate = 0;
    std::string name;
    std::vector<uint64_t> ts;      // event-time observations, in-window
    std::vector<double>   mid;     // dollars
    std::vector<int8_t>   sign;    // collapsed aggressive-flow signs
    uint64_t last_sign_ts = 0;
    int8_t   last_sign    = 0;
    uint64_t one_sided_events = 0; // applied msgs skipped: book one-sided
};

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <itch_file> <out_dir> [--top N]\n",
                     argv[0]);
        return 2;
    }
    const char* out_dir = argv[2];
    size_t top_n = 20;
    uint64_t collapse_ns = 1000000;   // 1ms; see header comment
    for (int i = 3; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--top") && i + 1 < argc)
            top_n = strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--collapse-ns") && i + 1 < argc)
            collapse_ns = strtoull(argv[++i], nullptr, 10);
        else { std::fprintf(stderr, "bad arg: %s\n", argv[i]); return 2; }
    }

    std::vector<uint8_t> wire = slurp(argv[1]);
    std::printf("file: %s (%.1f MB), sign collapse window %" PRIu64 " ns\n",
                argv[1], wire.size() / 1e6, collapse_ns);

    bool dir_err = false;
    auto dir = lob::scan_stock_directory(wire.data(), wire.size(), dir_err);
    if (dir_err) { std::fprintf(stderr, "directory scan failed\n"); return 1; }
    std::vector<std::string> name_of(size_t(1) << 16, "?");
    for (auto& d : dir) {
        std::string s(d.stock.data(), 8);
        while (!s.empty() && s.back() == ' ') s.pop_back();
        name_of[d.h.stock_locate] = s;
    }

    // ---- pass 1: pick top N by book-message count -------------------------
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
    std::vector<int> slot_of(size_t(1) << 16, -1);
    std::vector<Tracked> tr;
    for (size_t i = 0; i < top_n && msgs[order[i]]; ++i) {
        Tracked t;
        t.locate = order[i];
        t.name   = name_of[order[i]];
        slot_of[t.locate] = int(tr.size());
        tr.push_back(std::move(t));
    }

    // ---- pass 2: replay, observe mids + flow signs ------------------------
    lob::BookSet set;
    itch::FrameReader rd{wire.data(), wire.size()};
    while (auto m = rd.next()) {
        uint16_t loc = lob::locate_of(*m);
        int slot = slot_of[loc];
        if (slot < 0) {
            if (set.apply(*m) != lob::Result::Ok) {
                std::fprintf(stderr, "REJECT - reconstruction bug\n");
                return 1;
            }
            continue;
        }
        Tracked& t = tr[size_t(slot)];
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
        bool in_window = ts >= T_OPEN && ts < T_CLOSE;
        lob::Book& b = set.book(loc);

        // Aggressor sign before apply: the resting order's side is known now.
        char type = itch::type_of(*m);
        if (in_window && (type == 'E' || type == 'C')) {
            if (const lob::Order* o = b.find(lob::ref_of(*m))) {
                int8_t s = o->side == lob::Side::Sell ? int8_t(1) : int8_t(-1);
                if (t.sign.empty() || s != t.last_sign ||
                    ts - t.last_sign_ts > collapse_ns)
                    t.sign.push_back(s);
                t.last_sign_ts = ts;
                t.last_sign    = s;
            }
        }
        if (lob::apply(b, *m) != lob::Result::Ok) {
            std::fprintf(stderr, "REJECT - reconstruction bug\n");
            return 1;
        }
        if (in_window) {
            uint32_t bid = b.best_bid(), ask = b.best_ask();
            if (bid && ask) {
                t.ts.push_back(ts);
                t.mid.push_back((double(bid) + double(ask)) / 2.0 / 1e4);
            } else {
                ++t.one_sided_events;
            }
        }
    }
    if (rd.error) { std::fprintf(stderr, "STREAM ERROR\n"); return 1; }

    // ---- per-symbol computation + CSVs ------------------------------------
    std::string spath = std::string(out_dir) + "/stylized_summary.csv";
    FILE* sum = std::fopen(spath.c_str(), "w");
    if (!sum) { std::perror(spath.c_str()); return 1; }
    std::fprintf(sum,
        "symbol,n_event,zero_frac_event,kurt_event,kurt_tick,"
        "n_1s,zero_frac_1s,kurt_1s,kurt_1s_nonzero,kurt_10s,kurt_60s,"
        "hill_event,hill_1s,"
        "acf1_r_event,acf1_r_1s,acf1_absr_1s,acf10_absr_1s,acf50_absr_1s,"
        "acf100_absr_1s,n_tick,acf1_absr_tick,acf10_absr_tick,"
        "acf50_absr_tick,acf100_absr_tick,n_signs,acf1_sign,acf10_sign,"
        "acf100_sign,acf1000_sign,sign_loglog_slope\n");

    std::printf("\n%-8s %9s %7s %8s %8s %8s %8s %7s %7s %8s %8s %8s %8s\n",
                "symbol", "n_event", "zeroE", "kurtE", "kurt1s", "kurt10s",
                "kurt60s", "hill1s", "acf1E", "acf1|r|", "a1|r|tk", "signN",
                "signSlope");
    for (auto& t : tr) {
        // event series + returns
        auto r_ev = log_returns(t.mid);
        // tick series: mid-change only
        std::vector<double> tickmid;
        for (size_t i = 0; i < t.mid.size(); ++i)
            if (tickmid.empty() || t.mid[i] != tickmid.back())
                tickmid.push_back(t.mid[i]);
        auto r_tick = log_returns(tickmid);
        // calendar grids
        auto m1  = grid_sample(t.ts, t.mid, 1 * NS);
        auto m10 = grid_sample(t.ts, t.mid, 10 * NS);
        auto m60 = grid_sample(t.ts, t.mid, 60 * NS);
        auto r1 = log_returns(m1), r10 = log_returns(m10),
             r60 = log_returns(m60);
        std::vector<double> r1_nz;
        for (double v : r1) if (v != 0.0) r1_nz.push_back(v);

        auto acf_r_ev   = acf(r_ev, 50);
        std::vector<double> absr_ev(r_ev.size());
        for (size_t i = 0; i < r_ev.size(); ++i) absr_ev[i] = std::fabs(r_ev[i]);
        auto acf_absr_ev = acf(absr_ev, 100);
        auto acf_r_1s   = acf(r1, 50);
        std::vector<double> absr1(r1.size());
        for (size_t i = 0; i < r1.size(); ++i) absr1[i] = std::fabs(r1[i]);
        auto acf_absr_1s = acf(absr1, 100);
        // 2a: |r| ACF on the tick series (every observation a genuine mid
        // change, zeros absent by construction) - separates volatility
        // clustering from bursty-activity clustering.
        std::vector<double> absr_tick(r_tick.size());
        for (size_t i = 0; i < r_tick.size(); ++i)
            absr_tick[i] = std::fabs(r_tick[i]);
        auto acf_absr_tick = acf(absr_tick, 100);

        std::vector<double> signd(t.sign.begin(), t.sign.end());
        size_t sign_maxlag = std::min<size_t>(1000, signd.size() / 4);
        auto acf_sign = acf(signd, sign_maxlag);
        size_t slope_pts = 0;
        double slope = loglog_slope(acf_sign, 1,
                                    std::min<size_t>(100, sign_maxlag),
                                    &slope_pts);

        // CSVs
        auto open_csv = [&](const char* suffix) {
            std::string p = std::string(out_dir) + "/stylized_" + t.name +
                            "_" + suffix + ".csv";
            FILE* f = std::fopen(p.c_str(), "w");
            if (!f) { std::perror(p.c_str()); std::exit(1); }
            return f;
        };
        FILE* f = open_csv("event");
        std::fprintf(f, "ts_ns,mid\n");
        for (size_t i = 0; i < t.ts.size(); ++i)
            std::fprintf(f, "%" PRIu64 ",%.6f\n", t.ts[i], t.mid[i]);
        std::fclose(f);
        f = open_csv("1s");
        std::fprintf(f, "t_s,mid\n");
        for (size_t i = 0; i < m1.size(); ++i)
            std::fprintf(f, "%" PRIu64 ",%.6f\n",
                         (T_OPEN / NS) + uint64_t(i) +
                             uint64_t(m1.size() ? (23401 - m1.size()) : 0),
                         m1[i]);
        std::fclose(f);
        f = open_csv("acf");
        std::fprintf(f, "lag,acf_r_event,acf_absr_event,acf_r_1s,acf_absr_1s,"
                        "acf_absr_tick\n");
        for (size_t k = 1; k <= 100; ++k)
            std::fprintf(f, "%zu,%.6f,%.6f,%.6f,%.6f,%.6f\n", k,
                         k < acf_r_ev.size() ? acf_r_ev[k] : NAN,
                         k < acf_absr_ev.size() ? acf_absr_ev[k] : NAN,
                         k < acf_r_1s.size() ? acf_r_1s[k] : NAN,
                         k < acf_absr_1s.size() ? acf_absr_1s[k] : NAN,
                         k < acf_absr_tick.size() ? acf_absr_tick[k] : NAN);
        std::fclose(f);
        f = open_csv("flowacf");
        std::fprintf(f, "lag,acf_sign\n");
        for (size_t k = 1; k < acf_sign.size(); ++k)
            std::fprintf(f, "%zu,%.6f\n", k, acf_sign[k]);
        std::fclose(f);

        auto at = [](const std::vector<double>& v, size_t k) {
            return k < v.size() ? v[k] : NAN;
        };
        std::fprintf(sum,
            "%s,%zu,%.4f,%.2f,%.2f,%zu,%.4f,%.2f,%.2f,%.2f,%.2f,"
            "%.2f,%.2f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%zu,%.4f,%.4f,%.4f,"
            "%.4f,%zu,%.4f,%.4f,%.4f,%.4f,%.3f\n",
            t.name.c_str(), r_ev.size(), zero_fraction(r_ev),
            excess_kurtosis(r_ev), excess_kurtosis(r_tick), r1.size(),
            zero_fraction(r1), excess_kurtosis(r1), excess_kurtosis(r1_nz),
            excess_kurtosis(r10), excess_kurtosis(r60),
            hill_alpha(r_ev, 0.05), hill_alpha(r1, 0.05),
            at(acf_r_ev, 1), at(acf_r_1s, 1), at(acf_absr_1s, 1),
            at(acf_absr_1s, 10), at(acf_absr_1s, 50), at(acf_absr_1s, 100),
            r_tick.size(), at(acf_absr_tick, 1), at(acf_absr_tick, 10),
            at(acf_absr_tick, 50), at(acf_absr_tick, 100),
            signd.size(), at(acf_sign, 1), at(acf_sign, 10),
            at(acf_sign, 100), at(acf_sign, 1000), slope);

        std::printf("%-8s %9zu %6.1f%% %8.1f %8.1f %8.1f %8.1f %7.2f %7.3f "
                    "%8.3f %8.3f %8zu %8.3f\n",
                    t.name.c_str(), r_ev.size(),
                    100 * zero_fraction(r_ev), excess_kurtosis(r_ev),
                    excess_kurtosis(r1), excess_kurtosis(r10),
                    excess_kurtosis(r60), hill_alpha(r1, 0.05),
                    at(acf_r_ev, 1), at(acf_absr_1s, 1),
                    at(acf_absr_tick, 1), signd.size(), slope);
    }
    std::fclose(sum);
    std::printf("\nwrote per-symbol CSVs + stylized_summary.csv to %s\n",
                out_dir);
    return 0;
}
