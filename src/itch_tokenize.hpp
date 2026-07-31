// ITCH -> OFTK ingest: parse a BX day, drive THIS repo's reconstruction,
// and emit [TYPE][SIDE][PRICE_OFF][SIZE][DT] 5-tuples per symbol in the
// OFTK v2 format (src/oftk.hpp). Design decisions (PLAN.md 2026-07-30):
//   - 'U' expands to Delete + Add (user's decision). The delete half
//     carries the original order's remaining shares at its price; the add
//     half carries the replacement price/size, same side, dt = 0 (same
//     timestamp). The pair_first flag marks the delete half; fingerprints
//     skip the intra-pair state so the expanded replay is comparable to
//     the atomic Book::replace on the raw path.
//   - PRICE_OFF is the signed occupied-level index on the event's side of
//     the book state BEFORE the event applies (tape's level_index rule):
//     0 = at best, +k = k occupied levels strictly better, -1 = better
//     than best (inside spread), UNK when the side is empty. The add half
//     of a U indexes against the post-delete book (its true pre-state).
//   - Tokens cover the 09:30-16:00 window only (auction exclusion = time
//     filter, per stylized); ALL messages are applied whatever their
//     timestamp (books must be correct all day). dt is measured between
//     consecutive emitted events; the first emitted event gets dt = 0.
//   - F -> Add (MPID dropped), C -> ExecVisible (exec price detail
//     dropped by the level-index scheme), E -> ExecVisible, X ->
//     PartialCancel, D -> Delete. P/Q/H are skipped upstream by
//     FrameReader (not book types). Consequence, documented rather than
//     hidden: the HALT/RESUME vocab ids are never emitted by this ingest,
//     and a trading halt appears in the stream as one large DT gap rather
//     than tape's HALT marker + clock advance. Acceptable for BX (halts
//     are rare and carry no book change); revisit only if halt structure
//     ever matters for a probe.
// Zero-reject bar: any reject while applying real data is OUR bug and is
// surfaced as a hard error, never tolerated.
#ifndef EXCHANGE_ITCH_TOKENIZE_HPP
#define EXCHANGE_ITCH_TOKENIZE_HPP

#include <cstdint>
#include <map>
#include <string>
#include <variant>
#include <vector>

#include "bookset.hpp"
#include "feed.hpp"
#include "itch.hpp"
#include "itch_replay.hpp"
#include "oftk.hpp"

namespace ingest {

constexpr uint64_t kNs = 1000000000ull;
constexpr uint64_t kOpenNs = 34200 * kNs;   // 09:30:00
constexpr uint64_t kCloseNs = 57600 * kNs;  // 16:00:00

inline bool in_window(uint64_t ts) { return ts >= kOpenNs && ts < kCloseNs; }

inline uint64_t ts_of(const itch::Message& m) {
    return std::visit(
        [](const auto& x) -> uint64_t {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, itch::AddOrderMpid>)
                return x.add.h.timestamp;
            else if constexpr (std::is_same_v<T, itch::OrderExecutedPrice>)
                return x.exec.h.timestamp;
            else
                return x.h.timestamp;
        },
        m);
}

// One exact (pre-quantization) event of the expanded per-symbol stream.
// Carries everything the tokens quantize (type/direction/lvl_off/size/dt)
// PLUS the exact fields (price, ref) that make the stream replayable into
// a fresh Book - the lossless layer of the round-trip test.
struct Event {
    oftk::MsgType type = oftk::MsgType::Add;
    int8_t direction = 1;   // +1 bid, -1 ask (side of the standing order)
    bool has_px = false;    // false -> UNK in the PRICE_OFF slot
    int64_t lvl_off = 0;    // uncapped occupied-level index (token clamps)
    uint32_t price = 0;     // exact ITCH fixed-point price
    uint32_t size = 0;      // exact shares (exec/cancel amount or remaining)
    uint64_t ref = 0;       // order ref this event targets / creates
    uint64_t ts = 0;
    int64_t dt_ns = 0;      // vs previous emitted event; 0 outside window
    bool in_win = false;
    bool pair_first = false;  // delete half of an expanded U
};

struct StreamStats {
    uint64_t events = 0;  // emitted (in-window) events
    uint64_t px_unk = 0, px_inside = 0, px_tail = 0;
    uint64_t dt_zero = 0, dt_tail = 0;
    uint64_t sz_hist[oftk::kSizeBins] = {};
};

struct IngestResult {
    std::vector<uint16_t> tokens;  // empty when bins == nullptr (fit mode)
    std::vector<Event> events;     // all-day expanded stream, target symbol
    StreamStats stats;
    std::map<int64_t, uint64_t> px_hist;  // uncapped lvl_off, in-window
    uint64_t px_unk_inwin = 0;            // in-window events with empty side
    uint64_t msgs_total = 0;              // book msgs applied, all symbols
    uint64_t msgs_symbol = 0;             // raw book msgs, target symbol
    uint64_t events_inwindow = 0;
    uint64_t u_expanded = 0;  // U messages expanded to Delete+Add
    // STRUCTURAL CLASSIFICATION of in-window ADD prices against the
    // pre-event book (2026-07-31). PRICE_OFF records the COUNT of strictly
    // better occupied levels, so "join occupied level k" and "open a NEW
    // level just better than occupied level k" encode to the SAME token -
    // and the shim's inverse always picks "join level k". These counters
    // measure how much of real flow that conflation covers, instead of
    // assuming it is rare. Adds only: E/C/X/D take their price from a
    // standing order, which is at an occupied level by construction.
    uint64_t add_at_level = 0;        // price == an occupied level: exact
    uint64_t add_new_interior = 0;    // NEW level between two occupied ones
    uint64_t add_new_bottom = 0;      // NEW level worse than every occupied
    uint64_t add_inside = 0;          // better than best (lvl_off -1)
    uint64_t add_no_side = 0;         // side empty (UNK)
    uint64_t add_new_bottom_tail = 0; // of add_new_bottom, those at idx > 10
                                      // (PX_TAIL, the one new-level case the
                                      //  shim inverse can express)
};

namespace detail {

// Occupied-level index of `price` on `side` of `b`, tape's level_index
// rule, computed by walking levels best-first. Returns false when the side
// has no occupied level (PRICE_OFF = UNK).
inline bool level_index(const lob::Book& b, lob::Side side, uint32_t price,
                        int64_t& out) {
    bool any = false;
    bool first = true;
    uint32_t best = 0;
    int64_t idx = 0;
    bool stopped = false;
    b.for_each_level([&](lob::Side s, const lob::Level& lv) {
        if (s != side) return true;  // other side: keep walking
        any = true;
        if (first) {
            best = lv.price;
            first = false;
        }
        const bool better = side == lob::Side::Buy ? lv.price > price
                                                   : lv.price < price;
        if (!better) {
            stopped = true;
            return false;  // best-first: nothing further is better
        }
        ++idx;
        return true;
    });
    (void)stopped;
    if (!any) return false;
    if (idx == 0 && price != best) out = -1;  // better than the best
    else out = idx;
    return true;
}

// Where an ADD's price sits relative to the occupied levels on its side.
// Partitions every add; see the IngestResult counters for why it matters.
enum class AddPx : uint8_t { NoSide, Inside, AtLevel, NewInterior, NewBottom };

inline AddPx classify_add(const lob::Book& b, lob::Side side, uint32_t price) {
    const size_t n_levels =
        side == lob::Side::Buy ? b.bid_levels() : b.ask_levels();
    if (n_levels == 0) return AddPx::NoSide;
    bool exact = false;
    size_t n_better = 0;  // == lvl_off from level_index, by construction
    b.for_each_level([&](lob::Side s, const lob::Level& lv) {
        if (s != side) return true;
        if (lv.price == price) {
            exact = true;
            return false;
        }
        const bool better = side == lob::Side::Buy ? lv.price > price
                                                   : lv.price < price;
        if (!better) return false;  // best-first: nothing further is better
        ++n_better;
        return true;
    });
    if (exact) return AddPx::AtLevel;
    if (n_better == 0) return AddPx::Inside;          // better than best
    if (n_better == n_levels) return AddPx::NewBottom;  // below the bottom
    return AddPx::NewInterior;
}

inline int8_t dir_of(lob::Side s) {
    return s == lob::Side::Buy ? int8_t{1} : int8_t{-1};
}

// FNV-1a over the observable book state; folded after every event so
// intermediate states are compared, not just the (drained) close. EVERY
// level is folded (review 2026-07-30: a top-12 cap left divergence deeper
// than level 12 invisible when aggregates matched), plus the event
// timestamp, so a wrong recorded ts cannot agree with the raw side.
struct Fingerprint {
    uint64_t h = 1469598103934665603ull;
    uint64_t folds = 0;

    void fold(uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            h ^= (v >> (i * 8)) & 0xff;
            h *= 1099511628211ull;
        }
    }
    void fold_book(const lob::Book& b) {
        ++folds;
        fold(b.best_bid());
        fold(b.best_ask());
        fold(b.open_orders());
        fold(b.shares_added());
        fold(b.shares_executed());
        fold(b.shares_canceled());
        fold(b.shares_resting());
        b.for_each_level([&](lob::Side s, const lob::Level& lv) {
            fold(uint64_t(s));
            fold(lv.price);
            fold(lv.total_shares);
            fold(lv.order_count);
            return true;
        });
    }
};

}  // namespace detail

// Tokenize one day for one locate. Applies EVERY decoded book message to
// `set` (zero-reject bar); extracts the expanded exact-event stream for
// `locate`; quantizes in-window events to tokens when `bins` is given.
// max_msgs > 0 caps decoded book messages (bounded-slice tests; the same
// cap in replay_raw_symbol stops at the identical point).
inline bool ingest_day(const uint8_t* data, size_t size, uint16_t locate,
                       const oftk::TickerBins* bins, IngestResult& out,
                       std::string* err, size_t max_msgs = 0) {
    if (bins && !bins->valid()) {
        if (err) *err = "invalid ticker bins";
        return false;
    }
    lob::BookSet set;
    itch::FrameReader rd{data, size};
    if (bins) {
        out.tokens.push_back(oftk::BOS);
        out.tokens.push_back(oftk::SESSION_OPEN);
    }
    bool have_prev_ts = false;
    uint64_t prev_ts = 0;

    auto emit = [&](Event& e) {
        e.in_win = in_window(e.ts);
        if (e.in_win) {
            e.dt_ns = have_prev_ts ? int64_t(e.ts) - int64_t(prev_ts) : 0;
            prev_ts = e.ts;
            have_prev_ts = true;
            ++out.events_inwindow;
            if (e.has_px) ++out.px_hist[e.lvl_off];
            else ++out.px_unk_inwin;
            if (bins) {
                oftk::ApproxEvent a;
                a.type = e.type;
                a.direction = e.direction;
                a.has_ref = e.has_px;
                a.lvl_off = e.lvl_off;
                a.size = e.size;
                a.dt_ns = e.dt_ns;
                uint16_t t[5];
                oftk::encode_event(a, *bins, t);
                out.tokens.insert(out.tokens.end(), t, t + 5);
                ++out.stats.events;
                if (t[2] == oftk::UNK) ++out.stats.px_unk;
                if (t[2] == oftk::PX_INSIDE) ++out.stats.px_inside;
                if (t[2] == oftk::PX_TAIL) ++out.stats.px_tail;
                if (t[4] == oftk::DT_ZERO) ++out.stats.dt_zero;
                if (t[4] == oftk::DT_TAIL) ++out.stats.dt_tail;
                ++out.stats.sz_hist[t[3] - oftk::SZ_BASE];
            }
        } else {
            e.dt_ns = 0;
        }
        out.events.push_back(e);
    };

    auto count_add_px = [&](const lob::Book& b, lob::Side side,
                            uint32_t price, int64_t lvl_off) {
        switch (detail::classify_add(b, side, price)) {
            case detail::AddPx::NoSide: ++out.add_no_side; break;
            case detail::AddPx::Inside: ++out.add_inside; break;
            case detail::AddPx::AtLevel: ++out.add_at_level; break;
            case detail::AddPx::NewInterior: ++out.add_new_interior; break;
            case detail::AddPx::NewBottom:
                ++out.add_new_bottom;
                if (lvl_off > oftk::kPxMax) ++out.add_new_bottom_tail;
                break;
        }
    };

    auto fail = [&](const std::string& what, uint64_t ts) {
        if (err)
            *err = what + " at ts " + std::to_string(ts) +
                   " (reject on real data = our bug)";
        return false;
    };

    size_t seen = 0;
    bool capped = false;
    while (auto m = rd.next()) {
        if (max_msgs && ++seen > max_msgs) {
            capped = true;
            break;
        }
        const uint16_t loc = lob::locate_of(*m);
        if (loc != locate) {
            if (set.apply(*m) != lob::Result::Ok)
                return fail("off-symbol reject", ts_of(*m));
            ++out.msgs_total;
            continue;
        }
        lob::Book& b = set.book(loc);
        const uint64_t ts = ts_of(*m);
        ++out.msgs_total;
        ++out.msgs_symbol;

        if (const auto* u = std::get_if<itch::OrderReplace>(&*m)) {
            // Expand U -> Delete(orig) + Add(new). Each half's PRICE_OFF is
            // computed against its true pre-state (add half: post-delete).
            const lob::Order* o = b.find(u->orig_order_ref);
            if (!o) return fail("U with unknown orig ref", ts);
            const lob::Side side = o->side;
            Event del;
            del.type = oftk::MsgType::Delete;
            del.direction = detail::dir_of(side);
            del.price = o->price;
            del.size = o->shares;
            del.ref = u->orig_order_ref;
            del.ts = ts;
            del.pair_first = true;
            del.has_px = detail::level_index(b, side, del.price, del.lvl_off);
            emit(del);
            if (b.remove(u->orig_order_ref) != lob::Result::Ok)
                return fail("U delete-half reject", ts);

            Event add;
            add.type = oftk::MsgType::Add;
            add.direction = detail::dir_of(side);
            add.price = u->price;
            add.size = u->shares;
            add.ref = u->new_order_ref;
            add.ts = ts;
            add.has_px = detail::level_index(b, side, add.price, add.lvl_off);
            if (in_window(ts)) count_add_px(b, side, add.price, add.lvl_off);
            emit(add);
            if (b.add(u->new_order_ref, side, u->shares, u->price) !=
                lob::Result::Ok)
                return fail("U add-half reject", ts);
            ++out.u_expanded;
            continue;
        }

        Event e;
        e.ts = ts;
        bool ok = true;
        std::visit(
            [&](const auto& x) {
                using T = std::decay_t<decltype(x)>;
                if constexpr (std::is_same_v<T, itch::AddOrder>) {
                    e.type = oftk::MsgType::Add;
                    e.direction = x.side == 'B' ? int8_t{1} : int8_t{-1};
                    e.price = x.price;
                    e.size = x.shares;
                    e.ref = x.order_ref;
                } else if constexpr (std::is_same_v<T, itch::AddOrderMpid>) {
                    e.type = oftk::MsgType::Add;  // MPID attribution dropped
                    e.direction = x.add.side == 'B' ? int8_t{1} : int8_t{-1};
                    e.price = x.add.price;
                    e.size = x.add.shares;
                    e.ref = x.add.order_ref;
                } else {
                    // E/C/X/D: the standing order names side and price.
                    const uint64_t ref = lob::ref_of(*m);
                    const lob::Order* o = b.find(ref);
                    if (!o) {
                        ok = false;
                        return;
                    }
                    e.direction = detail::dir_of(o->side);
                    e.price = o->price;
                    e.ref = ref;
                    if constexpr (std::is_same_v<T, itch::OrderExecuted>) {
                        e.type = oftk::MsgType::ExecVisible;
                        e.size = x.shares;
                    } else if constexpr (std::is_same_v<
                                             T, itch::OrderExecutedPrice>) {
                        e.type = oftk::MsgType::ExecVisible;  // price dropped
                        e.size = x.exec.shares;
                    } else if constexpr (std::is_same_v<T,
                                                        itch::OrderCancel>) {
                        e.type = oftk::MsgType::PartialCancel;
                        e.size = x.shares;
                    } else {
                        e.type = oftk::MsgType::Delete;
                        e.size = o->shares;  // remaining, LOBSTER convention
                    }
                }
            },
            *m);
        if (!ok) return fail("event on unknown ref", ts);
        const lob::Side side =
            e.direction > 0 ? lob::Side::Buy : lob::Side::Sell;
        e.has_px = detail::level_index(b, side, e.price, e.lvl_off);
        if (e.type == oftk::MsgType::Add && in_window(ts))
            count_add_px(b, side, e.price, e.lvl_off);
        emit(e);
        if (lob::apply(b, *m) != lob::Result::Ok)
            return fail("apply reject", ts);
    }
    if (rd.error && !capped) {
        if (err) *err = "malformed frame (desynced stream)";
        return false;
    }
    if (bins) {
        out.tokens.push_back(oftk::SESSION_CLOSE);
        out.tokens.push_back(oftk::EOS);
    }
    return true;
}

// ---- round-trip layer 1: replay the exact expanded stream ------------------
// Drives a fresh Book from the detokenizer-facing event stream (via exact
// price/size/ref) and folds the running fingerprint. With `strict`,
// additionally validates every recorded field the raw replay would not
// exercise on its own (review 2026-07-30 - these were blind spots):
//   - PRICE_OFF re-derived from the fresh book pre-apply;
//   - for Exec/Cancel/Delete, the recorded side/price must match the
//     standing order in the FRESH book, and a Delete's recorded size must
//     equal that order's remaining shares (b.remove ignores e.size, so
//     without this the Delete SZ token was validated by nothing);
//   - dt recomputed from the event timestamps (0 for the first emitted
//     and for out-of-window events).
inline bool replay_events(const std::vector<Event>& evs, lob::Book& b,
                          bool strict, detail::Fingerprint& fp,
                          std::string* err) {
    bool have_prev_ts = false;
    uint64_t prev_ts = 0;
    for (size_t i = 0; i < evs.size(); ++i) {
        const Event& e = evs[i];
        const lob::Side side =
            e.direction > 0 ? lob::Side::Buy : lob::Side::Sell;
        if (strict) {
            int64_t idx = 0;
            const bool has = detail::level_index(b, side, e.price, idx);
            if (has != e.has_px || (has && idx != e.lvl_off)) {
                if (err)
                    *err = "lvl_off mismatch at event " + std::to_string(i) +
                           ": stream says " +
                           (e.has_px ? std::to_string(e.lvl_off) : "UNK") +
                           ", fresh book says " +
                           (has ? std::to_string(idx) : "UNK");
                return false;
            }
            if (e.type != oftk::MsgType::Add) {
                const lob::Order* o = b.find(e.ref);
                if (!o || o->price != e.price ||
                    detail::dir_of(o->side) != e.direction ||
                    (e.type == oftk::MsgType::Delete &&
                     o->shares != e.size)) {
                    if (err)
                        *err = "recorded side/price/size disagrees with the "
                               "fresh book's standing order at event " +
                               std::to_string(i);
                    return false;
                }
            }
            const int64_t want_dt =
                e.in_win ? (have_prev_ts ? int64_t(e.ts) - int64_t(prev_ts)
                                         : 0)
                         : 0;
            if (e.dt_ns != want_dt) {
                if (err)
                    *err = "dt mismatch at event " + std::to_string(i) +
                           ": recorded " + std::to_string(e.dt_ns) +
                           ", recomputed " + std::to_string(want_dt);
                return false;
            }
            if (e.in_win) {
                prev_ts = e.ts;
                have_prev_ts = true;
            }
        }
        lob::Result r = lob::Result::Ok;
        switch (e.type) {
            case oftk::MsgType::Add:
                r = b.add(e.ref, side, e.size, e.price);
                break;
            case oftk::MsgType::PartialCancel:
                r = b.cancel(e.ref, e.size);
                break;
            case oftk::MsgType::Delete:
                r = b.remove(e.ref);
                break;
            case oftk::MsgType::ExecVisible:
                r = b.execute(e.ref, e.size);
                break;
            default:
                r = lob::Result::UnknownId;  // never emitted by the ingest
        }
        if (r != lob::Result::Ok) {
            if (err)
                *err = "replay reject (" + std::string(lob::to_string(r)) +
                       ") at event " + std::to_string(i);
            return false;
        }
        if (!e.pair_first) {
            fp.fold(e.ts);
            fp.fold_book(b);
        }
    }
    return true;
}

// The raw side of layer 1: drive a fresh Book straight from the day's raw
// ITCH messages for `locate` (U applied atomically via Book::replace) and
// fold at the same points (once per raw message).
inline bool replay_raw_symbol(const uint8_t* data, size_t size,
                              uint16_t locate, lob::Book& b,
                              detail::Fingerprint& fp, std::string* err,
                              size_t max_msgs = 0) {
    itch::FrameReader rd{data, size};
    size_t seen = 0;
    bool capped = false;
    while (auto m = rd.next()) {
        if (max_msgs && ++seen > max_msgs) {
            capped = true;
            break;
        }
        if (lob::locate_of(*m) != locate) continue;
        if (lob::apply(b, *m) != lob::Result::Ok) {
            if (err) *err = "raw replay reject at ts " +
                            std::to_string(ts_of(*m));
            return false;
        }
        fp.fold(ts_of(*m));
        fp.fold_book(b);
    }
    // A desync is fatal unless the loop stopped BECAUSE it hit the cap
    // (review 2026-07-30: `!max_msgs` alone swallowed early desyncs, so a
    // capped run could "verify" an arbitrarily short prefix of garbage).
    if (rd.error && !capped) {
        if (err) *err = "malformed frame (desynced stream)";
        return false;
    }
    return true;
}

// ---- round-trip layer 2: tokens <-> exact stream inverse checks ------------
// The tokens are a quantization of the exact stream, so the invertible
// content is checked exactly: stream shape, per-event re-encode identity
// (catches a writer/token corruption), decode agreement on the lossless
// fields (type/side/px slot), tape's roundtrip_ok, and bucket membership
// of the exact size/dt in the decoded bucket (== token equality, since the
// bucket functions are deterministic).
inline bool verify_tokens(const IngestResult& r, const oftk::TickerBins& bins,
                          std::string* err) {
    const std::vector<uint16_t>& t = r.tokens;
    auto fail = [&](const std::string& what) {
        if (err) *err = what;
        return false;
    };
    if (t.size() < 4 || t[0] != oftk::BOS || t[1] != oftk::SESSION_OPEN ||
        t[t.size() - 2] != oftk::SESSION_CLOSE || t.back() != oftk::EOS)
        return fail("bad stream frame (BOS/SESSION/EOS)");
    if ((t.size() - 4) % oftk::kEventTokens != 0)
        return fail("payload not a multiple of 5");
    const size_t n = (t.size() - 4) / oftk::kEventTokens;
    if (n != r.events_inwindow)
        return fail("token event count != in-window event count");
    size_t k = 0;
    for (const Event& e : r.events) {
        if (!e.in_win) continue;
        const uint16_t* tt = &t[2 + k * oftk::kEventTokens];
        ++k;
        oftk::ApproxEvent exact;
        exact.type = e.type;
        exact.direction = e.direction;
        exact.has_ref = e.has_px;
        exact.lvl_off = e.lvl_off;
        exact.size = e.size;
        exact.dt_ns = e.dt_ns;
        uint16_t enc[5];
        if (!oftk::encode_event(exact, bins, enc))
            return fail("re-encode failed at event " + std::to_string(k - 1));
        if (std::memcmp(enc, tt, sizeof enc) != 0)
            return fail("token/stream mismatch at event " +
                        std::to_string(k - 1));
        if (!oftk::roundtrip_ok(tt, bins))
            return fail("roundtrip_ok failed at event " +
                        std::to_string(k - 1));
        oftk::ApproxEvent d;
        if (!oftk::decode_event(tt, bins, d))
            return fail("decode failed at event " + std::to_string(k - 1));
        if (d.type != e.type || d.direction != e.direction ||
            d.has_ref != e.has_px)
            return fail("decode disagrees on lossless fields at event " +
                        std::to_string(k - 1));
    }
    return true;
}

}  // namespace ingest

#endif  // EXCHANGE_ITCH_TOKENIZE_HPP
