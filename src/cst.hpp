// cst.hpp - Cont-Stoikov-Talreja null model (Phase 3 null column).
//
// A memoryless order-flow generator: independent Poisson streams of
//   - limit orders  at rate lambda(side, d), d = distance in ticks from the
//     OPPOSITE best quote (the CST convention), d = 1..B
//   - cancellations at rate theta(side, d) PER RESTING ORDER at distance d
//     (state-dependent thinning; d = 1..B, plus a tail bucket for orders
//     that drift beyond B when the opposite best moves)
//   - market orders at rate mu(side), size drawn empirically; executed via
//     Book::match, so fills/sweeps are decided by the real engine and the
//     emitted stream (A/E/D) is reconstruction-closed by construction.
//
// The rates are EMPIRICAL per bucket (calibrated by tools/cst_calibrate from
// TRAIN days), not a fitted power law: the null must be a fair description
// of the marginal intensities, differing from reality ONLY in having no
// memory - every event time is exponential given the current book, every
// size an i.i.d. draw, every side a coin weighted by the marginal rate.
//
// Prices are ITCH fixed point (1e-4 dollars), tick = 100 (whole cents; the
// panel symbols trade in pennies). The book starts with one seed order per
// side around p0 and warms up under the calibrated flow; callers put the
// warm-up before 09:30 so tools/stylized's window never samples it.
#pragma once
#include <algorithm>
#include <cstdint>
#include <random>
#include <vector>

#include "match.hpp"

namespace cst {

constexpr int      B    = 100;   // add placement buckets, d = 1..B
constexpr int      BT   = B + 1; // tail bucket (cancel/exposure only)
constexpr uint32_t TICK = 100;

// Empirical size sampler: sizes with cumulative counts.
struct SizeCdf {
    std::vector<uint32_t> size;
    std::vector<uint64_t> cum;
    void add(uint32_t s, uint64_t count) {
        size.push_back(s);
        cum.push_back((cum.empty() ? 0 : cum.back()) + count);
    }
    uint32_t sample(std::mt19937_64& rng) const {
        if (size.empty()) return 100;
        uint64_t u = std::uniform_int_distribution<uint64_t>(
                         1, cum.back())(rng);
        size_t i = size_t(std::lower_bound(cum.begin(), cum.end(), u) -
                          cum.begin());
        return size[i];
    }
};

struct SymbolParams {
    std::string name;                 // ticker, <= 8 chars
    uint32_t p0 = 0;                  // initial mid, tick-aligned
    uint32_t half_spread_ticks = 1;
    double   lambda[2][B + 1]  = {};  // adds/sec, [side][d], d 1..B
    double   theta[2][BT + 1]  = {};  // cancels per order-second, d 1..BT
    double   mu[2]             = {};  // market-order decisions/sec
    SizeCdf  add_sz, mkt_sz;
};

struct SimStats {
    uint64_t adds = 0, cancels = 0, mkts = 0, fills = 0;
    uint64_t mkt_empty = 0;           // market orders into an empty side
    uint64_t add_skipped = 0;         // placement had no reference / underflow
};

using TsMsg = std::pair<uint64_t, itch::Message>;

// d of a resting order from the opposite best (falls back to the last known
// opposite best while that side is empty). Clamped to [1, BT].
inline int dist_of(lob::Side side, uint32_t price, uint32_t opp) {
    if (!opp) return BT;
    int64_t diff = side == lob::Side::Buy ? int64_t(opp) - int64_t(price)
                                          : int64_t(price) - int64_t(opp);
    int64_t d = (diff + TICK / 2) / TICK;
    return d < 1 ? 1 : d > BT ? BT : int(d);
}

inline SimStats simulate(const SymbolParams& sp, uint16_t locate,
                         uint64_t seed, uint64_t t_start_ns,
                         uint64_t t_end_ns, std::vector<TsMsg>& out) {
    std::mt19937_64 rng(seed);
    SimStats st;
    lob::Book book;
    itch::Stock stock = {' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
    for (size_t i = 0; i < 8 && i < sp.name.size(); ++i) stock[i] = sp.name[i];

    uint64_t next_ref  = (uint64_t(locate) << 40) + 1;
    uint64_t match_seq = uint64_t(locate) << 40;
    std::vector<lob::Fill> fills;
    std::vector<itch::Message> mout;

    // Per-side add-placement CDF over d and total rates.
    double add_cdf[2][B + 1] = {};
    double lam_tot[2] = {};
    for (int s = 0; s < 2; ++s) {
        for (int d = 1; d <= B; ++d) {
            lam_tot[s] += sp.lambda[s][d];
            add_cdf[s][d] = lam_tot[s];
        }
    }

    uint64_t ts = t_start_ns;
    auto push = [&](const itch::Message& m) { out.emplace_back(ts, m); };
    auto emit_add = [&](lob::Side side, uint32_t price, uint32_t shares) {
        uint64_t ref = next_ref++;
        lob::Result r = book.add(ref, side, shares, price);
        if (r != lob::Result::Ok) { ++st.add_skipped; return; }
        itch::AddOrder a;
        a.h.stock_locate = locate;
        a.h.timestamp    = ts;
        a.order_ref      = ref;
        a.side           = side == lob::Side::Buy ? 'B' : 'S';
        a.shares         = shares;
        a.price          = price;
        a.stock          = stock;
        push(a);
        ++st.adds;
    };

    // Seed one order per side so distance references exist from t0.
    uint32_t hs = sp.half_spread_ticks * TICK;
    uint32_t seed_sz = 100;
    {
        std::mt19937_64 r2(seed ^ 0x5eedULL);
        seed_sz = sp.add_sz.sample(r2);
    }
    uint32_t last_bid = sp.p0 > hs ? sp.p0 - hs : TICK;
    uint32_t last_ask = sp.p0 + hs;
    emit_add(lob::Side::Buy, last_bid, seed_sz);
    ++ts;
    emit_add(lob::Side::Sell, last_ask, seed_sz);

    std::exponential_distribution<double> exp1(1.0);
    std::uniform_real_distribution<double> u01(0.0, 1.0);
    double t_sec = 0;
    const double horizon = double(t_end_ns - t_start_ns) / 1e9;

    while (true) {
        // State-dependent total intensity: adds + markets are constant,
        // cancellation is theta(d) summed over resting orders.
        double w_cancel = 0;
        book.for_each_level([&](lob::Side s, const lob::Level& lv) {
            uint32_t opp = s == lob::Side::Buy
                               ? (book.best_ask() ? book.best_ask() : last_ask)
                               : (book.best_bid() ? book.best_bid() : last_bid);
            w_cancel += sp.theta[int(s)][dist_of(s, lv.price, opp)] *
                        lv.order_count;
            return true;
        });
        double lam = lam_tot[0] + lam_tot[1] + sp.mu[0] + sp.mu[1] + w_cancel;
        if (lam <= 0) break;
        t_sec += exp1(rng) / lam;
        if (t_sec >= horizon) break;
        uint64_t nts = t_start_ns + uint64_t(t_sec * 1e9);
        ts = nts > ts ? nts : ts + 1;

        double u = u01(rng) * lam;
        if (u < lam_tot[0] + lam_tot[1]) {                    // ---- limit add
            int s = u < lam_tot[0] ? 0 : 1;
            double v = s == 0 ? u : u - lam_tot[0];
            int d = int(std::lower_bound(&add_cdf[s][1], &add_cdf[s][B + 1],
                                         v) - &add_cdf[s][0]);
            if (d > B) d = B;
            lob::Side side = lob::Side(s);
            uint32_t opp = side == lob::Side::Buy
                               ? (book.best_ask() ? book.best_ask() : last_ask)
                               : (book.best_bid() ? book.best_bid() : last_bid);
            if (!opp) { ++st.add_skipped; continue; }
            uint32_t off = uint32_t(d) * TICK;
            if (side == lob::Side::Buy && opp <= off) { ++st.add_skipped; continue; }
            uint32_t price = side == lob::Side::Buy ? opp - off : opp + off;
            emit_add(side, price, sp.add_sz.sample(rng));
        } else if (u < lam_tot[0] + lam_tot[1] + sp.mu[0] + sp.mu[1]) {
            int s = u < lam_tot[0] + lam_tot[1] + sp.mu[0] ? 0 : 1; // -- market
            lob::Side side = lob::Side(s);
            bool opp_empty = side == lob::Side::Buy ? !book.best_ask()
                                                    : !book.best_bid();
            if (opp_empty) { ++st.mkt_empty; continue; }
            lob::MatchRequest req;
            req.locate = locate;  req.timestamp = ts;
            req.ref    = next_ref++;
            req.side   = side;
            req.shares = sp.mkt_sz.sample(rng);
            req.market = true;
            req.stock  = stock;
            mout.clear();
            lob::MatchOutcome mo =
                lob::match_submit(book, req, match_seq, fills, mout);
            if (mo.result == lob::Result::Ok) {
                for (const auto& m : mout) push(m);
                st.fills += fills.size();
                ++st.mkts;
            }
        } else {                                              // ---- cancel
            double v = u - (lam_tot[0] + lam_tot[1] + sp.mu[0] + sp.mu[1]);
            const lob::Order* victim = nullptr;
            book.for_each_level([&](lob::Side s, const lob::Level& lv) {
                uint32_t opp = s == lob::Side::Buy
                                   ? (book.best_ask() ? book.best_ask()
                                                      : last_ask)
                                   : (book.best_bid() ? book.best_bid()
                                                      : last_bid);
                double w = sp.theta[int(s)][dist_of(s, lv.price, opp)] *
                           lv.order_count;
                if (v < w) {
                    uint32_t k = std::uniform_int_distribution<uint32_t>(
                                     0, lv.order_count - 1)(rng);
                    const lob::Order* o = lv.head;
                    while (k--) o = o->next;
                    victim = o;
                    return false;
                }
                v -= w;
                return true;
            });
            if (victim) {
                itch::OrderDelete dmsg;
                dmsg.h.stock_locate = locate;
                dmsg.h.timestamp    = ts;
                dmsg.order_ref      = victim->ref;
                push(dmsg);
                book.remove(victim->ref);
                ++st.cancels;
            }
        }
        if (book.best_bid()) last_bid = book.best_bid();
        if (book.best_ask()) last_ask = book.best_ask();
    }
    return st;
}

}  // namespace cst
