// The train/val/test split guard (src/dataset.hpp) is enforcement, not
// advice: these tests pin the allocation invariants and assert the guard
// actually fires on TEST days. If a test here fails because the table
// changed, that change is a Decision to log in PLAN.md, not a tweak.
#include "../third_party/catch.hpp"

#include <cstring>
#include <set>
#include <string>

#include "../src/dataset.hpp"

using dataset::Access;
using dataset::Split;

TEST_CASE("split table covers all 9 days exactly once", "[dataset]") {
    REQUIRE(dataset::kNumDays == 9);
    std::set<std::string> seen;
    for (const auto& d : dataset::kDays) {
        REQUIRE(std::strlen(d.day) == 8);
        REQUIRE(seen.insert(d.day).second);   // no duplicates
    }
}

TEST_CASE("split constraints: TEST>=2 incl the high-vol day, VAL>=1",
          "[dataset]") {
    size_t n_train = 0, n_val = 0, n_test = 0;
    bool high_vol_in_test = false;
    for (const auto& d : dataset::kDays) {
        switch (d.split) {
            case Split::Train: ++n_train; break;
            case Split::Val:   ++n_val;   break;
            case Split::Test:  ++n_test;  break;
        }
        if (!std::strcmp(d.day, "20181228"))
            high_vol_in_test = d.split == Split::Test;
    }
    REQUIRE(n_test >= 2);
    REQUIRE(high_vol_in_test);   // regime transfer is untestable without it
    REQUIRE(n_val >= 1);
    REQUIRE(n_train >= 1);
    REQUIRE(n_train + n_val + n_test == dataset::kNumDays);
}

TEST_CASE("guard fires on TEST days and only the override opens them",
          "[dataset]") {
    Access a = dataset::classify("data/20181228.BX_ITCH_50.gz");
    REQUIRE(a == Access::TestBlocked);
    REQUIRE_FALSE(dataset::allowed(a, false));
    REQUIRE(dataset::allowed(a, true));
    // decompressed name blocks the same way
    REQUIRE(dataset::classify("data/20181228.BX_ITCH_50") ==
            Access::TestBlocked);
    REQUIRE(dataset::classify("/abs/path/data/20200130.BX_ITCH_50.gz") ==
            Access::TestBlocked);
}

TEST_CASE("TRAIN and VAL days pass without any flag", "[dataset]") {
    REQUIRE(dataset::classify("data/20190130.BX_ITCH_50.gz") ==
            Access::OkTrain);
    const char* day = nullptr;
    Access v = dataset::classify("data/20190730.BX_ITCH_50", &day);
    REQUIRE(v == Access::OkVal);
    REQUIRE(std::string(day) == "20190730");
    REQUIRE(dataset::allowed(v, false));
}

TEST_CASE("unassigned days are refused even with the override", "[dataset]") {
    Access a = dataset::classify("data/20250101.BX_ITCH_50.gz");
    REQUIRE(a == Access::UnknownDay);
    REQUIRE_FALSE(dataset::allowed(a, false));
    REQUIRE_FALSE(dataset::allowed(a, true));   // override is not a bypass
}

TEST_CASE("non-day files (synthetic captures) pass through", "[dataset]") {
    Access a = dataset::classify("synth_5m_seed42.itch");
    REQUIRE(a == Access::OkNotADay);
    REQUIRE(dataset::allowed(a, false));
}
