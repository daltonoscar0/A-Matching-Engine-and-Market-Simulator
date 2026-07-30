// itch_tokenize_fit: fit-mode half of the ITCH ingest. Fits the SIZE
// quantile edges and DT log-spaced edges for one ticker on one or more BX
// TRAIN days (pooled), and freezes them into the OFTK manifest. The only
// path from data statistics to bin edges is this tool -> manifest; the
// apply tool (itch_tokenize) cannot fit (tape's fit/apply split).
// Fit sample = the same event stream tokenize emits: in-window expanded
// events of the target symbol (sizes exact; dts nonzero only).
//
// Usage:
//   itch_tokenize_fit --manifest M --ticker T [--tick 100]
//                     <day-file>... [--i-am-running-the-final-comparison]
#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "../src/dataset.hpp"
#include "../src/itch_tokenize.hpp"
#include "../src/oftk_fit.hpp"

namespace {

std::vector<uint8_t> slurp(const char* path) {
    std::vector<uint8_t> buf;
    FILE* f = std::fopen(path, "rb");
    if (!f) return buf;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n > 0) {
        buf.resize(size_t(n));
        if (std::fread(buf.data(), 1, size_t(n), f) != size_t(n)) buf.clear();
    }
    std::fclose(f);
    return buf;
}

std::string trim_stock(const itch::Stock& s) {
    std::string t(s.begin(), s.end());
    while (!t.empty() && t.back() == ' ') t.pop_back();
    return t;
}

std::string basename_of(const std::string& p) {
    size_t k = p.find_last_of('/');
    return k == std::string::npos ? p : p.substr(k + 1);
}

}  // namespace

int main(int argc, char** argv) {
    std::string ticker, manifest_path;
    int64_t tick = 100;
    bool override_flag = false;
    std::vector<const char*> days;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", what);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--ticker") ticker = next("--ticker");
        else if (a == "--manifest") manifest_path = next("--manifest");
        else if (a == "--tick") tick = std::atoll(next("--tick"));
        else if (a == dataset::kOverrideFlag) override_flag = true;
        else days.push_back(argv[i]);
    }
    if (ticker.empty() || manifest_path.empty() || days.empty()) {
        std::fprintf(stderr,
                     "usage: itch_tokenize_fit --manifest M --ticker T "
                     "[--tick 100] <day-file>...\n");
        return 2;
    }

    std::vector<int64_t> sizes, dts;
    uint64_t rows = 0;
    int64_t last_ts = 0;
    std::string files;
    for (const char* path : days) {
        dataset::enforce(path, override_flag);
        std::vector<uint8_t> wire = slurp(path);
        if (wire.empty()) {
            std::fprintf(stderr, "cannot read %s\n", path);
            return 1;
        }
        bool framing_error = false;
        auto dir =
            lob::scan_stock_directory(wire.data(), wire.size(), framing_error);
        if (framing_error) {
            std::fprintf(stderr, "framing error in %s\n", path);
            return 1;
        }
        uint16_t locate = 0;
        bool found = false;
        for (const auto& d : dir)
            if (trim_stock(d.stock) == ticker) {
                locate = d.h.stock_locate;
                found = true;
                break;
            }
        if (!found) {
            std::fprintf(stderr, "ticker %s not in %s directory\n",
                         ticker.c_str(), path);
            return 1;
        }
        ingest::IngestResult res;
        std::string err;
        if (!ingest::ingest_day(wire.data(), wire.size(), locate, nullptr,
                                res, &err)) {
            std::fprintf(stderr, "ingest FAILED on %s: %s\n", path,
                         err.c_str());
            return 1;
        }
        for (const ingest::Event& e : res.events) {
            if (!e.in_win) continue;
            sizes.push_back(int64_t(e.size));
            if (e.dt_ns > 0) dts.push_back(e.dt_ns);
            last_ts = std::max(last_ts, int64_t(e.ts));
            ++rows;
        }
        files += (files.empty() ? "" : ",") + basename_of(path);
        std::printf("%s: %" PRIu64 " in-window events pooled\n", path,
                    res.events_inwindow);
    }
    if (sizes.empty()) {
        std::fprintf(stderr, "no in-window events - nothing to fit\n");
        return 1;
    }

    oftk::TickerEntry entry;
    entry.bins.tick_size = tick;
    entry.bins.size_edges = oftk::fit::fit_size_edges(sizes);
    entry.bins.dt_edges_ns = oftk::fit::fit_dt_edges(dts);
    if (!entry.bins.valid()) {
        std::fprintf(stderr, "fitted bins failed validity check\n");
        return 1;
    }
    // Provenance bridged to tape's FitInfo: our train/eval split is by DAY
    // (src/dataset.hpp), not chronological-within-file, so frac = 1.0 over
    // the pooled TRAIN-day events and first_eval_time_ns = last + 1 (tape's
    // frac-1.0 convention).
    entry.fit.messages_file = files;
    entry.fit.train_frac = 1.0;
    entry.fit.rows_total = rows;
    entry.fit.train_end_index = rows;
    entry.fit.last_train_time_ns = last_ts;
    entry.fit.first_eval_time_ns = last_ts + 1;

    oftk::Manifest m;
    {
        std::ifstream in(manifest_path);
        if (in) {
            std::string err;
            if (!oftk::load_manifest(in, m, &err)) {
                std::fprintf(stderr, "existing manifest unreadable: %s\n",
                             err.c_str());
                return 1;
            }
        }
    }
    m.tickers[ticker] = entry;
    std::ofstream out(manifest_path, std::ios::trunc);
    if (!out) {
        std::fprintf(stderr, "cannot write %s\n", manifest_path.c_str());
        return 1;
    }
    oftk::write_manifest(out, m);

    std::printf("%s: fit on %" PRIu64 " events (%zu days)\nsize_edges:",
                ticker.c_str(), rows, days.size());
    for (int64_t e : entry.bins.size_edges) std::printf(" %lld", (long long)e);
    std::printf("\ndt_edges_ns:");
    for (int64_t e : entry.bins.dt_edges_ns) std::printf(" %lld", (long long)e);
    std::printf("\nwrote %s\n", manifest_path.c_str());
    return 0;
}
