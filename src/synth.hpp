// synth.hpp - synthetic ITCH message stream with realistic type ratios.
//
// Stand-in for the LOBSTER sample (not available in this environment; see
// PLAN.md). Type mix loosely follows observed TotalView traffic for a liquid
// name: adds ~40% (1 in 10 of them 'F'), deletes ~33%, replaces ~12%,
// executes ~7% (1 in 5 of them 'C'), partial cancels ~8%. Optionally injects
// deliberately invalid messages (unknown ids, crossing adds, duplicate ids,
// oversize cancels, zero-share adds) at `invalid_permille` for fuzzing.
//
// The generator owns a shadow Book, so every message it labels valid is
// valid by construction; if the shadow ever rejects one, that is a generator
// bug and we abort.
#pragma once
#include <cstdint>
#include <cstdlib>
#include <random>
#include <vector>

#include "book.hpp"
#include "feed.hpp"
#include "itch.hpp"

namespace synth {

struct Config {
    uint64_t seed             = 42;
    uint32_t invalid_permille = 0;         // 0..1000
    uint32_t mid0             = 1'000'000; // $100.0000, ITCH fixed point
    uint32_t tick             = 100;       // one cent
    uint32_t depth_ticks      = 20;
    // Multi-instrument fields. Defaults keep single-symbol streams
    // byte-identical to before these fields existed.
    uint16_t locate           = 1;
    uint64_t ref_base         = 0;         // order refs start here (+1, +2, ...)
    itch::Stock stock         = {'S','Y','N','T','H',' ',' ',' '};
};

class Generator {
public:
    explicit Generator(Config cfg)
        : cfg_(cfg), rng_(cfg.seed), next_ref_(cfg.ref_base) {}

    // Next message; expect_ok := a correct book must accept it.
    itch::Message next(bool& expect_ok) {
        bool bad = cfg_.invalid_permille > 0 && pct_(rng_) < cfg_.invalid_permille;
        itch::Message m = bad ? next_invalid() : next_valid();
        expect_ok = !bad;
        if (expect_ok) apply_to_shadow(m);
        return m;
    }

    const lob::Book& shadow() const { return shadow_; }

private:
    static constexpr uint64_t kUnknownBase = 0xDEAD000000000000ULL;

    itch::Header hdr() {
        itch::Header h;
        h.stock_locate = cfg_.locate;
        h.timestamp    = ts_ += 1 + rng_() % 5000;
        return h;
    }

    itch::Stock stock() const { return cfg_.stock; }

    uint64_t pick_live() { return live_[rng_() % live_.size()]; }

    void forget(uint64_t ref) {
        for (size_t i = 0; i < live_.size(); ++i)
            if (live_[i] == ref) {
                live_[i] = live_.back();
                live_.pop_back();
                return;
            }
    }

    // A price on `side` that cannot cross the shadow book.
    uint32_t safe_price(lob::Side side) {
        uint32_t k = 1 + rng_() % cfg_.depth_ticks;
        if (side == lob::Side::Buy) {
            uint32_t ba   = shadow_.best_ask();
            uint32_t base = ba ? ba : cfg_.mid0 + cfg_.tick;
            uint32_t p    = base > k * cfg_.tick ? base - k * cfg_.tick : cfg_.tick;
            return p;                                   // p <= ba - tick < ba
        }
        uint32_t bb   = shadow_.best_bid();
        uint32_t base = bb ? bb : cfg_.mid0 - cfg_.tick;
        return base + k * cfg_.tick;                    // p >= bb + tick > bb
    }

    itch::Message make_add(lob::Side s, uint64_t ref, uint32_t shares,
                           uint32_t price, bool mpid) {
        if (mpid) {
            itch::AddOrderMpid f;
            f.add.h = hdr(); f.add.order_ref = ref;
            f.add.side = s == lob::Side::Buy ? 'B' : 'S';
            f.add.shares = shares; f.add.price = price; f.add.stock = stock();
            f.attribution = {'C','L','D','E'};
            return f;
        }
        itch::AddOrder a;
        a.h = hdr(); a.order_ref = ref;
        a.side = s == lob::Side::Buy ? 'B' : 'S';
        a.shares = shares; a.price = price; a.stock = stock();
        return a;
    }

    itch::Message next_valid() {
        uint32_t roll = live_.size() < 8 ? 0 : pct_(rng_);  // seed population
        if (roll < 400) {                                   // adds
            lob::Side s = (rng_() & 1) ? lob::Side::Buy : lob::Side::Sell;
            return make_add(s, ++next_ref_, 100 * (1 + rng_() % 10),
                            safe_price(s), rng_() % 10 == 0);
        }
        if (roll < 730) {                                   // deletes
            itch::OrderDelete d; d.h = hdr(); d.order_ref = pick_live();
            return d;
        }
        if (roll < 850) {                                   // replaces
            uint64_t orig = pick_live();
            const lob::Order* o = shadow_.find(orig);
            itch::OrderReplace u;
            u.h = hdr(); u.orig_order_ref = orig; u.new_order_ref = ++next_ref_;
            u.shares = 100 * (1 + rng_() % 10);
            u.price  = safe_price(o->side);
            return u;
        }
        if (roll < 920) {                                   // executes
            uint64_t ref = pick_live();
            const lob::Order* o = shadow_.find(ref);
            uint32_t sh = 1 + rng_() % o->shares;
            if (rng_() % 5 == 0) {
                itch::OrderExecutedPrice c;
                c.exec.h = hdr(); c.exec.order_ref = ref; c.exec.shares = sh;
                c.exec.match_num = ++match_; c.printable = 'N';
                c.price = o->price;
                return c;
            }
            itch::OrderExecuted e;
            e.h = hdr(); e.order_ref = ref; e.shares = sh; e.match_num = ++match_;
            return e;
        }
        uint64_t ref = pick_live();                         // partial cancels
        const lob::Order* o = shadow_.find(ref);
        itch::OrderCancel x;
        x.h = hdr(); x.order_ref = ref; x.shares = 1 + rng_() % o->shares;
        return x;
    }

    itch::Message next_invalid() {
        switch (rng_() % 5) {
            case 0: {                        // unknown-id delete
                itch::OrderDelete d; d.h = hdr();
                d.order_ref = kUnknownBase + rng_() % 1000;
                return d;
            }
            case 1: {                        // unknown-id execute
                itch::OrderExecuted e; e.h = hdr();
                e.order_ref = kUnknownBase + rng_() % 1000;
                e.shares = 100; e.match_num = ++match_;
                return e;
            }
            case 2: {                        // crossing add
                if (uint32_t ba = shadow_.best_ask())
                    return make_add(lob::Side::Buy, ++next_ref_, 100, ba, false);
                if (uint32_t bb = shadow_.best_bid())
                    return make_add(lob::Side::Sell, ++next_ref_, 100, bb, false);
                return make_add(lob::Side::Buy, ++next_ref_, 0,      // zero-share
                                cfg_.mid0, false);
            }
            case 3: {                        // duplicate-id add
                if (!live_.empty()) {
                    lob::Side s = (rng_() & 1) ? lob::Side::Buy : lob::Side::Sell;
                    return make_add(s, live_[rng_() % live_.size()], 100,
                                    safe_price(s), false);
                }
                return make_add(lob::Side::Sell, ++next_ref_, 0,     // zero-share
                                cfg_.mid0, false);
            }
            default: {                       // oversize cancel
                if (!live_.empty()) {
                    uint64_t ref = pick_live();
                    const lob::Order* o = shadow_.find(ref);
                    itch::OrderCancel x; x.h = hdr(); x.order_ref = ref;
                    x.shares = o->shares + 1 + rng_() % 100;
                    return x;
                }
                itch::OrderCancel x; x.h = hdr();
                x.order_ref = kUnknownBase; x.shares = 100;
                return x;
            }
        }
    }

    void apply_to_shadow(const itch::Message& m) {
        if (lob::apply(shadow_, m) != lob::Result::Ok) std::abort();
        if (auto* d = std::get_if<itch::OrderDelete>(&m)) {
            forget(d->order_ref);
        } else if (auto* u = std::get_if<itch::OrderReplace>(&m)) {
            forget(u->orig_order_ref);
            live_.push_back(u->new_order_ref);
        } else if (auto* a = std::get_if<itch::AddOrder>(&m)) {
            live_.push_back(a->order_ref);
        } else if (auto* f = std::get_if<itch::AddOrderMpid>(&m)) {
            live_.push_back(f->add.order_ref);
        } else if (auto* e = std::get_if<itch::OrderExecuted>(&m)) {
            if (!shadow_.find(e->order_ref)) forget(e->order_ref);
        } else if (auto* c = std::get_if<itch::OrderExecutedPrice>(&m)) {
            if (!shadow_.find(c->exec.order_ref)) forget(c->exec.order_ref);
        } else if (auto* x = std::get_if<itch::OrderCancel>(&m)) {
            if (!shadow_.find(x->order_ref)) forget(x->order_ref);
        }
    }

    Config cfg_;
    std::mt19937_64 rng_;
    std::uniform_int_distribution<uint32_t> pct_{0, 999};
    lob::Book shadow_;
    std::vector<uint64_t> live_;
    uint64_t next_ref_ = 0;
    uint64_t match_    = 0;
    uint64_t ts_       = 34'200'000'000'000ULL;  // 09:30:00 ns since midnight
};

// Interleaves N independent per-symbol Generators, one locate code each.
// Order refs are globally unique across symbols (per-symbol ref_base): a
// message misrouted to the wrong book therefore hits UnknownId/DuplicateId
// instead of silently succeeding, which is what makes routing fuzzable.
// Per-symbol timestamps advance independently, so the merged stream is not
// globally timestamp-monotonic; none of the consumers care.
struct MultiConfig {
    uint64_t seed             = 42;
    uint32_t invalid_permille = 0;
    uint16_t n_symbols        = 8;
};

class MultiGenerator {
public:
    explicit MultiGenerator(MultiConfig mc)
        : pick_rng_(mc.seed ^ 0x9E3779B97F4A7C15ULL) {
        gens_.reserve(mc.n_symbols);
        for (uint16_t i = 0; i < mc.n_symbols; ++i) {
            Config c;
            c.seed             = mc.seed + 1'000'003ULL * (i + 1);
            c.invalid_permille = mc.invalid_permille;
            c.mid0             = 1'000'000 + 200'000u * i;  // $100, $120, ...
            c.locate           = uint16_t(i + 1);           // locate 0 unused
            c.ref_base         = (uint64_t(i) + 1) << 40;
            c.stock            = {'S','Y','N',
                                  char('0' + (i / 10) % 10),
                                  char('0' + i % 10), ' ',' ',' '};
            gens_.emplace_back(c);
        }
    }

    itch::Message next(bool& expect_ok) {
        return gens_[pick_rng_() % gens_.size()].next(expect_ok);
    }

    size_t n_symbols() const { return gens_.size(); }
    uint16_t locate(size_t i) const { return uint16_t(i + 1); }
    const Generator& sub(size_t i) const { return gens_[i]; }

private:
    std::mt19937_64 pick_rng_;
    std::vector<Generator> gens_;
};

}  // namespace synth
