#include "adapter.hpp"

#include <algorithm>
#include <cmath>

namespace lob {

const char* to_string(Reject r) {
    switch (r) {
        case Reject::None:               return "None";
        case Reject::Unparseable:        return "Unparseable";
        case Reject::UnknownReference:   return "UnknownReference";
        case Reject::InvariantViolation: return "InvariantViolation";
        case Reject::EconomicallyAbsurd: return "EconomicallyAbsurd";
        default:                         return "?";
    }
}

size_t sample_index(const std::vector<double>& logits,
                    const AdapterConfig& cfg, std::mt19937_64& rng) {
    size_t n = logits.size();
    if (n == 0) return 0;
    double temp = cfg.temperature > 0 ? cfg.temperature : 1.0;

    // Optional top-k: keep the k highest logits, mask the rest to -inf.
    std::vector<double> z(logits);
    if (cfg.top_k > 0 && size_t(cfg.top_k) < n) {
        std::vector<double> sorted(logits);
        std::nth_element(sorted.begin(), sorted.begin() + (n - cfg.top_k),
                         sorted.end());
        double kth = sorted[n - cfg.top_k];   // k-th largest threshold
        for (double& v : z) if (v < kth) v = -std::numeric_limits<double>::infinity();
    }

    // Softmax with temperature, numerically stabilized.
    double mx = -std::numeric_limits<double>::infinity();
    for (double v : z) mx = std::max(mx, v);
    double sum = 0;
    for (double& v : z) {
        v = std::isfinite(v) ? std::exp((v - mx) / temp) : 0.0;
        sum += v;
    }
    if (!(sum > 0)) return 0;
    double u = std::uniform_real_distribution<double>(0.0, sum)(rng);
    double acc = 0;
    for (size_t i = 0; i < n; ++i) {
        acc += z[i];
        if (u < acc) return i;
    }
    return n - 1;
}

uint64_t Adapter::head_ref_at(Side side, uint32_t price) const {
    uint64_t ref = 0;
    book_.for_each_level([&](Side s, const Level& lv) {
        if (s == side && lv.price == price && lv.head) {
            ref = lv.head->ref;
            return false;   // stop
        }
        // Levels are best-first per side; once past `price` on this side we
        // can't match, but for_each_level spans both sides, so just continue.
        return true;
    });
    return ref;
}

Outcome Adapter::submit(const EmittedAction& a) {
    Outcome out;
    auto reject = [&](Reject r) -> Outcome {
        out.reject = r;
        ++reject_[static_cast<size_t>(r)];
        ++total_rejected_;
        return out;
    };

    // --- 1. structural (Unparseable) ---
    // Cancel targets a resting order by (side, price); its `shares` field is
    // unused, so only Limit/Market require a positive size.
    if (a.side != Side::Buy && a.side != Side::Sell) return reject(Reject::Unparseable);
    if (a.kind != ActionKind::Cancel && a.shares == 0) return reject(Reject::Unparseable);
    if (a.kind == ActionKind::Limit && a.price == 0) return reject(Reject::Unparseable);
    if (a.kind == ActionKind::Cancel && a.price == 0) return reject(Reject::Unparseable);

    // --- 2. economic plausibility (EconomicallyAbsurd) ---
    if (a.shares > cfg_.max_shares) return reject(Reject::EconomicallyAbsurd);
    if (a.kind == ActionKind::Limit) {
        if (a.price > cfg_.abs_price_cap) return reject(Reject::EconomicallyAbsurd);
        // Judge against the opposite touch when one exists.
        uint32_t opp = a.side == Side::Buy ? book_.best_ask() : book_.best_bid();
        if (opp) {
            double rel = std::fabs(double(a.price) / double(opp) - 1.0);
            if (rel > cfg_.max_rel_price) return reject(Reject::EconomicallyAbsurd);
        }
    }

    // --- 3. reference resolution (UnknownReference) ---
    uint64_t cancel_ref = 0;
    if (a.kind == ActionKind::Cancel) {
        cancel_ref = head_ref_at(a.side, a.price);
        if (!cancel_ref) return reject(Reject::UnknownReference);
    }

    // --- 4. route + apply. Every primitive below is pre-validated to Ok;
    //        a non-Ok return is a defensive InvariantViolation and, because
    //        the primitives reject before mutating, leaves the book intact. ---
    switch (a.kind) {
        case ActionKind::Cancel: {
            const Order* o = book_.find(cancel_ref);
            uint32_t sh = o ? o->shares : 0;
            if (book_.remove(cancel_ref) != Result::Ok)
                return reject(Reject::InvariantViolation);
            out.canceled = sh;
            break;
        }
        case ActionKind::Market: {
            MatchRequest req;
            req.timestamp = 0;
            req.ref       = next_ref_++;
            req.side      = a.side;
            req.shares    = a.shares;
            req.market    = true;
            emit_.clear();
            MatchOutcome mo = match_submit(book_, req, match_seq_, fills_, emit_);
            if (mo.result != Result::Ok) { --next_ref_; return reject(Reject::InvariantViolation); }
            out.filled = mo.filled;
            break;
        }
        case ActionKind::Limit: {
            uint32_t opp = a.side == Side::Buy ? book_.best_ask() : book_.best_bid();
            bool marketable = opp && (a.side == Side::Buy ? a.price >= opp
                                                          : a.price <= opp);
            if (marketable) {
                MatchRequest req;
                req.timestamp = 0;
                req.ref       = next_ref_++;
                req.side      = a.side;
                req.shares    = a.shares;
                req.limit     = a.price;
                req.market    = false;
                emit_.clear();
                MatchOutcome mo = match_submit(book_, req, match_seq_, fills_, emit_);
                if (mo.result != Result::Ok) { --next_ref_; return reject(Reject::InvariantViolation); }
                out.filled = mo.filled;
                out.rested = mo.rested;
            } else {
                uint64_t ref = next_ref_++;
                if (book_.add(ref, a.side, a.shares, a.price) != Result::Ok) {
                    --next_ref_;
                    return reject(Reject::InvariantViolation);
                }
                out.rested = a.shares;
            }
            break;
        }
    }
    ++applied_;
    return out;
}

BookView Adapter::state() const {
    BookView v;
    v.bids.reserve(cfg_.feedback_levels);
    v.asks.reserve(cfg_.feedback_levels);
    book_.for_each_level([&](Side s, const Level& lv) {
        auto& dst = s == Side::Buy ? v.bids : v.asks;
        if (dst.size() < cfg_.feedback_levels)
            dst.push_back({lv.price, lv.total_shares});
        return true;
    });
    v.best_bid = book_.best_bid();
    v.best_ask = book_.best_ask();
    v.two_sided = v.best_bid && v.best_ask;
    v.spread = v.two_sided ? v.best_ask - v.best_bid : 0;
    return v;
}

}  // namespace lob
