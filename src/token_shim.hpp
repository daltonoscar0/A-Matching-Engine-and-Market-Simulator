// token_shim.hpp - the token<->action boundary above src/adapter.hpp: turns
// generated OFTK 5-tuples into concrete EmittedActions so a trained model
// can drive the closed loop. The adapter stays at the absolute-price level
// (its design boundary); ALL token semantics live here.
//
// Resolution rules (PLAN.md 2026-07-30, Phase 7), representative and
// deterministic - the decode of a lossy token can only be a representative:
//   TYPE_ADD  -> Limit, side = SIDE slot, shares = size-bucket
//     representative. Price from PRICE_OFF against the CURRENT book:
//       0..+10 : price of that occupied same-side level (must exist, else
//                UnknownReference - the model referenced a level that
//                isn't there);
//       -1     : one tick inside the same-side best (needs a same-side
//                best to be inside of);
//       PX_TAIL: one tick beyond (worse than) the 11th occupied level
//                (index 10; needs >= 11 occupied levels);
//       UNK    : the side is empty; rest one tick off the OPPOSITE best
//                (needs an opposite side, else UnknownReference - an
//                empty book gives no price reference at all).
//   TYPE_EXEC -> Market from the OPPOSITE side (the SIDE slot names the
//     standing order, so the aggressor is its counterparty), shares =
//     representative. PRICE_OFF is not needed to act and is ignored.
//   TYPE_CANCEL / TYPE_DELETE -> Cancel at (side, resolved level price);
//     the level must exist (UNK/-1/absent level -> UnknownReference). The
//     adapter's Cancel removes the FIFO head in full, so the partial/full
//     distinction and the SIZE slot are dropped - cancel sizing is a
//     sanity-check quantity, logged, not scored.
//   TYPE_EXEC_HIDDEN / TYPE_CROSS -> Unparseable: legal vocab ids the BX
//     ingest never emits and the visible-book loop cannot express.
//   DT slot: ignored (the adapter loop has no clock; inter-arrival timing
//     is a property of the generated stream, not of one action).
//   Malformed tuples (any slot outside its id range) -> Unparseable.
// Shim-level rejects use the SAME Reject categories as the adapter and are
// counted alongside its counts, so a rejection breakdown covers both.
#ifndef EXCHANGE_TOKEN_SHIM_HPP
#define EXCHANGE_TOKEN_SHIM_HPP

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

#include "adapter.hpp"
#include "oftk.hpp"

namespace shim {

// WHY a resolution failed, at the granularity of the RULE that refused -
// diagnostic only, never consulted by the loop. The four adapter Reject
// categories say a tuple failed; these say which resolution rule said no,
// which is what distinguishes a model defect from a harness defect (a book
// with one occupied level per side rejects every add at index 1..10 no
// matter how good the model is - see the 2026-07-31 cold-start control).
enum class Why : uint8_t {
    None = 0,
    DecodeFailed,      // slot outside its id range
    HiddenOrCross,     // legal id the visible-book loop cannot express
    UnkNonAdd,         // UNK price slot on a cancel/delete: names no order
    UnkEmptyOpposite,  // UNK add, but the opposite side is empty too
    InsideNonAdd,      // -1 on a cancel/delete: nothing rests inside
    InsideNoBest,      // -1 add with no same-side best to be inside of
    TailNonAdd,        // PX_TAIL on a cancel/delete: names no level
    TailNoDepth,       // PX_TAIL add with < 11 occupied levels on that side
    LevelAbsent,       // 0..+10 index: that occupied level does not exist
    Resync,            // driver-level: misaligned or truncated pseudo-tuple
    kCount
};

inline const char* why_name(Why w) {
    switch (w) {
        case Why::None: return "none";
        case Why::DecodeFailed: return "decode_failed";
        case Why::HiddenOrCross: return "hidden_or_cross";
        case Why::UnkNonAdd: return "unk_non_add";
        case Why::UnkEmptyOpposite: return "unk_empty_opposite";
        case Why::InsideNonAdd: return "inside_non_add";
        case Why::InsideNoBest: return "inside_no_best";
        case Why::TailNonAdd: return "tail_non_add";
        case Why::TailNoDepth: return "tail_no_depth";
        case Why::LevelAbsent: return "level_absent";
        case Why::Resync: return "resync_or_truncated";
        default: return "?";
    }
}

struct Resolution {
    bool ok = false;
    lob::Reject reject = lob::Reject::None;  // set when !ok
    Why why = Why::None;                     // diagnostic detail for !ok
    lob::EmittedAction action;               // valid when ok
    oftk::ApproxEvent event;                 // decoded (valid if not
                                             // Unparseable-at-decode)
};

namespace detail {

// Price of the k-th occupied level (0 = best) on `side`, plus the level
// count up to k+2 (early-exit walk). Returns false if fewer than k+1
// occupied levels.
inline bool kth_level_price(const lob::Book& b, lob::Side side, int64_t k,
                            uint32_t& out) {
    int64_t i = 0;
    bool found = false;
    b.for_each_level([&](lob::Side s, const lob::Level& lv) {
        if (s != side) return true;
        if (i == k) {
            out = lv.price;
            found = true;
            return false;
        }
        ++i;
        return true;
    });
    return found;
}

}  // namespace detail

// Resolve one 5-token tuple against the current book. Does not mutate.
inline Resolution resolve(const uint16_t t[5], const oftk::TickerBins& bins,
                          const lob::Book& b) {
    Resolution r;
    if (!oftk::decode_event(t, bins, r.event)) {
        r.reject = lob::Reject::Unparseable;
        r.why = Why::DecodeFailed;
        return r;
    }
    const oftk::ApproxEvent& e = r.event;
    if (e.type == oftk::MsgType::ExecHidden ||
        e.type == oftk::MsgType::CrossTrade) {
        r.reject = lob::Reject::Unparseable;  // outside the visible-book loop
        r.why = Why::HiddenOrCross;
        return r;
    }
    const lob::Side side =
        e.direction > 0 ? lob::Side::Buy : lob::Side::Sell;
    const int64_t tick = bins.tick_size > 0 ? bins.tick_size : 100;

    if (e.type == oftk::MsgType::ExecVisible) {
        r.action.kind = lob::ActionKind::Market;
        r.action.side = side == lob::Side::Buy ? lob::Side::Sell
                                               : lob::Side::Buy;
        r.action.price = 0;
        r.action.shares = uint32_t(e.size);
        r.ok = true;
        return r;
    }

    // Add / PartialCancel / Delete all need a resolved level price.
    uint32_t price = 0;
    if (!e.has_ref) {  // UNK: same side empty
        if (e.type != oftk::MsgType::Add) {
            r.reject = lob::Reject::UnknownReference;  // nothing to cancel
            r.why = Why::UnkNonAdd;
            return r;
        }
        const uint32_t opp = side == lob::Side::Buy ? b.best_ask()
                                                    : b.best_bid();
        if (opp == 0) {
            r.reject = lob::Reject::UnknownReference;  // no reference at all
            r.why = Why::UnkEmptyOpposite;
            return r;
        }
        price = side == lob::Side::Buy ? uint32_t(int64_t(opp) - tick)
                                       : uint32_t(int64_t(opp) + tick);
    } else if (e.lvl_off == -1) {
        if (e.type != oftk::MsgType::Add) {
            r.reject = lob::Reject::UnknownReference;  // no order inside
            r.why = Why::InsideNonAdd;
            return r;
        }
        const uint32_t best = side == lob::Side::Buy ? b.best_bid()
                                                     : b.best_ask();
        if (best == 0) {
            r.reject = lob::Reject::UnknownReference;
            r.why = Why::InsideNoBest;
            return r;
        }
        price = side == lob::Side::Buy ? uint32_t(int64_t(best) + tick)
                                       : uint32_t(int64_t(best) - tick);
    } else if (e.lvl_off > oftk::kPxMax) {  // PX_TAIL
        if (e.type != oftk::MsgType::Add) {
            // A cancel "somewhere beyond +10" names no level.
            r.reject = lob::Reject::UnknownReference;
            r.why = Why::TailNonAdd;
            return r;
        }
        uint32_t deep = 0;
        if (!detail::kth_level_price(b, side, oftk::kPxMax, deep)) {
            r.reject = lob::Reject::UnknownReference;
            r.why = Why::TailNoDepth;
            return r;
        }
        price = side == lob::Side::Buy ? uint32_t(int64_t(deep) - tick)
                                       : uint32_t(int64_t(deep) + tick);
    } else {
        if (!detail::kth_level_price(b, side, e.lvl_off, price)) {
            r.reject = lob::Reject::UnknownReference;
            r.why = Why::LevelAbsent;
            return r;
        }
    }
    r.action.kind = e.type == oftk::MsgType::Add ? lob::ActionKind::Limit
                                                 : lob::ActionKind::Cancel;
    r.action.side = side;
    r.action.price = price;
    r.action.shares = uint32_t(e.size);
    r.ok = true;
    return r;
}

// Merged rejection accounting: shim-level rejects (resolution) and
// adapter-level rejects (validation) in one breakdown.
struct Counts {
    uint64_t tuples = 0;      // 5-tuples seen
    uint64_t submitted = 0;   // reached Adapter::submit
    uint64_t applied = 0;
    uint64_t specials = 0;    // BOS/EOS/SESSION/HALT/RESUME skipped
    uint64_t resyncs = 0;     // stream positions skipped to find a TYPE token
    std::array<uint64_t, size_t(lob::Reject::kCount)> rejects{};

    uint64_t rejected() const {
        uint64_t n = 0;
        for (uint64_t v : rejects) n += v;
        return n;
    }
};

// Resolve + submit one tuple; all rejections (shim or adapter) land in c.
inline lob::Outcome step(lob::Adapter& a, const uint16_t t[5],
                         const oftk::TickerBins& bins, Counts& c) {
    ++c.tuples;
    Resolution r = resolve(t, bins, a.book());
    if (!r.ok) {
        ++c.rejects[size_t(r.reject)];
        lob::Outcome o;
        o.reject = r.reject;
        return o;
    }
    ++c.submitted;
    lob::Outcome o = a.submit(r.action);
    if (o.applied()) ++c.applied;
    else ++c.rejects[size_t(o.reject)];
    return o;
}

// Drive the adapter from a raw generated token stream. Specials are
// skipped; a position whose token cannot start a tuple (not a TYPE id) is
// counted as one Unparseable and the stream advances one token (resync) -
// a generative model CAN emit misaligned garbage and the loop must not
// wedge. A truncated trailing tuple is likewise one Unparseable.
// `on_tuple`, when set, fires after every counted tuple (resolved or not)
// with the resolution and outcome - the instrumentation hook the
// stationarity diagnostic (tools/sim_health) records from. For resync /
// truncation pseudo-tuples it receives an empty Resolution with reject =
// Unparseable.
using TupleHook =
    std::function<void(size_t idx, const Resolution&, const lob::Outcome&)>;

inline void drive(lob::Adapter& a, const std::vector<uint16_t>& stream,
                  const oftk::TickerBins& bins, Counts& c,
                  const TupleHook& on_tuple = {}) {
    size_t i = 0;
    size_t idx = 0;
    const size_t n = stream.size();
    auto reject_one = [&]() {
        ++c.tuples;
        ++c.rejects[size_t(lob::Reject::Unparseable)];
        if (on_tuple) {
            Resolution r;
            r.reject = lob::Reject::Unparseable;
            r.why = Why::Resync;
            lob::Outcome o;
            o.reject = lob::Reject::Unparseable;
            on_tuple(idx++, r, o);
        } else {
            ++idx;
        }
    };
    while (i < n) {
        const uint16_t t = stream[i];
        if (t <= oftk::RESUME) {  // UNK..RESUME: specials, no action
            ++c.specials;
            ++i;
            continue;
        }
        if (t < oftk::TYPE_BASE || t >= oftk::SIDE_BASE) {
            ++c.resyncs;
            reject_one();
            ++i;
            continue;
        }
        if (i + oftk::kEventTokens > n) {
            reject_one();
            break;
        }
        // step() re-resolves; resolve here once so the hook sees it.
        Resolution r = resolve(&stream[i], bins, a.book());
        ++c.tuples;
        lob::Outcome o;
        if (!r.ok) {
            ++c.rejects[size_t(r.reject)];
            o.reject = r.reject;
        } else {
            ++c.submitted;
            o = a.submit(r.action);
            if (o.applied()) ++c.applied;
            else ++c.rejects[size_t(o.reject)];
        }
        if (on_tuple) on_tuple(idx, r, o);
        ++idx;
        i += oftk::kEventTokens;
    }
}

}  // namespace shim

#endif  // EXCHANGE_TOKEN_SHIM_HPP
