// warm_book: produce the WARM-START SNAPSHOT for one symbol - the real
// resting book at 09:30, the instant the measurement window opens.
//
// Motivation and the exact failure this repairs are in src/warm_book.hpp.
// In one line: the shim's PRICE_OFF is an occupied-level INDEX, so a
// two-order seed can resolve only indices 0 and -1, and the 2026-07-31
// cold-start control showed the REAL token stream dies on that seed within
// 3 tuples. Generation must start from a real book.
//
// This is deliberately a SEPARATE tool from sim_health/shim_drive rather
// than a mode inside them (implementation call, logged in PLAN.md): the
// VAL sampling sweep runs sim_health dozens of times, and re-replaying a
// multi-GB ITCH day per run is untenable - the raw TRAIN days are gzipped
// and the machine has ~2GB of headroom. The snapshot is a small,
// inspectable, reproducible artifact; sim_health/shim_drive just load it.
//
// It writes NO new reconstruction: ingest::ingest_day produces the same
// expanded event stream the tokenizer consumes, and ingest::replay_events
// (strict) drives it into a fresh Book with the same per-event validation
// the round-trip test uses. Only events with ts < the cutoff are replayed,
// so this reads pre-open flow ONLY - it never touches the 09:30-16:00
// window that the tokens, the stylized facts, or the scoring see.
//
// Usage:
//   warm_book <day-file> --ticker T -o snapshot.book [--at-ns NS]
// dataset::enforce() runs first: a sealed TEST day is refused.
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "../src/dataset.hpp"
#include "../src/itch_tokenize.hpp"
#include "../src/warm_book.hpp"

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

bool find_locate(const uint8_t* data, size_t size, const std::string& ticker,
                 uint16_t& out) {
    bool framing_error = false;
    auto dir = lob::scan_stock_directory(data, size, framing_error);
    if (framing_error) return false;
    for (const auto& d : dir)
        if (trim_stock(d.stock) == ticker) {
            out = d.h.stock_locate;
            return true;
        }
    return false;
}

// Same hazard the ingest tools guard (2026-07-30 review): -o TRUNCATES its
// target and enforce() only guards the day argument.
void require_safe_aux(const char* what, const std::string& p) {
    if (p.empty()) return;
    if (p.find(".BX_ITCH_50") != std::string::npos) {
        std::fprintf(stderr, "%s path %s names a raw data file - refused\n",
                     what, p.c_str());
        std::exit(3);
    }
    if (dataset::classify(p.c_str()) == dataset::Access::TestBlocked) {
        std::fprintf(stderr, "%s path %s names a sealed TEST day - refused\n",
                     what, p.c_str());
        std::exit(3);
    }
}

std::string day_of(const std::string& path) {
    size_t slash = path.find_last_of('/');
    std::string base = slash == std::string::npos ? path
                                                  : path.substr(slash + 1);
    size_t dot = base.find('.');
    return dot == std::string::npos ? base : base.substr(0, dot);
}

}  // namespace

int main(int argc, char** argv) {
    const char* path = nullptr;
    std::string ticker, out_path;
    uint64_t at_ns = ingest::kOpenNs;  // 09:30:00, the window open
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
        else if (a == "-o") out_path = next("-o");
        else if (a == "--at-ns")
            at_ns = std::strtoull(next("--at-ns"), nullptr, 10);
        else if (!path) path = argv[i];
        else {
            std::fprintf(stderr, "unexpected argument %s\n", argv[i]);
            return 2;
        }
    }
    if (!path || ticker.empty() || out_path.empty()) {
        std::fprintf(stderr, "usage: warm_book <day-file> --ticker T "
                             "-o snapshot.book [--at-ns NS]\n");
        return 2;
    }
    // No override flag is accepted here at all: a warm start has no business
    // reading a sealed day, so the guard cannot be talked out of it.
    dataset::enforce(path, false);
    require_safe_aux("-o", out_path);

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

    ingest::IngestResult res;
    std::string err;
    if (!ingest::ingest_day(wire.data(), wire.size(), locate, nullptr, res,
                            &err)) {
        std::fprintf(stderr, "ingest FAILED: %s\n", err.c_str());
        return 1;
    }
    std::vector<ingest::Event> pre;
    for (const ingest::Event& e : res.events) {
        if (e.ts >= at_ns) break;
        pre.push_back(e);
    }

    lob::Book b;
    ingest::detail::Fingerprint fp;
    if (!ingest::replay_events(pre, b, /*strict=*/true, fp, &err)) {
        std::fprintf(stderr, "pre-open replay FAILED: %s\n", err.c_str());
        return 1;
    }
    if (!b.audit().empty() || !b.invariants_fast()) {
        std::fprintf(stderr, "warm book fails audit: %s\n",
                     b.audit().c_str());
        return 1;
    }

    std::ofstream os(out_path);
    if (!os || !warm::write(os, ticker, day_of(path), at_ns, b, &err)) {
        std::fprintf(stderr, "cannot write %s: %s\n", out_path.c_str(),
                     err.c_str());
        return 1;
    }
    os.close();
    if (!os) {
        std::fprintf(stderr, "cannot write %s\n", out_path.c_str());
        return 1;
    }

    std::printf("%s %s: %zu pre-%" PRIu64 " events of %zu -> %zu resting "
                "orders, %zu bid / %zu ask levels, best %u/%u\n",
                ticker.c_str(), day_of(path).c_str(), pre.size(), at_ns,
                res.events.size(), b.open_orders(), b.bid_levels(),
                b.ask_levels(), b.best_bid(), b.best_ask());
    return 0;
}
