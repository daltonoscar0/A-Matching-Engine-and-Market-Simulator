// Replay a bounded slice of a real NASDAQ BX ITCH 5.0 day through the
// BookSet and hold the Phase 1 contract: genuine exchange data produces
// ZERO rejects, zero invariant violations, and a clean audit. The data file
// is 800MB and not checked in; the test skips gracefully when it is absent.
#include <cstdio>
#include <vector>

#include "../src/itch_replay.hpp"
#include "../third_party/catch.hpp"

namespace {

// ctest runs in build/, a manual `./tests` may run from the repo root.
const char* find_data_file() {
    static const char* candidates[] = {
        "data/20190730.BX_ITCH_50",
        "../data/20190730.BX_ITCH_50",
    };
    for (const char* p : candidates) {
        if (FILE* f = std::fopen(p, "rb")) { std::fclose(f); return p; }
    }
    return nullptr;
}

std::vector<uint8_t> read_prefix(const char* path, size_t max_bytes) {
    FILE* f = std::fopen(path, "rb");
    REQUIRE(f != nullptr);
    std::vector<uint8_t> buf(max_bytes);
    size_t n = std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    buf.resize(n);
    return buf;
}

}  // namespace

TEST_CASE("itch replay: real BX slice, zero rejects, invariants clean") {
    const char* path = find_data_file();
    if (!path) {
        WARN("data/20190730.BX_ITCH_50 not present; skipping real-data "
             "replay test");
        return;
    }

    // 64MB prefix holds well over 300k book messages (measured); capping at
    // 200k decoded messages keeps this inside ~1s for ctest and guarantees
    // we stop before the buffer's truncated final frame.
    constexpr size_t SLICE_BYTES = 64u << 20;
    constexpr size_t MAX_MSGS    = 200'000;
    std::vector<uint8_t> wire = read_prefix(path, SLICE_BYTES);

    // The slice's own 'R' frames are the locate map; sanity-check it exists
    // and never claims locate 0 (reserved for system-wide messages).
    {
        // The directory scan walks the full slice, whose last frame is
        // truncated by the byte cut - that framing error is expected here
        // and is NOT the malformed-stream signal the replay asserts on.
        bool framing_error = false;
        auto dir = lob::scan_stock_directory(wire.data(), wire.size(),
                                             framing_error);
        CHECK(dir.size() > 100);
        for (const auto& d : dir) CHECK(d.h.stock_locate != 0);
    }

    lob::BookSet set;
    lob::ReplayOptions opt;
    opt.max_messages     = MAX_MSGS;
    opt.check_invariants = true;
    auto out = lob::replay_itch(wire.data(), wire.size(), set, opt);
    const lob::ReplayStats& st = out.stats;

    INFO("decoded=" << st.decoded << " applied=" << st.applied
         << " skipped=" << st.skipped << " books=" << st.books_touched);

    // The cap must be what stopped us - otherwise the slice was too small
    // and we ran into the truncated tail (stream_error).
    REQUIRE(st.decoded == MAX_MSGS);
    REQUIRE(!st.stream_error);

    // The Phase 1 contract on real data.
    CHECK(st.rejected == 0);
    for (const auto& c : out.reject_samples)
        FAIL_CHECK("reject: " << lob::to_string(c.result) << " on "
                   << lob::describe(c.msg));
    CHECK(st.invariant_failure.empty());
    CHECK(st.applied == st.decoded);

    bool audits_clean = true;
    uint64_t added = 0, executed = 0, canceled = 0, resting = 0;
    set.for_each_book([&](uint16_t loc, lob::Book& b) {
        std::string err = b.audit();
        if (!err.empty()) {
            audits_clean = false;
            FAIL_CHECK("audit failed for book " << loc << ": " << err);
        }
        added    += b.shares_added();
        executed += b.shares_executed();
        canceled += b.shares_canceled();
        resting  += b.shares_resting();
    });
    CHECK(audits_clean);
    CHECK(added == executed + canceled + resting);
    CHECK(st.books_touched > 0);
}
