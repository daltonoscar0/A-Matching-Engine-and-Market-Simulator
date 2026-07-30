// bench_replay - replay a framed ITCH stream file through the book and
// report throughput plus per-message decode+apply latency percentiles.
//
// Two passes over the same data:
//   pass 1 (throughput): tight loop, one clock read total -> msgs/sec.
//   pass 2 (latency): steady_clock read around each decode+apply. Clock
//     reads cost ~20ns each on this class of hardware, so percentiles carry
//     that overhead; noted in BENCH.md.
//
// Usage: bench_replay <stream_file> [repeat_pass1=3] [mode] [reserve=1048576]
//   mode "set": route through a BookSet on stock_locate (for interleaved
//   multi-symbol streams). Default: single Book, single-symbol path.
//   reserve: order-pool buckets pre-reserved per book. Must exceed the peak
//   per-book open-order count or rehash spikes come back (RESULTS.md
//   2026-07-30).
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../src/book.hpp"
#include "../src/bookset.hpp"
#include "../src/feed.hpp"
#include "../src/itch.hpp"

using Clock = std::chrono::steady_clock;

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

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <stream_file> [repeat=3]\n", argv[0]);
        return 2;
    }
    int repeat = argc > 2 ? atoi(argv[2]) : 3;
    bool use_set = argc > 3 && std::strcmp(argv[3], "set") == 0;
    size_t reserve = argc > 4 ? strtoull(argv[4], nullptr, 10) : (1 << 20);
    std::vector<uint8_t> wire = slurp(argv[1]);

    // ---- pass 1: throughput ------------------------------------------------
    double best_rate = 0;
    size_t n_msgs = 0;
    for (int r = 0; r < repeat; ++r) {
        lob::Book book;
        book.reserve(reserve);
        lob::BookSet set(reserve);
        itch::FrameReader rd{wire.data(), wire.size()};
        size_t n = 0, rejected = 0;
        auto t0 = Clock::now();
        if (use_set) {
            while (auto m = rd.next()) {
                if (set.apply(*m) != lob::Result::Ok) ++rejected;
                ++n;
            }
        } else {
            while (auto m = rd.next()) {
                if (lob::apply(book, *m) != lob::Result::Ok) ++rejected;
                ++n;
            }
        }
        auto t1 = Clock::now();
        if (rd.error) { std::fprintf(stderr, "stream error\n"); return 1; }
        double secs = std::chrono::duration<double>(t1 - t0).count();
        double rate = n / secs;
        best_rate = std::max(best_rate, rate);
        n_msgs = n;
        size_t open = book.open_orders();
        bool audit_ok = book.audit().empty();
        if (use_set) {
            open = 0;
            set.for_each_book([&](uint16_t, lob::Book& b) {
                open += b.open_orders();
                if (!b.audit().empty()) audit_ok = false;
            });
        }
        std::printf("pass1[%d]: %zu msgs in %.3fs -> %.2fM msgs/sec "
                    "(rejected=%zu, open=%zu%s)\n",
                    r, n, secs, rate / 1e6, rejected, open,
                    use_set ? ", bookset" : "");
        if (!audit_ok) { std::fprintf(stderr, "AUDIT FAIL\n"); return 1; }
    }

    // ---- pass 2: per-message latency --------------------------------------
    std::vector<uint32_t> lat_ns;
    lat_ns.reserve(n_msgs);
    {
        lob::Book book;
        book.reserve(reserve);
        lob::BookSet set(reserve);
        itch::FrameReader rd{wire.data(), wire.size()};
        // Manual framing so the timed region is exactly decode+apply.
        while (rd.pos < rd.size) {
            uint16_t len = itch::get_u16(rd.data + rd.pos);
            const uint8_t* body = rd.data + rd.pos + 2;
            auto t0 = Clock::now();
            auto m = itch::decode(body, len);
            if (use_set) set.apply(*m);
            else         lob::apply(book, *m);
            auto t1 = Clock::now();
            lat_ns.push_back(uint32_t(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
                    .count()));
            rd.pos += 2 + len;
        }
    }
    std::sort(lat_ns.begin(), lat_ns.end());
    auto pct = [&](double p) {
        return lat_ns[size_t(p * (lat_ns.size() - 1))];
    };
    std::printf("latency (decode+apply, incl ~clock overhead): "
                "p50=%uns p90=%uns p99=%uns p99.9=%uns max=%uns\n",
                pct(0.50), pct(0.90), pct(0.99), pct(0.999),
                lat_ns.back());
    std::printf("throughput (best of %d): %.2fM msgs/sec\n",
                repeat, best_rate / 1e6);
    return 0;
}
