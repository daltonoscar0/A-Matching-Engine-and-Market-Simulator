// field_sanity - decode-offset sanity check against reality (Step 1a).
//
// The replay proved internal consistency; it cannot prove we read the right
// bytes. Conservation balances perfectly even if `shares` is decoded from the
// wrong offset, because the same wrong field is added and later removed. The
// external check is distributional: displayed US equity flow has round-lot
// structure (hard spike at 100, multiples of 100 dominant, thin odd-lot
// tail), and prices divided by 10^4 must be plausible dollar values. A wrong
// offset gives arbitrary values with no such structure.
//
// Usage: field_sanity <itch_file>
#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <vector>

#include "../src/dataset.hpp"
#include "../src/itch.hpp"

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

using Hist = std::unordered_map<uint32_t, uint64_t>;

// Weighted percentile over a value->count histogram.
static uint32_t percentile(const Hist& h, uint64_t total, double p) {
    std::vector<std::pair<uint32_t, uint64_t>> v(h.begin(), h.end());
    std::sort(v.begin(), v.end());
    uint64_t target = uint64_t(p * double(total - 1));
    uint64_t seen = 0;
    for (auto& [val, cnt] : v) {
        seen += cnt;
        if (seen > target) return val;
    }
    return v.empty() ? 0 : v.back().first;
}

static void print_top(const Hist& h, uint64_t total, size_t n, bool dollars) {
    std::vector<std::pair<uint32_t, uint64_t>> v(h.begin(), h.end());
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) {
        return a.second != b.second ? a.second > b.second : a.first < b.first;
    });
    if (v.size() > n) v.resize(n);
    for (auto& [val, cnt] : v) {
        if (dollars)
            std::printf("    %12.4f  %10" PRIu64 "  (%5.2f%%)\n", val / 1e4,
                        cnt, 100.0 * double(cnt) / double(total));
        else
            std::printf("    %8u  %10" PRIu64 "  (%5.2f%%)\n", val, cnt,
                        100.0 * double(cnt) / double(total));
    }
}

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
    std::printf("file: %s (%.1f MB)\n", argv[1], wire.size() / 1e6);

    Hist shares_h, price_h;
    uint64_t n_af = 0, sum_af_shares = 0;
    uint64_t n_u = 0, sum_u_shares = 0;   // to reconcile the ledger mean
    itch::FrameReader rd{wire.data(), wire.size()};
    while (auto m = rd.next()) {
        const itch::AddOrder* a = nullptr;
        if (auto* p = std::get_if<itch::AddOrder>(&*m)) a = p;
        else if (auto* p2 = std::get_if<itch::AddOrderMpid>(&*m)) a = &p2->add;
        else if (auto* u = std::get_if<itch::OrderReplace>(&*m)) {
            ++n_u; sum_u_shares += u->shares;
            continue;
        } else {
            continue;
        }
        ++n_af;
        sum_af_shares += a->shares;
        ++shares_h[a->shares];
        ++price_h[a->price];
    }
    if (rd.error) { std::fprintf(stderr, "STREAM ERROR\n"); return 1; }

    // ---- shares ------------------------------------------------------------
    uint64_t mult100 = 0, odd_lt100 = 0;
    for (auto& [val, cnt] : shares_h) {
        if (val % 100 == 0) mult100 += cnt;
        if (val < 100) odd_lt100 += cnt;
    }
    std::printf("\nA/F messages: %" PRIu64 "  (distinct share values: %zu)\n",
                n_af, shares_h.size());
    std::printf("shares: top 20 by frequency\n");
    print_top(shares_h, n_af, 20, false);
    std::printf("  multiples of 100: %.2f%%   odd lots (<100): %.2f%%\n",
                100.0 * double(mult100) / double(n_af),
                100.0 * double(odd_lt100) / double(n_af));
    std::printf("  median %u  p90 %u  p99 %u  max %u  mean %.1f\n",
                percentile(shares_h, n_af, 0.50),
                percentile(shares_h, n_af, 0.90),
                percentile(shares_h, n_af, 0.99),
                percentile(shares_h, n_af, 1.0),
                double(sum_af_shares) / double(n_af));
    std::printf("  ledger reconciliation: A/F add %" PRIu64 " shares; "
                "U re-adds %" PRIu64 " shares over %" PRIu64
                " replaces -> total added %.3fB over %.1fM add events "
                "(mean %.0f)\n",
                sum_af_shares, sum_u_shares, n_u,
                double(sum_af_shares + sum_u_shares) / 1e9,
                double(n_af + n_u) / 1e6,
                double(sum_af_shares + sum_u_shares) / double(n_af + n_u));

    // ---- price -------------------------------------------------------------
    uint64_t whole_penny = 0, zero_p = 0, over_10k = 0, under_1 = 0;
    for (auto& [val, cnt] : price_h) {
        if (val % 100 == 0) whole_penny += cnt;   // 4 decimals: penny = 100
        if (val == 0) zero_p += cnt;
        if (val > 10000u * 10000u) over_10k += cnt;
        if (val < 10000u && val > 0) under_1 += cnt;
    }
    std::printf("\nprice (dollars = raw/1e4): top 20 by frequency\n");
    print_top(price_h, n_af, 20, true);
    std::printf("  whole-penny: %.2f%%   zero: %" PRIu64
                "   <$1: %.2f%%   >$10000: %" PRIu64 "\n",
                100.0 * double(whole_penny) / double(n_af), zero_p,
                100.0 * double(under_1) / double(n_af), over_10k);
    std::printf("  p1 $%.4f  p25 $%.4f  median $%.4f  p75 $%.4f  p99 $%.4f  "
                "min $%.4f  max $%.4f\n",
                percentile(price_h, n_af, 0.01) / 1e4,
                percentile(price_h, n_af, 0.25) / 1e4,
                percentile(price_h, n_af, 0.50) / 1e4,
                percentile(price_h, n_af, 0.75) / 1e4,
                percentile(price_h, n_af, 0.99) / 1e4,
                percentile(price_h, n_af, 0.0) / 1e4,
                percentile(price_h, n_af, 1.0) / 1e4);
    return 0;
}
