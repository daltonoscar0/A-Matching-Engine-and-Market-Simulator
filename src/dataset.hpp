// dataset - the mechanical TRAIN / VAL / TEST allocation of the real BX ITCH
// days in data/. This table is the single source of truth for which day may
// be read by which stage of the project; the reasoning is logged in PLAN.md
// Decisions (2026-07-30, dataset split).
//
// Why this exists: the 9 days are BOTH the orderflow-lm training set AND the
// source of the Phase 3 "real" column. Without a held-out set, Phase 3 would
// ask whether the model reproduces statistics of data it memorised. The
// split therefore has to be decided once, mechanically, before any training
// - not implicitly by whatever is convenient later.
//
// Rules enforced by enforce() (and asserted by tests/test_dataset.cpp):
//   - TEST days are refused unless the caller passes the override flag
//     --i-am-running-the-final-comparison. TEST is not to be looked at -
//     not for baselines, not for debugging, not for "just checking" - until
//     the headline real-vs-sim comparison runs once.
//   - A file whose name carries an 8-digit day NOT in this table is refused
//     outright: a new day must be assigned here before anything reads it.
//   - Files with no 8-digit day token (synthetic captures) are not dataset
//     days and pass through.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dataset {

enum class Split { Train, Val, Test };

struct DayAssignment {
    const char* day;   // YYYYMMDD as it appears in the filename
    Split split;
};

// All 9 days on disk, each assigned to exactly one split.
inline constexpr DayAssignment kDays[] = {
    {"20181228", Split::Test},   // Q4-2018 selloff, 4.6x volume: the
                                 // regime-transfer day, held out
    {"20190130", Split::Train},
    {"20190327", Split::Train},
    {"20190530", Split::Train},
    {"20190730", Split::Val},    // the methodology-development day: every
                                 // published number and tuning decision so
                                 // far used it, so it can never be TEST;
                                 // VAL formalizes its role as the tuning day
    {"20190830", Split::Train},
    {"20191030", Split::Train},
    {"20191230", Split::Train},
    {"20200130", Split::Test},   // most recent day: forward-in-time transfer
};
inline constexpr size_t kNumDays = sizeof(kDays) / sizeof(kDays[0]);

inline const char* to_string(Split s) {
    switch (s) {
        case Split::Train: return "TRAIN";
        case Split::Val:   return "VAL";
        case Split::Test:  return "TEST";
    }
    return "?";
}

enum class Access {
    OkTrain,      // TRAIN day
    OkVal,        // VAL day
    OkNotADay,    // no 8-digit day token in the filename (synthetic etc.)
    TestBlocked,  // TEST day; requires the override flag
    UnknownDay,   // 8-digit day token not in kDays; must be assigned first
};

// Pure classification of a path, separated from enforce() so the guard's
// decision logic is unit-testable without spawning processes.
inline Access classify(const char* path, const char** day_out = nullptr) {
    if (day_out) *day_out = nullptr;
    const char* base = path;
    for (const char* p = path; *p; ++p)
        if (*p == '/') base = p + 1;
    // find an 8-digit run in the basename
    const char* tok = nullptr;
    for (const char* p = base; *p; ++p) {
        size_t n = 0;
        while (p[n] >= '0' && p[n] <= '9') ++n;
        if (n == 8) { tok = p; break; }
        if (n) p += n - 1;
    }
    if (!tok) return Access::OkNotADay;
    for (const auto& d : kDays) {
        if (std::strncmp(tok, d.day, 8) == 0) {
            if (day_out) *day_out = d.day;
            switch (d.split) {
                case Split::Train: return Access::OkTrain;
                case Split::Val:   return Access::OkVal;
                case Split::Test:  return Access::TestBlocked;
            }
        }
    }
    return Access::UnknownDay;
}

inline bool allowed(Access a, bool final_comparison_override) {
    switch (a) {
        case Access::OkTrain:
        case Access::OkVal:
        case Access::OkNotADay:   return true;
        case Access::TestBlocked: return final_comparison_override;
        case Access::UnknownDay:  return false;
    }
    return false;
}

inline constexpr const char* kOverrideFlag = "--i-am-running-the-final-comparison";

// Call before reading a day file. Prints the split so every tool run records
// which split it touched; exits 3 on a violation.
inline void enforce(const char* path, bool final_comparison_override) {
    const char* day = nullptr;
    Access a = classify(path, &day);
    if (allowed(a, final_comparison_override)) {
        switch (a) {
            case Access::OkTrain:
                std::printf("dataset split: %s = TRAIN\n", day); break;
            case Access::OkVal:
                std::printf("dataset split: %s = VAL\n", day); break;
            case Access::TestBlocked:
                std::printf("dataset split: %s = TEST (override: final "
                            "comparison)\n", day);
                break;
            case Access::OkNotADay:
            case Access::UnknownDay: break;
        }
        return;
    }
    if (a == Access::UnknownDay) {
        std::fprintf(stderr,
            "REFUSED: %s carries a day not in the split table "
            "(src/dataset.hpp).\nAssign it to TRAIN/VAL/TEST and log the "
            "decision in PLAN.md before reading it.\n", path);
    } else {
        std::fprintf(stderr,
            "REFUSED: %s is a TEST day.\nTEST days are held out until the "
            "final real-vs-sim comparison (PLAN.md Decisions 2026-07-30) - "
            "not for baselines,\nnot for debugging, not for \"just "
            "checking\". If this IS the final comparison, pass %s\n",
            path, kOverrideFlag);
    }
    std::exit(3);
}

}  // namespace dataset
