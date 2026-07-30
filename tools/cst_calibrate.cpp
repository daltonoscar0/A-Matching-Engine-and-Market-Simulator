// cst_calibrate - measure the Cont-Stoikov-Talreja rates from one real day
// (Step: Phase 3 null column). Emits an additive accumulator CSV per day;
// tools/cst_sim merges several days' accumulators into rates, so calibration
// can run day-by-day without holding every decompressed day on disk.
//
// Per tracked symbol, in-window (09:30-16:00) only:
//   - add counts per (side, distance-in-ticks from the OPPOSITE best at
//     arrival), d = 1..100; beyond-100 and no-reference adds counted but
//     excluded from the model (far-from-touch flow does not move the mid).
//   - cancel counts per (side, d at cancellation time), d = 1..101 (101 =
//     tail for orders that drifted beyond 100). X, D, and the cancel half
//     of U each count as one cancel decision; U's re-add counts as an add.
//   - exposure: order-seconds per (side, d), the time integral of resting
//     order counts, accumulated exactly between consecutive messages of the
//     symbol (the profile is constant in between). theta = cancels/exposure.
//   - market-order decisions per aggressor side, collapsed with the SAME
//     1ms same-sign window tools/stylized uses, with total shares per
//     decision -> empirical market size histogram.
//   - add-size histogram, spread histogram (ticks), opening mid.
//
// Usage: cst_calibrate <itch_file> <out_csv> [--symbols A,B,..] [override]
#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "../src/cst.hpp"
#include "../src/dataset.hpp"
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
static constexpr uint64_t T_OPEN  = 34200 * NS;
static constexpr uint64_t T_CLOSE = 57600 * NS;
static constexpr uint64_t COLLAPSE_NS = 1000000;   // stylized's default

static const char* kDefaultSymbols =
    "IWM,SPY,XLK,QQQ,IWO,UVXY,SOXL,TLT,IJH,XLE,XLV,IWN,VXX";

using cst::B;
using cst::BT;

struct Acc {
    std::string name;
    uint16_t locate = 0;
    uint64_t add[2][BT + 1]    = {};   // d 1..100 model, BT = beyond-100
    uint64_t cancel[2][BT + 1] = {};
    double   expo[2][BT + 1]   = {};
    uint64_t mkt[2] = {};
    std::unordered_map<uint32_t, uint64_t> addsz, mktsz, spread;
    uint32_t open_mid = 0;
    uint64_t noref_add = 0, noref_cancel = 0;
    uint64_t last_ts = T_OPEN;
    // market-decision collapse state
    uint64_t dec_ts = 0, dec_shares = 0;
    int8_t   dec_sign = 0;
    void flush_decision() {
        if (dec_sign) {
            int s = dec_sign > 0 ? 0 : 1;   // +1 = buy aggressor
            ++mkt[s];
            ++mktsz[uint32_t(std::min<uint64_t>(dec_shares, 0xFFFFFFFFull))];
        }
        dec_sign = 0; dec_shares = 0;
    }
};

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: %s <itch_file> <out_csv> [--symbols A,B,..] "
                     "[%s]\n", argv[0], dataset::kOverrideFlag);
        return 2;
    }
    std::string symcsv = kDefaultSymbols;
    bool final_comparison = false;
    for (int i = 3; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--symbols") && i + 1 < argc)
            symcsv = argv[++i];
        else if (!std::strcmp(argv[i], dataset::kOverrideFlag))
            final_comparison = true;
        else { std::fprintf(stderr, "bad arg: %s\n", argv[i]); return 2; }
    }
    dataset::enforce(argv[1], final_comparison);

    std::vector<std::string> want;
    for (size_t p = 0; p < symcsv.size();) {
        size_t c = symcsv.find(',', p);
        if (c == std::string::npos) c = symcsv.size();
        want.push_back(symcsv.substr(p, c - p));
        p = c + 1;
    }

    std::vector<uint8_t> wire = slurp(argv[1]);
    std::printf("file: %s (%.1f MB)\n", argv[1], wire.size() / 1e6);

    bool dir_err = false;
    auto dir = lob::scan_stock_directory(wire.data(), wire.size(), dir_err);
    if (dir_err) { std::fprintf(stderr, "directory scan failed\n"); return 1; }
    std::vector<int> slot_of(size_t(1) << 16, -1);
    std::vector<Acc> acc;
    for (auto& d : dir) {
        std::string s(d.stock.data(), 8);
        while (!s.empty() && s.back() == ' ') s.pop_back();
        for (auto& w : want)
            if (s == w) {
                Acc a;
                a.name = s;
                a.locate = d.h.stock_locate;
                slot_of[a.locate] = int(acc.size());
                acc.push_back(std::move(a));
            }
    }
    std::printf("tracking %zu of %zu requested symbols\n", acc.size(),
                want.size());

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
        Acc& a = acc[size_t(slot)];
        lob::Book& b = set.book(loc);
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

        if (in_window) {
            // Exposure over [last_ts, ts): book state is pre-apply and
            // constant since the symbol's previous message.
            double dt = double(ts - a.last_ts) / 1e9;
            if (dt > 0) {
                b.for_each_level([&](lob::Side s, const lob::Level& lv) {
                    uint32_t opp = s == lob::Side::Buy ? b.best_ask()
                                                       : b.best_bid();
                    if (opp)
                        a.expo[int(s)][cst::dist_of(s, lv.price, opp)] +=
                            dt * lv.order_count;
                    return true;
                });
            }
            a.last_ts = ts;

            auto count_add = [&](char side_c, uint32_t price) {
                lob::Side s = side_c == 'B' ? lob::Side::Buy : lob::Side::Sell;
                uint32_t opp = s == lob::Side::Buy ? b.best_ask()
                                                   : b.best_bid();
                if (!opp) { ++a.noref_add; return; }
                int d = cst::dist_of(s, price, opp);
                ++a.add[int(s)][d];   // d==BT bucket = beyond-100, reporting
            };
            auto count_cancel = [&](uint64_t ref) {
                const lob::Order* o = b.find(ref);
                if (!o) return;
                uint32_t opp = o->side == lob::Side::Buy ? b.best_ask()
                                                         : b.best_bid();
                if (!opp) { ++a.noref_cancel; return; }
                ++a.cancel[int(o->side)][cst::dist_of(o->side, o->price, opp)];
            };
            char type = itch::type_of(*m);
            switch (type) {
                case 'A': {
                    auto& x = std::get<itch::AddOrder>(*m);
                    count_add(x.side, x.price);
                    ++a.addsz[x.shares];
                    break;
                }
                case 'F': {
                    auto& x = std::get<itch::AddOrderMpid>(*m);
                    count_add(x.add.side, x.add.price);
                    ++a.addsz[x.add.shares];
                    break;
                }
                case 'X': count_cancel(lob::ref_of(*m)); break;
                case 'D': count_cancel(lob::ref_of(*m)); break;
                case 'U': {
                    auto& x = std::get<itch::OrderReplace>(*m);
                    const lob::Order* o = b.find(x.orig_order_ref);
                    count_cancel(x.orig_order_ref);
                    if (o) {
                        count_add(o->side == lob::Side::Buy ? 'B' : 'S',
                                  x.price);
                        ++a.addsz[x.shares];
                    }
                    break;
                }
                case 'E': case 'C': {
                    const lob::Order* o = b.find(lob::ref_of(*m));
                    if (o) {
                        int8_t sg = o->side == lob::Side::Sell ? int8_t(1)
                                                               : int8_t(-1);
                        uint32_t sh = std::visit(
                            [](const auto& x) -> uint32_t {
                                using T = std::decay_t<decltype(x)>;
                                if constexpr (std::is_same_v<T,
                                        itch::OrderExecuted>)
                                    return x.shares;
                                else if constexpr (std::is_same_v<T,
                                        itch::OrderExecutedPrice>)
                                    return x.exec.shares;
                                else
                                    return 0;
                            }, *m);
                        if (sg != a.dec_sign || ts - a.dec_ts > COLLAPSE_NS)
                            a.flush_decision();
                        a.dec_sign = sg;
                        a.dec_shares += sh;
                        a.dec_ts = ts;
                    }
                    break;
                }
                default: break;
            }
        }
        if (set.apply(*m) != lob::Result::Ok) {
            std::fprintf(stderr, "REJECT - reconstruction bug\n");
            return 1;
        }
        if (in_window) {
            uint32_t bid = b.best_bid(), ask = b.best_ask();
            if (bid && ask) {
                if (!a.open_mid) a.open_mid = (bid + ask) / 2;
                ++a.spread[(ask - bid + cst::TICK / 2) / cst::TICK];
            }
        }
    }
    if (rd.error) { std::fprintf(stderr, "STREAM ERROR\n"); return 1; }

    FILE* out = std::fopen(argv[2], "w");
    if (!out) { std::perror(argv[2]); return 1; }
    std::fprintf(out, "kind,symbol,side,key,a,b,c\n");
    for (auto& a : acc) {
        a.flush_decision();
        const char* sc[2] = {"B", "S"};
        for (int s = 0; s < 2; ++s) {
            for (int d = 1; d <= BT; ++d)
                if (a.add[s][d] || a.cancel[s][d] || a.expo[s][d] > 0)
                    std::fprintf(out,
                                 "flow,%s,%s,%d,%" PRIu64 ",%" PRIu64 ",%.6f\n",
                                 a.name.c_str(), sc[s], d, a.add[s][d],
                                 a.cancel[s][d], a.expo[s][d]);
            std::fprintf(out, "mkt,%s,%s,0,%" PRIu64 ",0,0\n", a.name.c_str(),
                         sc[s], a.mkt[s]);
        }
        for (auto& [sz, n] : a.addsz)
            std::fprintf(out, "addsz,%s,-,%u,%" PRIu64 ",0,0\n",
                         a.name.c_str(), sz, n);
        for (auto& [sz, n] : a.mktsz)
            std::fprintf(out, "mktsz,%s,-,%u,%" PRIu64 ",0,0\n",
                         a.name.c_str(), sz, n);
        for (auto& [tk, n] : a.spread)
            std::fprintf(out, "spread,%s,-,%u,%" PRIu64 ",0,0\n",
                         a.name.c_str(), tk, n);
        std::fprintf(out, "meta,%s,-,0,%u,%" PRIu64 ",%" PRIu64 "\n",
                     a.name.c_str(), a.open_mid, a.noref_add, a.noref_cancel);
        uint64_t tot_add = 0, far = a.add[0][BT] + a.add[1][BT];
        for (int s = 0; s < 2; ++s)
            for (int d = 1; d <= BT; ++d) tot_add += a.add[s][d];
        std::printf("%-6s adds=%" PRIu64 " (beyond-100: %.1f%%, noref %"
                    PRIu64 ") mkt B/S=%" PRIu64 "/%" PRIu64 " open_mid=%u\n",
                    a.name.c_str(), tot_add,
                    tot_add ? 100.0 * double(far) / double(tot_add) : 0.0,
                    a.noref_add, a.mkt[0], a.mkt[1], a.open_mid);
    }
    std::fclose(out);
    std::printf("wrote %s\n", argv[2]);
    return 0;
}
