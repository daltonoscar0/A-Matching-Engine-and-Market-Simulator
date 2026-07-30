// Randomized message-sequence fuzzer.
//
// Drives a fresh Book (separate from the generator's shadow) through the
// full pipeline: generate -> encode -> decode -> apply. After EVERY message
// it asserts the four Phase-1 invariants:
//   (a) bids never cross asks              - O(1) via invariants_fast()
//   (b) FIFO within a price level          - deep audit walks seq numbers
//   (c) share conservation                 - O(1) via invariants_fast()
//   (d) unknown-id cancel/replace rejected - expect_ok bookkeeping
// The O(n) structural audit (which fully re-verifies (a)-(c) plus list
// integrity) runs every AUDIT_EVERY messages and at the end.
//
// Message count defaults to 200k for ctest; set FUZZ_N=1000000 for the full
// session gate. Uses raw asserts in the hot loop (Catch2 CHECK costs ~1us
// per call, which would dominate a 1M-message run).
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../src/book.hpp"
#include "../src/bookset.hpp"
#include "../src/feed.hpp"
#include "../src/itch.hpp"
#include "../src/synth.hpp"
#include "../third_party/catch.hpp"

namespace {

constexpr size_t AUDIT_EVERY = 4096;

void run_fuzz(uint64_t seed, size_t n, uint32_t invalid_permille) {
    synth::Generator gen({.seed = seed, .invalid_permille = invalid_permille});
    lob::Book book;
    uint8_t buf[itch::MAX_MSG_LEN];
    size_t rejects = 0;

    for (size_t i = 0; i < n; ++i) {
        bool expect_ok;
        itch::Message m = gen.next(expect_ok);

        // Full wire round trip on every message.
        size_t len = itch::encode(m, buf);
        auto d = itch::decode(buf, len);
        if (!d) { FAIL("decode failed at msg " << i); }

        lob::Result r = lob::apply(book, *d);
        if (expect_ok && r != lob::Result::Ok)
            FAIL("valid msg rejected at " << i << ": " << lob::to_string(r));
        if (!expect_ok) {
            if (r == lob::Result::Ok)
                FAIL("invalid msg accepted at " << i);
            ++rejects;
        }

        // Invariants (a) + (c) after every single message, O(1).
        if (!book.invariants_fast())
            FAIL("fast invariant broken at msg " << i << ": " << book.audit());

        // Deep audit, including FIFO-by-seq within every level, periodically.
        if (i % AUDIT_EVERY == 0) {
            std::string err = book.audit();
            if (!err.empty()) FAIL("audit failed at msg " << i << ": " << err);
        }
    }

    std::string err = book.audit();
    if (!err.empty()) FAIL("final audit failed: " << err);

    // The fuzz book and the generator's shadow book evolved independently
    // from the same message stream; they must agree exactly.
    const lob::Book& shadow = gen.shadow();
    CHECK(book.open_orders() == shadow.open_orders());
    CHECK(book.best_bid() == shadow.best_bid());
    CHECK(book.best_ask() == shadow.best_ask());
    CHECK(book.shares_added() == shadow.shares_added());
    CHECK(book.shares_executed() == shadow.shares_executed());
    CHECK(book.shares_resting() == shadow.shares_resting());

    if (invalid_permille > 0) CHECK(rejects > 0);
    INFO("msgs=" << n << " rejects=" << rejects
                 << " open=" << book.open_orders());
}

// Same drill as run_fuzz, but the stream interleaves n_symbols instruments
// and the unit under test is a BookSet routing on stock_locate. Invariants
// (a)-(d) are asserted per book; refs are globally unique across symbols, so
// a misrouted message surfaces as a wrong accept/reject (invariant (d)).
void run_fuzz_multi(uint64_t seed, size_t n, uint32_t invalid_permille,
                    uint16_t n_symbols) {
    synth::MultiGenerator gen(
        {.seed = seed, .invalid_permille = invalid_permille,
         .n_symbols = n_symbols});
    lob::BookSet set;
    uint8_t buf[itch::MAX_MSG_LEN];
    size_t rejects = 0;

    for (size_t i = 0; i < n; ++i) {
        bool expect_ok;
        itch::Message m = gen.next(expect_ok);

        size_t len = itch::encode(m, buf);
        auto d = itch::decode(buf, len);
        if (!d) { FAIL("decode failed at msg " << i); }

        uint16_t loc = lob::locate_of(*d);
        lob::Result r = set.apply(*d);
        if (expect_ok && r != lob::Result::Ok)
            FAIL("valid msg rejected at " << i << " (locate " << loc
                 << "): " << lob::to_string(r));
        if (!expect_ok) {
            if (r == lob::Result::Ok)
                FAIL("invalid msg accepted at " << i << " (locate " << loc
                     << ")");
            ++rejects;
        }

        if (!set.book(loc).invariants_fast())
            FAIL("fast invariant broken at msg " << i << " in book " << loc
                 << ": " << set.book(loc).audit());

        if (i % AUDIT_EVERY == 0) {
            set.for_each_book([&](uint16_t l, lob::Book& b) {
                std::string err = b.audit();
                if (!err.empty())
                    FAIL("audit failed at msg " << i << " in book " << l
                         << ": " << err);
            });
        }
    }

    // Every book must match the corresponding generator's shadow exactly.
    for (size_t s = 0; s < gen.n_symbols(); ++s) {
        const lob::Book& shadow = gen.sub(s).shadow();
        lob::Book& b = set.book(gen.locate(s));
        std::string err = b.audit();
        if (!err.empty()) FAIL("final audit, book " << gen.locate(s) << ": " << err);
        CHECK(b.open_orders() == shadow.open_orders());
        CHECK(b.best_bid() == shadow.best_bid());
        CHECK(b.best_ask() == shadow.best_ask());
        CHECK(b.shares_added() == shadow.shares_added());
        CHECK(b.shares_executed() == shadow.shares_executed());
        CHECK(b.shares_resting() == shadow.shares_resting());
    }
    CHECK(set.book_count() == gen.n_symbols());
    if (invalid_permille > 0) CHECK(rejects > 0);
}

size_t fuzz_n() {
    if (const char* s = std::getenv("FUZZ_N")) return strtoull(s, nullptr, 10);
    return 200'000;
}

}  // namespace

TEST_CASE("fuzz: valid-only stream, invariants after every message") {
    run_fuzz(/*seed=*/1, fuzz_n(), /*invalid_permille=*/0);
}

TEST_CASE("fuzz: 5% hostile messages are rejected without state damage") {
    run_fuzz(/*seed=*/2, fuzz_n(), /*invalid_permille=*/50);
}

TEST_CASE("fuzz: multiple seeds, smaller runs") {
    for (uint64_t seed = 10; seed < 18; ++seed)
        run_fuzz(seed, 20'000, /*invalid_permille=*/100);
}

TEST_CASE("fuzz: interleaved symbols, per-book invariants via BookSet") {
    run_fuzz_multi(/*seed=*/3, fuzz_n(), /*invalid_permille=*/50,
                   /*n_symbols=*/8);
}
