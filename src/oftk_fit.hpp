// Bin fitting for the OFTK factored tokenizer. Deliberately NOT part of
// oftk.hpp: the tokenize tool must be unable to fit bins, so the only path
// from data statistics to bin edges is itch_tokenize_fit -> manifest
// (tape's fit/apply split, kept). Include from tools/itch_tokenize_fit.cpp
// and tests only. Algorithms are tape's fit_core.hpp verbatim so BX-refit
// bins are fitted by the same rules the LOBSTER bins were.
#ifndef EXCHANGE_OFTK_FIT_HPP
#define EXCHANGE_OFTK_FIT_HPP

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "oftk.hpp"

namespace oftk {
namespace fit {

// Force strictly increasing integer edges with edges[0] >= 2, so every
// bucket contains at least one integer and bucket representatives exist.
// Quantiles of heavily discrete distributions (round-lot sizes) otherwise
// produce duplicate edges.
inline void enforce_edge_invariants(std::vector<int64_t>& edges) {
    for (size_t i = 0; i < edges.size(); ++i) {
        int64_t lo = i == 0 ? 2 : edges[i - 1] + 1;
        if (edges[i] < lo) edges[i] = lo;
    }
}

// Nearest-rank quantile of a sorted vector, q in [0,1].
inline int64_t quantile(const std::vector<int64_t>& sorted, double q) {
    size_t idx = static_cast<size_t>(q * double(sorted.size() - 1) + 0.5);
    return sorted[std::min(idx, sorted.size() - 1)];
}

// 7 size edges at quantiles k/8 of the training sizes.
inline std::vector<int64_t> fit_size_edges(std::vector<int64_t> sizes) {
    std::sort(sizes.begin(), sizes.end());
    std::vector<int64_t> edges;
    for (int k = 1; k < kSizeBins; ++k)
        edges.push_back(quantile(sizes, double(k) / kSizeBins));
    enforce_edge_invariants(edges);
    return edges;
}

// 14 log-spaced (geometric) edges anchored at the training p0.1 / p99.9 of
// nonzero inter-event gaps.
inline std::vector<int64_t> fit_dt_edges(std::vector<int64_t> nonzero_dts) {
    std::vector<int64_t> edges;
    if (nonzero_dts.empty()) {  // degenerate input; cover us..10s
        nonzero_dts = {1000, 10000000000LL};
    }
    std::sort(nonzero_dts.begin(), nonzero_dts.end());
    double lo = double(std::max<int64_t>(quantile(nonzero_dts, 0.001), 2));
    double hi = double(std::max<int64_t>(quantile(nonzero_dts, 0.999), 2));
    if (hi <= lo) hi = lo + 1;
    for (int k = 0; k < kDtBins; ++k) {
        double t = double(k) / (kDtBins - 1);
        edges.push_back(int64_t(std::llround(lo * std::pow(hi / lo, t))));
    }
    enforce_edge_invariants(edges);
    return edges;
}

}  // namespace fit
}  // namespace oftk

#endif  // EXCHANGE_OFTK_FIT_HPP
