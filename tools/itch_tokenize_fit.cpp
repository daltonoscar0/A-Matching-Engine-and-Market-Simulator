// itch_tokenize_fit: fit-mode half of the ITCH ingest. Fits the SIZE
// quantile edges and DT log-spaced edges for one ticker on one or more BX
// TRAIN days (pooled), and freezes them into the OFTK manifest. The only
// path from data statistics to bin edges is this tool -> manifest; the
// apply tool (itch_tokenize) cannot fit (tape's fit/apply split).
// Fit sample = the same event stream tokenize emits: in-window expanded
// events of the target symbol (sizes exact; dts nonzero only).
//
// --train-frac F (default 1.0) fits on the chronological prefix
// floor(F * events) of the pooled stream and records the boundary in the
// manifest fit provenance - tape's within-file split semantics, used by
// the pilot run so held-out loss sees events the bins were never fit on.
// The panel bins stay F = 1.0: this repo's real train/eval split is by
// DAY (src/dataset.hpp), not within-file.
//
// Usage:
//   itch_tokenize_fit --manifest M --ticker T [--tick 100]
//                     [--train-frac F] <day-file>...
//                     [--i-am-running-the-final-comparison]
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
    double train_frac = 1.0;
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
        else if (a == "--train-frac") train_frac = std::atof(next("--train-frac"));
        else if (a == dataset::kOverrideFlag) override_flag = true;
        else days.push_back(argv[i]);
    }
    if (ticker.empty() || manifest_path.empty() || days.empty()) {
        std::fprintf(stderr,
                     "usage: itch_tokenize_fit --manifest M --ticker T "
                     "[--tick 100] <day-file>...\n");
        return 2;
    }
    // The manifest is read AND rewritten; it must never be a raw data
    // file or name a sealed TEST day (review 2026-07-30; refined
    // 2026-07-31 - TRAIN/VAL day tokens in artifact names are fine, the
    // hazard is *.BX_ITCH_50 targets and TEST tokens).
    if (manifest_path.find(".BX_ITCH_50") != std::string::npos ||
        dataset::classify(manifest_path.c_str()) ==
            dataset::Access::TestBlocked) {
        std::fprintf(stderr, "--manifest path %s names a raw data file or "
                             "sealed TEST day - refused\n",
                     manifest_path.c_str());
        return 3;
    }

    if (!(train_frac > 0.0 && train_frac <= 1.0)) {
        std::fprintf(stderr, "--train-frac must be in (0, 1]\n");
        return 2;
    }
    std::vector<int64_t> ev_sizes, ev_dts, ev_ts;
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
            ev_sizes.push_back(int64_t(e.size));
            ev_dts.push_back(e.dt_ns);
            ev_ts.push_back(int64_t(e.ts));
        }
        files += (files.empty() ? "" : ",") + basename_of(path);
        std::printf("%s: %" PRIu64 " in-window events pooled\n", path,
                    res.events_inwindow);
    }
    const uint64_t rows = ev_sizes.size();
    const uint64_t n_train = uint64_t(train_frac * double(rows));
    if (n_train < 2) {
        std::fprintf(stderr, "training split has fewer than 2 events\n");
        return 1;
    }
    std::vector<int64_t> sizes(ev_sizes.begin(),
                               ev_sizes.begin() + ptrdiff_t(n_train));
    std::vector<int64_t> dts;
    for (uint64_t i = 0; i < n_train; ++i)
        if (ev_dts[i] > 0) dts.push_back(ev_dts[i]);

    oftk::TickerEntry entry;
    entry.bins.tick_size = tick;
    entry.bins.size_edges = oftk::fit::fit_size_edges(sizes);
    entry.bins.dt_edges_ns = oftk::fit::fit_dt_edges(dts);
    if (!entry.bins.valid()) {
        std::fprintf(stderr, "fitted bins failed validity check\n");
        return 1;
    }
    // Provenance bridged to tape's FitInfo. Panel bins use frac 1.0 (the
    // real train/eval split is by DAY, src/dataset.hpp) and mark
    // first_eval_time_ns = last + 1, tape's frac-1.0 convention; the pilot
    // uses frac < 1 for a genuine within-file held-out split.
    entry.fit.messages_file = files;
    entry.fit.train_frac = train_frac;
    entry.fit.rows_total = rows;
    entry.fit.train_end_index = n_train;
    entry.fit.last_train_time_ns = ev_ts[n_train - 1];
    entry.fit.first_eval_time_ns =
        n_train < rows ? ev_ts[n_train] : ev_ts[rows - 1] + 1;

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
    // Write-to-temp + rename: a failed write must not destroy the other
    // tickers' frozen bins in the existing manifest (review 2026-07-30).
    const std::string tmp_path = manifest_path + ".tmp";
    {
        std::ofstream out(tmp_path, std::ios::trunc);
        if (!out) {
            std::fprintf(stderr, "cannot write %s\n", tmp_path.c_str());
            return 1;
        }
        oftk::write_manifest(out, m);
        out.flush();
        if (!out) {
            std::fprintf(stderr, "write failed for %s\n", tmp_path.c_str());
            std::remove(tmp_path.c_str());
            return 1;
        }
    }
    if (std::rename(tmp_path.c_str(), manifest_path.c_str()) != 0) {
        std::fprintf(stderr, "cannot rename %s -> %s\n", tmp_path.c_str(),
                     manifest_path.c_str());
        std::remove(tmp_path.c_str());
        return 1;
    }

    std::printf("%s: fit on %" PRIu64 " events (%zu days)\nsize_edges:",
                ticker.c_str(), rows, days.size());
    for (int64_t e : entry.bins.size_edges) std::printf(" %lld", (long long)e);
    std::printf("\ndt_edges_ns:");
    for (int64_t e : entry.bins.dt_edges_ns) std::printf(" %lld", (long long)e);
    std::printf("\nwrote %s\n", manifest_path.c_str());
    return 0;
}
