// bench_adapter - throughput of the Phase 2 closed loop: generate an action,
// submit it (validate + route + apply), and build the state feedback the
// model would condition on. NOT comparable to the replay or match rows: this
// times validation + engine + a top-N BookView construction per step, plus
// the generator, not raw book work.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

#include "../src/adapter.hpp"

using Clock = std::chrono::steady_clock;
using namespace lob;

int main(int argc, char** argv) {
    long steps = argc > 1 ? std::atol(argv[1]) : 5'000'000;
    AdapterConfig cfg; cfg.seed = 1;
    Adapter ad(cfg);
    std::mt19937_64 g(2024);
    std::uniform_int_distribution<int> kind(0, 9), sidebit(0, 1),
        off(-30, 30), sz(1, 2000);
    uint32_t base = 100000;

    uint64_t applied = 0, feedback_levels = 0;
    auto t0 = Clock::now();
    for (long i = 0; i < steps; ++i) {
        base += uint32_t(off(g));
        if (base < 5000) base = 5000;
        Side s = sidebit(g) ? Side::Buy : Side::Sell;
        EmittedAction a; a.side = s;
        int k = kind(g);
        if (k < 6) { a.kind = ActionKind::Limit;
                     a.price = base + uint32_t(off(g)) * 100;
                     a.shares = uint32_t(sz(g)); }
        else if (k < 8) { a.kind = ActionKind::Market; a.shares = uint32_t(sz(g)); }
        else {
            BookView v = ad.state();
            const auto& l = s == Side::Buy ? v.bids : v.asks;
            a.kind = ActionKind::Cancel;
            a.price = l.empty() ? 12345 : l[size_t(g()) % l.size()].price;
            a.shares = 1;
        }
        if (ad.submit(a).applied()) ++applied;
        BookView v = ad.state();            // feedback construction, timed
        feedback_levels += v.bids.size() + v.asks.size();
    }
    auto t1 = Clock::now();
    double secs = std::chrono::duration<double>(t1 - t0).count();
    std::printf("adapter loop: %ld steps in %.3fs -> %.2fM steps/sec\n",
                steps, secs, steps / secs / 1e6);
    std::printf("  applied %llu (%.1f%%), rejected %llu, mean feedback levels "
                "%.1f, final open=%zu\n",
                (unsigned long long)applied, 100.0 * double(applied) / double(steps),
                (unsigned long long)ad.rejected(),
                double(feedback_levels) / double(steps), ad.book().open_orders());
    for (int r = 1; r < int(Reject::kCount); ++r)
        std::printf("  reject[%s] = %llu\n", to_string(Reject(r)),
                    (unsigned long long)ad.reject_count(Reject(r)));
    return 0;
}
