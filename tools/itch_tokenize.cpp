// itch_tokenize: apply-mode ITCH -> OFTK tokenizer (the tokenizer-facing
// ingest). Parses a BX day, drives the validated reconstruction, and emits
// [TYPE][SIDE][PRICE_OFF][SIZE][DT] tuples for one symbol in the OFTK v2
// binary format + manifest bins (fit those with itch_tokenize_fit; this
// tool cannot fit - tape's fit/apply split).
//
// Round-trip test mode (--roundtrip): the strong test for this pipeline.
//   Layer 1 (lossless): the exact expanded event stream drives a fresh
//   Book (with independent PRICE_OFF recomputation) and its running
//   per-event fingerprint must equal the raw-ITCH-driven Book's.
//   Layer 2 (quantized): tokens must be the exact quantization of the
//   stream, invertible per tape's roundtrip contract.
// --mutate swap|pxoff verifies the test CAN fail: a deliberate adjacent-
// event swap must break layer 1; a PRICE_OFF off-by-one must break the
// recompute (event side) and layer 2 (token side). Exit 1 if a mutation
// goes undetected (test too weak).
//
// Usage:
//   itch_tokenize <day-file> --ticker T [--manifest M] [-o tokens.bin]
//                 [--roundtrip] [--mutate swap|pxoff|size|dt]
//                 [--pxhist f.csv] [--i-am-running-the-final-comparison]
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "../src/dataset.hpp"
#include "../src/itch_tokenize.hpp"

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

// Auxiliary paths (manifest, outputs): -o / --pxhist / --szhist / --dthist
// TRUNCATE
// their target and enforce() only guards the day-file argument (review
// 2026-07-30). Refined 2026-07-31: the first version refused ANY 8-digit
// day token, which also blocked legitimate day-stamped artifact names
// (SPY_20190130.tokens.bin). The actual hazards are (a) targeting a raw
// data file - any *.BX_ITCH_50[.gz] name, whatever the day - and (b)
// naming a sealed TEST day at all. TRAIN/VAL day tokens in artifact
// names are fine.
void require_safe_aux(const char* what, const std::string& p) {
    if (p.empty()) return;
    if (p.find(".BX_ITCH_50") != std::string::npos) {
        std::fprintf(stderr, "%s path %s names a raw data file - refused\n",
                     what, p.c_str());
        std::exit(3);
    }
    if (dataset::classify(p.c_str()) == dataset::Access::TestBlocked) {
        std::fprintf(stderr, "%s path %s names a sealed TEST day - "
                             "refused\n",
                     what, p.c_str());
        std::exit(3);
    }
}

bool find_locate(const uint8_t* data, size_t size, const std::string& ticker,
                 uint16_t& out) {
    bool framing_error = false;
    auto dir = lob::scan_stock_directory(data, size, framing_error);
    if (framing_error) return false;
    for (const auto& d : dir) {
        if (trim_stock(d.stock) == ticker) {
            out = d.h.stock_locate;
            return true;
        }
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    const char* path = nullptr;
    std::string ticker, manifest_path, out_path, pxhist_path, szhist_path,
        dthist_path, mutate;
    bool roundtrip = false, override_flag = false;
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
        else if (a == "-o") out_path = next("-o");
        else if (a == "--pxhist") pxhist_path = next("--pxhist");
        else if (a == "--szhist") szhist_path = next("--szhist");
        else if (a == "--dthist") dthist_path = next("--dthist");
        else if (a == "--mutate") mutate = next("--mutate");
        else if (a == "--roundtrip") roundtrip = true;
        else if (a == dataset::kOverrideFlag) override_flag = true;
        else if (!path) path = argv[i];
        else {
            std::fprintf(stderr, "unexpected argument %s\n", argv[i]);
            return 2;
        }
    }
    if (!path || ticker.empty()) {
        std::fprintf(stderr,
                     "usage: itch_tokenize <day-file> --ticker T "
                     "[--manifest M] [-o tokens.bin] [--roundtrip] "
                     "[--mutate swap|pxoff] [--pxhist f.csv] "
                     "[--szhist f.csv] [--dthist f.csv]\n");
        return 2;
    }
    dataset::enforce(path, override_flag);
    require_safe_aux("--manifest", manifest_path);
    require_safe_aux("-o", out_path);
    require_safe_aux("--pxhist", pxhist_path);
    require_safe_aux("--szhist", szhist_path);
    require_safe_aux("--dthist", dthist_path);

    std::vector<uint8_t> wire = slurp(path);
    if (wire.empty()) {
        std::fprintf(stderr, "cannot read %s\n", path);
        return 1;
    }
    uint16_t locate = 0;
    if (!find_locate(wire.data(), wire.size(), ticker, locate)) {
        std::fprintf(stderr, "ticker %s not in stock directory\n",
                     ticker.c_str());
        return 1;
    }

    oftk::TickerBins bins;
    bool have_bins = false;
    if (!manifest_path.empty()) {
        std::ifstream mf(manifest_path);
        oftk::Manifest m;
        std::string err;
        if (!mf || !oftk::load_manifest(mf, m, &err)) {
            std::fprintf(stderr, "manifest: %s\n", err.c_str());
            return 1;
        }
        auto it = m.tickers.find(ticker);
        if (it == m.tickers.end()) {
            std::fprintf(stderr,
                         "no bins for %s in manifest - run itch_tokenize_fit "
                         "first\n",
                         ticker.c_str());
            return 1;
        }
        bins = it->second.bins;
        have_bins = true;
    }

    ingest::IngestResult res;
    std::string err;
    auto t0 = std::chrono::steady_clock::now();
    if (!ingest::ingest_day(wire.data(), wire.size(), locate,
                            have_bins ? &bins : nullptr, res, &err)) {
        std::fprintf(stderr, "ingest FAILED: %s\n", err.c_str());
        return 1;
    }
    auto t1 = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(t1 - t0).count();

    std::printf("file: %s  ticker: %s (locate %u)\n", path, ticker.c_str(),
                locate);
    std::printf(
        "book msgs applied: %" PRIu64 " total, %" PRIu64
        " on symbol; expanded events: %zu (%" PRIu64 " U expanded); "
        "in-window: %" PRIu64 "\n",
        res.msgs_total, res.msgs_symbol, res.events.size(), res.u_expanded,
        res.events_inwindow);
    std::printf("ingest: %.2fs, %.2fM msgs/sec\n", secs,
                double(res.msgs_total) / secs / 1e6);
    if (res.events_inwindow) {
        double n = double(res.events_inwindow);
        uint64_t inside = 0, tail = 0, deeper = 0;
        for (const auto& [off, c] : res.px_hist) {
            if (off == -1) inside += c;
            if (off > oftk::kPxMax) tail += c;
            if (off > 30) deeper += c;
        }
        std::printf(
            "PRICE_OFF: unk %.3f%%  inside(-1) %.3f%%  tail(>+10) %.3f%%  "
            "(>+30: %.3f%%)\n",
            100.0 * double(res.px_unk_inwin) / n, 100.0 * double(inside) / n,
            100.0 * double(tail) / n, 100.0 * double(deeper) / n);
    }
    {
        const uint64_t adds = res.add_at_level + res.add_new_interior +
                              res.add_new_bottom + res.add_inside +
                              res.add_no_side;
        const double d = double(adds ? adds : 1);
        std::printf(
            "in-window ADD price vs pre-event book (%" PRIu64 " adds): "
            "at-level %.2f%%  NEW-interior %.2f%%  NEW-below-bottom %.2f%% "
            "(of which PX_TAIL %.2f%%)  inside %.2f%%  empty-side %.2f%%\n",
            adds, 100.0 * double(res.add_at_level) / d,
            100.0 * double(res.add_new_interior) / d,
            100.0 * double(res.add_new_bottom) / d,
            100.0 * double(res.add_new_bottom_tail) / d,
            100.0 * double(res.add_inside) / d,
            100.0 * double(res.add_no_side) / d);
    }
    if (have_bins) {
        std::printf("tokens: %zu  dt_zero %.2f%%  dt_tail %.3f%%  sz_hist",
                    res.tokens.size(),
                    100.0 * double(res.stats.dt_zero) /
                        double(res.stats.events ? res.stats.events : 1),
                    100.0 * double(res.stats.dt_tail) /
                        double(res.stats.events ? res.stats.events : 1));
        for (int i = 0; i < oftk::kSizeBins; ++i)
            std::printf(" %" PRIu64, res.stats.sz_hist[i]);
        std::printf("\n");
    }

    if (!pxhist_path.empty()) {
        std::ofstream px(pxhist_path);
        px << "lvl_off,count\n";
        px << "UNK," << res.px_unk_inwin << "\n";
        for (const auto& [off, c] : res.px_hist)
            px << off << "," << c << "\n";
        px.flush();
        if (!px) {
            std::fprintf(stderr, "cannot write %s\n", pxhist_path.c_str());
            return 1;
        }
        std::printf("wrote %s\n", pxhist_path.c_str());
    }

    if (!szhist_path.empty()) {
        // Exact in-window event-size histogram (size,count) - the input
        // the SIZE quantile buckets discretize; used to quantify what the
        // 8-bucket quantization preserves.
        std::map<uint32_t, uint64_t> sh;
        for (const ingest::Event& e : res.events)
            if (e.in_win) ++sh[e.size];
        std::ofstream sz(szhist_path);
        sz << "size,count\n";
        for (const auto& [v, c] : sh) sz << v << "," << c << "\n";
        sz.flush();
        if (!sz) {
            std::fprintf(stderr, "cannot write %s\n", szhist_path.c_str());
            return 1;
        }
        std::printf("wrote %s\n", szhist_path.c_str());
    }

    if (!dthist_path.empty()) {
        // Exact in-window inter-event dt histogram (dt_ns,count) - the DT
        // twin of --szhist. Added 2026-07-31 for the dead-bucket audit:
        // occupancy alone cannot say what a bucket PRESERVES, and the DT
        // quantizer had never been characterised at all.
        std::map<int64_t, uint64_t> dh;
        for (const ingest::Event& e : res.events)
            if (e.in_win) ++dh[e.dt_ns];
        std::ofstream dt(dthist_path);
        dt << "dt_ns,count\n";
        for (const auto& [v, c] : dh) dt << v << "," << c << "\n";
        dt.flush();
        if (!dt) {
            std::fprintf(stderr, "cannot write %s\n", dthist_path.c_str());
            return 1;
        }
        std::printf("wrote %s\n", dthist_path.c_str());
    }

    auto run_roundtrip = [&](const std::vector<ingest::Event>& evs,
                             std::string& why) {
        ingest::detail::Fingerprint fp_ev, fp_raw;
        lob::Book fresh, raw;
        std::string e2;
        if (!ingest::replay_events(evs, fresh, true, fp_ev, &e2)) {
            why = "layer1 replay: " + e2;
            return false;
        }
        if (!ingest::replay_raw_symbol(wire.data(), wire.size(), locate, raw,
                                       fp_raw, &e2)) {
            why = "layer1 raw replay: " + e2;
            return false;
        }
        if (fp_ev.folds != fp_raw.folds) {
            why = "layer1 fold-count mismatch";
            return false;
        }
        if (fp_ev.h != fp_raw.h) {
            why = "layer1 fingerprint mismatch";
            return false;
        }
        return true;
    };

    if (roundtrip) {
        std::string why;
        bool l1 = run_roundtrip(res.events, why);
        std::printf("round-trip layer 1 (exact stream -> fresh book == raw "
                    "ITCH book, %" PRIu64 " state folds): %s%s%s\n",
                    res.msgs_symbol, l1 ? "PASS" : "FAIL",
                    l1 ? "" : " - ", l1 ? "" : why.c_str());
        bool l2 = true;
        if (have_bins) {
            std::string e2;
            l2 = ingest::verify_tokens(res, bins, &e2);
            std::printf("round-trip layer 2 (tokens <-> stream inverse, %"
                        PRIu64 " events): %s%s%s\n",
                        res.events_inwindow, l2 ? "PASS" : "FAIL",
                        l2 ? "" : " - ", l2 ? "" : e2.c_str());
        } else {
            std::printf("round-trip layer 2 skipped (no --manifest)\n");
        }
        if (!l1 || !l2) return 1;
    }

    if (!mutate.empty()) {
        if (mutate == "swap") {
            std::vector<ingest::Event> mut = res.events;
            size_t at = mut.size();
            for (size_t i = 0; i + 1 < mut.size(); ++i) {
                if (!mut[i].pair_first && !mut[i + 1].pair_first &&
                    mut[i].price != mut[i + 1].price) {
                    std::swap(mut[i], mut[i + 1]);
                    at = i;
                    break;
                }
            }
            if (at == mut.size()) {
                std::fprintf(stderr, "no eligible adjacent pair to swap\n");
                return 1;
            }
            std::string why;
            bool pass = run_roundtrip(mut, why);
            std::printf("mutation swap@%zu: %s\n", at,
                        pass ? "NOT CAUGHT - TEST TOO WEAK"
                             : ("caught (" + why + ")").c_str());
            if (pass) return 1;
        } else if (mutate == "pxoff") {
            if (!have_bins) {
                std::fprintf(stderr, "--mutate pxoff needs --manifest\n");
                return 2;
            }
            // (a) event-side: lvl_off off by one -> recompute must catch.
            ingest::IngestResult mut = res;
            size_t at = SIZE_MAX, tok_at = SIZE_MAX, k = 0;
            for (size_t i = 0; i < mut.events.size(); ++i) {
                const ingest::Event& e = mut.events[i];
                if (e.in_win && e.has_px && e.lvl_off >= 0 &&
                    e.lvl_off < oftk::kPxMax) {
                    at = i;
                    tok_at = k;
                    break;
                }
                if (e.in_win) ++k;
            }
            if (at == SIZE_MAX) {
                std::fprintf(stderr, "no eligible event for pxoff\n");
                return 1;
            }
            mut.events[at].lvl_off += 1;
            std::string why;
            ingest::detail::Fingerprint fp;
            lob::Book fresh;
            bool caught_event =
                !ingest::replay_events(mut.events, fresh, true, fp, &why);
            std::printf("mutation pxoff(event)@%zu: %s\n", at,
                        caught_event ? ("caught (" + why + ")").c_str()
                                     : "NOT CAUGHT - TEST TOO WEAK");
            // (b) token-side: px slot off by one -> layer 2 must catch.
            ingest::IngestResult mut2 = res;
            mut2.tokens[2 + tok_at * oftk::kEventTokens + 2] += 1;
            std::string e2;
            bool caught_tok = !ingest::verify_tokens(mut2, bins, &e2);
            std::printf("mutation pxoff(token)@%zu: %s\n", tok_at,
                        caught_tok ? ("caught (" + e2 + ")").c_str()
                                   : "NOT CAUGHT - TEST TOO WEAK");
            if (!caught_event || !caught_tok) return 1;
        } else if (mutate == "size") {
            // Bug model: the ingest records a wrong SIZE for a Delete
            // (b.remove ignores it, so only the strict find-check can
            // catch it - the review's blind spot, now closed).
            std::vector<ingest::Event> mut = res.events;
            size_t at = mut.size();
            for (size_t i = 0; i < mut.size(); ++i)
                if (mut[i].type == oftk::MsgType::Delete) {
                    mut[i].size += 1;
                    at = i;
                    break;
                }
            if (at == mut.size()) {
                std::fprintf(stderr, "no Delete event to mutate\n");
                return 1;
            }
            std::string why;
            ingest::detail::Fingerprint fp;
            lob::Book fresh;
            bool caught =
                !ingest::replay_events(mut, fresh, true, fp, &why);
            std::printf("mutation size(delete)@%zu: %s\n", at,
                        caught ? ("caught (" + why + ")").c_str()
                               : "NOT CAUGHT - TEST TOO WEAK");
            if (!caught) return 1;
        } else if (mutate == "dt") {
            // Bug model: dt assigned wrongly to one in-window event.
            std::vector<ingest::Event> mut = res.events;
            size_t at = mut.size();
            for (size_t i = 0; i < mut.size(); ++i)
                if (mut[i].in_win) {
                    mut[i].dt_ns += 1;
                    at = i;
                    break;
                }
            if (at == mut.size()) {
                std::fprintf(stderr, "no in-window event to mutate\n");
                return 1;
            }
            std::string why;
            ingest::detail::Fingerprint fp;
            lob::Book fresh;
            bool caught =
                !ingest::replay_events(mut, fresh, true, fp, &why);
            std::printf("mutation dt@%zu: %s\n", at,
                        caught ? ("caught (" + why + ")").c_str()
                               : "NOT CAUGHT - TEST TOO WEAK");
            if (!caught) return 1;
        } else {
            std::fprintf(stderr, "unknown mutation %s\n", mutate.c_str());
            return 2;
        }
    }

    if (!out_path.empty()) {
        if (!have_bins) {
            std::fprintf(stderr, "-o needs --manifest bins\n");
            return 2;
        }
        std::ofstream out(out_path, std::ios::binary);
        bool ok = out && oftk::write_token_bin(out, ticker, res.tokens);
        out.flush();  // surface buffered-tail errors the destructor swallows
        if (!ok || !out) {
            std::fprintf(stderr, "cannot write %s\n", out_path.c_str());
            return 1;
        }
        std::printf("wrote %s (%zu tokens)\n", out_path.c_str(),
                    res.tokens.size());
    }
    return 0;
}
