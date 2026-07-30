// replay_itch - replay a real NASDAQ BinaryFILE ITCH day through a BookSet
// and report everything needed to judge the Phase 1 milestone:
//   - messages seen / applied / skipped-by-type (histogram of the real day)
//   - rejects broken down by (Result x message type); first N with full
//     context (bytes, decoded fields, book state, order history)
//   - books touched, peak open orders (global + per book), peak depth
//   - final state per book (--books) and aggregate ledger
// Optional --bench runs the BENCH.md methodology on the same file: untimed
// throughput pass (best of R) + a latency pass timing decode+apply per
// book message (skipped frames advance untimed, they are not book work).
//
// Usage: replay_itch <file> [--max N] [--books] [--bench R] [--reserve N]
#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <vector>

#include "../src/dataset.hpp"
#include "../src/itch_replay.hpp"

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

static std::string ticker(const std::vector<itch::StockDirectory>& dir,
                          uint16_t locate) {
    for (const auto& d : dir)
        if (d.h.stock_locate == locate) {
            std::string s(d.stock.data(), 8);
            while (!s.empty() && s.back() == ' ') s.pop_back();
            return s;
        }
    return "?";
}

// Chronological history of every book message touching `ref`, for reject
// diagnosis. One extra pass over the buffer per call; only runs on failure.
static void print_history(const uint8_t* data, size_t size, uint64_t ref,
                          size_t limit) {
    itch::FrameReader rd{data, size};
    uint64_t idx = 0;
    size_t printed = 0;
    while (auto m = rd.next()) {
        bool hit = lob::ref_of(*m) == ref;
        if (auto* u = std::get_if<itch::OrderReplace>(&*m))
            hit = hit || u->new_order_ref == ref;
        if (hit) {
            std::printf("      msg %" PRIu64 ": %s\n", idx,
                        lob::describe(*m).c_str());
            if (++printed == limit) {
                std::printf("      ... (history capped at %zu)\n", limit);
                break;
            }
        }
        ++idx;
    }
}

static void print_reject(const lob::RejectContext& c,
                         const std::vector<itch::StockDirectory>& dir,
                         const uint8_t* data, size_t size) {
    std::printf("  reject at msg %" PRIu64 " (body offset %zu): %s\n",
                c.msg_index, c.body_offset, lob::to_string(c.result));
    std::printf("    decoded: %s  [%s]\n", lob::describe(c.msg).c_str(),
                ticker(dir, c.locate).c_str());
    size_t blen = lob::body_len_of(itch::type_of(c.msg));
    std::printf("    bytes:  ");
    for (size_t i = 0; i < blen && c.body_offset + i < size; ++i)
        std::printf("%02x", data[c.body_offset + i]);
    std::printf("\n");
    std::printf("    book(loc %u) at reject: best_bid=%u best_ask=%u open=%zu\n",
                c.locate, c.best_bid, c.best_ask, c.open_orders);
    if (c.ref_found)
        std::printf("    referenced order resting: %c %u @ %u\n", c.ref_side,
                    c.ref_shares, c.ref_price);
    else
        std::printf("    referenced order ref=%" PRIu64 " not in book\n",
                    lob::ref_of(c.msg));
    std::printf("    history of that ref:\n");
    print_history(data, size, lob::ref_of(c.msg), 50);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
            "usage: %s <itch_file> [--max N] [--books] [--bench R] "
            "[--reserve N] [%s]\n", argv[0], dataset::kOverrideFlag);
        return 2;
    }
    size_t max_msgs = size_t(-1), reserve = 0;
    bool print_books = false;
    int bench_repeat = 0;
    bool final_comparison = false;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--max") && i + 1 < argc)
            max_msgs = strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--books")) print_books = true;
        else if (!std::strcmp(argv[i], "--bench") && i + 1 < argc)
            bench_repeat = atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--reserve") && i + 1 < argc)
            reserve = strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], dataset::kOverrideFlag))
            final_comparison = true;
        else { std::fprintf(stderr, "bad arg: %s\n", argv[i]); return 2; }
    }
    dataset::enforce(argv[1], final_comparison);

    std::vector<uint8_t> wire = slurp(argv[1]);
    std::printf("file: %s (%.1f MB)\n", argv[1], wire.size() / 1e6);

    // ---- stock directory ---------------------------------------------------
    bool dir_err = false;
    auto dir = lob::scan_stock_directory(wire.data(), wire.size(), dir_err);
    if (dir_err) { std::fprintf(stderr, "directory scan: framing error\n"); return 1; }
    uint16_t max_loc = 0;
    for (auto& d : dir) max_loc = std::max(max_loc, d.h.stock_locate);
    std::printf("directory: %zu 'R' entries, locates 1..%u\n", dir.size(),
                max_loc);

    // ---- replay ------------------------------------------------------------
    lob::BookSet set(reserve);
    lob::ReplayOptions opt;
    opt.max_messages = max_msgs;
    auto t0 = Clock::now();
    auto out = lob::replay_itch(wire.data(), wire.size(), set, opt);
    auto t1 = Clock::now();
    const lob::ReplayStats& st = out.stats;
    double secs = std::chrono::duration<double>(t1 - t0).count();

    std::printf("\nreplay: %.3fs, %" PRIu64 " frames = %" PRIu64
                " book msgs + %" PRIu64 " skipped\n",
                secs, st.decoded + st.skipped, st.decoded, st.skipped);
    if (st.stream_error) {
        std::printf("STREAM ERROR at byte %zu\n", st.stream_error_pos);
        return 1;
    }
    std::printf("book msgs by type:");
    for (int t = 0; t < 256; ++t)
        if (st.decoded_by[t])
            std::printf("  %c %" PRIu64, char(t), st.decoded_by[t]);
    std::printf("\nskipped by type: ");
    for (int t = 0; t < 256; ++t)
        if (st.skips[t])
            std::printf("  %c %" PRIu64, char(t), st.skips[t]);
    std::printf("\n");

    std::printf("\napplied=%" PRIu64 " rejected=%" PRIu64 "\n", st.applied,
                st.rejected);
    if (st.rejected) {
        std::printf("rejects by (type x result):\n");
        for (int t = 0; t < 256; ++t)
            for (size_t r = 0; r < 8; ++r)
                if (st.rejects_by[t][r])
                    std::printf("  %c x %s: %" PRIu64 "\n", char(t),
                                lob::to_string(lob::Result(r)),
                                st.rejects_by[t][r]);
        std::printf("\nfirst %zu rejects in full context:\n",
                    out.reject_samples.size());
        for (const auto& c : out.reject_samples)
            print_reject(c, dir, wire.data(), wire.size());
    }

    // ---- book stats + final state ------------------------------------------
    uint64_t peak_book_open = 0, peak_book_levels = 0;
    uint16_t peak_open_loc = 0;
    for (size_t l = 0; l < st.per_book.size(); ++l) {
        if (st.per_book[l].peak_open > peak_book_open) {
            peak_book_open = st.per_book[l].peak_open;
            peak_open_loc  = uint16_t(l);
        }
        peak_book_levels = std::max(peak_book_levels,
                                    st.per_book[l].peak_levels);
    }
    std::printf("\nbooks touched: %" PRIu64
                ", peak open orders all books: %" PRIu64
                ", peak single book: %" PRIu64 " (%s), peak levels in a book: %"
                PRIu64 "\n",
                st.books_touched, st.peak_open_total, peak_book_open,
                ticker(dir, peak_open_loc).c_str(), peak_book_levels);

    uint64_t open = 0, added = 0, executed = 0, canceled = 0, resting = 0;
    bool audits_clean = true;
    set.for_each_book([&](uint16_t loc, lob::Book& b) {
        open     += b.open_orders();
        added    += b.shares_added();
        executed += b.shares_executed();
        canceled += b.shares_canceled();
        resting  += b.shares_resting();
        std::string err = b.audit();
        if (!err.empty()) {
            audits_clean = false;
            std::printf("AUDIT FAIL book %u (%s): %s\n", loc,
                        ticker(dir, loc).c_str(), err.c_str());
        }
        if (print_books && st.per_book[loc].msgs)
            std::printf("  book %5u %-8s msgs=%-9" PRIu64 " open=%-6zu "
                        "bid=%u ask=%u peak_open=%" PRIu64 "\n",
                        loc, ticker(dir, loc).c_str(), st.per_book[loc].msgs,
                        b.open_orders(), b.best_bid(), b.best_ask(),
                        st.per_book[loc].peak_open);
    });
    std::printf("final: open=%" PRIu64 " shares added=%" PRIu64
                " executed=%" PRIu64 " canceled=%" PRIu64 " resting=%" PRIu64
                " (conservation %s), audits %s\n",
                open, added, executed, canceled, resting,
                added == executed + canceled + resting ? "OK" : "BROKEN",
                audits_clean ? "clean" : "FAILED");
    if (!audits_clean || added != executed + canceled + resting) return 1;

    // ---- bench mode ---------------------------------------------------------
    if (bench_repeat > 0) {
        double best_rate = 0, best_frames = 0;
        for (int r = 0; r < bench_repeat; ++r) {
            lob::BookSet bset(reserve);
            itch::FrameReader rd{wire.data(), wire.size()};
            size_t n = 0, rejected = 0;
            auto b0 = Clock::now();
            while (auto m = rd.next()) {
                if (bset.apply(*m) != lob::Result::Ok) ++rejected;
                ++n;
            }
            auto b1 = Clock::now();
            if (rd.error) { std::fprintf(stderr, "stream error\n"); return 1; }
            double bs = std::chrono::duration<double>(b1 - b0).count();
            best_rate   = std::max(best_rate, n / bs);
            best_frames = std::max(best_frames, (n + rd.skipped) / bs);
            std::printf("bench pass1[%d]: %zu book msgs (+%" PRIu64
                        " skipped) in %.3fs -> %.2fM msgs/sec, %.2fM "
                        "frames/sec (rejected=%zu)\n",
                        r, n, rd.skipped, bs, n / bs / 1e6,
                        (n + rd.skipped) / bs / 1e6, rejected);
        }

        // Latency pass: manual framing so the timed region is exactly
        // decode+apply of book messages; skips advance untimed.
        std::vector<uint32_t> lat_ns;
        lat_ns.reserve(st.decoded);
        {
            lob::BookSet bset(reserve);
            size_t pos = 0;
            while (pos < wire.size()) {
                uint16_t len = itch::get_u16(wire.data() + pos);
                const uint8_t* body = wire.data() + pos + 2;
                if (!itch::is_book_type(char(body[0]))) { pos += 2 + len; continue; }
                auto l0 = Clock::now();
                auto m = itch::decode(body, len);
                bset.apply(*m);
                auto l1 = Clock::now();
                lat_ns.push_back(uint32_t(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        l1 - l0).count()));
                pos += 2 + len;
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
        std::printf("throughput (best of %d): %.2fM book msgs/sec "
                    "(%.2fM frames/sec)\n",
                    bench_repeat, best_rate / 1e6, best_frames / 1e6);
    }
    return st.rejected == 0 ? 0 : 1;
}
