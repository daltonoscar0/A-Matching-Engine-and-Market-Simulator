// bench_tail - attribute latency spikes to a cause instead of guessing.
//
// Replays a framed stream timing every decode+apply, then reports:
//   * every order-pool rehash (bucket_count change) and the latency of the
//     message that triggered it
//   * the top spikes, annotated with message type, pool size, and whether a
//     rehash happened on that message
//   * spike counts per 500k-message window (uniform spread across the run
//     with no size correlation points at OS preemption, not the code)
//
// Usage: bench_tail <stream_file> [spike_threshold_ns=2000] [reserve=0]
//   reserve != 0 pre-sizes the order pool, to confirm rehash spikes vanish.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../src/book.hpp"
#include "../src/feed.hpp"
#include "../src/itch.hpp"

using Clock = std::chrono::steady_clock;

struct Spike {
    size_t   idx;
    uint32_t ns;
    char     type;
    size_t   open;
    bool     rehash;
};

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
        std::fprintf(stderr,
                     "usage: %s <stream_file> [threshold_ns=2000] [reserve=0]\n",
                     argv[0]);
        return 2;
    }
    uint32_t threshold = argc > 2 ? uint32_t(atoi(argv[2])) : 2000;
    size_t   reserve_n = argc > 3 ? size_t(atoll(argv[3])) : 0;
    std::vector<uint8_t> wire = slurp(argv[1]);

    lob::Book book;
    if (reserve_n) book.reserve(reserve_n);

    std::vector<Spike> spikes;
    std::vector<uint32_t> lat_ns;
    struct Rehash { size_t idx; size_t from, to; uint32_t ns; };
    std::vector<Rehash> rehashes;

    itch::FrameReader rd{wire.data(), wire.size()};
    size_t idx = 0;
    size_t buckets = book.order_buckets();
    while (rd.pos < rd.size) {
        uint16_t len = itch::get_u16(rd.data + rd.pos);
        const uint8_t* body = rd.data + rd.pos + 2;
        char type = char(body[0]);
        auto t0 = Clock::now();
        auto m = itch::decode(body, len);
        lob::apply(book, *m);
        auto t1 = Clock::now();
        uint32_t ns = uint32_t(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
                .count());
        lat_ns.push_back(ns);
        size_t nb = book.order_buckets();
        bool rehashed = nb != buckets;
        if (rehashed) {
            rehashes.push_back({idx, buckets, nb, ns});
            buckets = nb;
        }
        if (ns >= threshold)
            spikes.push_back({idx, ns, type, book.open_orders(), rehashed});
        rd.pos += 2 + len;
        ++idx;
    }

    std::printf("%zu msgs, threshold %uns, reserve %zu\n", idx, threshold,
                reserve_n);

    std::printf("\n-- rehashes of the order pool: %zu --\n", rehashes.size());
    for (auto& r : rehashes)
        std::printf("  msg %9zu: %8zu -> %8zu buckets, that message took %uns\n",
                    r.idx, r.from, r.to, r.ns);

    std::sort(spikes.begin(), spikes.end(),
              [](const Spike& a, const Spike& b) { return a.ns > b.ns; });
    size_t top = std::min<size_t>(spikes.size(), 30);
    std::printf("\n-- top %zu spikes (of %zu >= %uns) --\n", top, spikes.size(),
                threshold);
    for (size_t i = 0; i < top; ++i) {
        auto& s = spikes[i];
        std::printf("  msg %9zu: %8uns type=%c open=%zu%s\n", s.idx, s.ns,
                    s.type, s.open, s.rehash ? "  <-- REHASH" : "");
    }

    std::printf("\n-- spikes per 500k-msg window --\n");
    size_t const win = 500000;
    std::vector<size_t> hist((idx + win - 1) / win, 0);
    for (auto& s : spikes) hist[s.idx / win]++;
    for (size_t w = 0; w < hist.size(); ++w)
        std::printf("  [%4.1fM-%4.1fM): %zu\n", w * win / 1e6,
                    (w + 1) * win / 1e6, hist[w]);

    std::sort(lat_ns.begin(), lat_ns.end());
    auto pct = [&](double p) {
        return lat_ns[size_t(p * (lat_ns.size() - 1))];
    };
    std::printf("\np50=%uns p90=%uns p99=%uns p99.9=%uns p99.99=%uns max=%uns\n",
                pct(0.50), pct(0.90), pct(0.99), pct(0.999), pct(0.9999),
                lat_ns.back());
    return 0;
}
