// match.hpp - submit an aggressive order to a Book and EMIT the resulting
// wire messages. The inverse of feed.hpp: feed.hpp applies executions the
// exchange already decided; match_submit decides them and reports them in
// the same ITCH format, which is what closes the Phase 2 loop - the LM sees
// its own executions in the wire format it emits.
//
// Emission per submitted order, in outbound order:
//   - one 'E' per fill, order_ref = the RESTING order, match_num from the
//     shared counter. Always 'E', never 'C': every fill in this engine
//     executes at the resting order's display price, and ITCH reserves 'C'
//     for executions at a price different from display (hidden/improved),
//     which this engine never produces.
//   - one 'A' if a limit remainder rests (ref = the aggressor's ref, price =
//     its limit).
//   - nothing for a canceled market remainder: it never rested, so there is
//     no order to 'D'. A market order into an empty book emits no messages.
// The stream is reconstruction-closed: replaying it through feed.hpp apply()
// into a fresh Book reproduces this book's state exactly (fuzz-verified in
// tests/test_match.cpp).
#pragma once
#include <vector>

#include "book.hpp"
#include "itch.hpp"

namespace lob {

struct MatchRequest {
    uint16_t    locate    = 0;
    uint64_t    timestamp = 0;
    uint64_t    ref       = 0;        // aggressor's ref; rests under this ref
    Side        side      = Side::Buy;
    uint32_t    shares    = 0;
    uint32_t    limit     = 0;        // ignored when market
    bool        market    = false;
    itch::Stock stock     = {' ',' ',' ',' ',' ',' ',' ',' '};
};

// `fills` is a caller-owned scratch buffer (cleared per call, no per-match
// allocation in steady state). Messages are appended to `out`.
inline MatchOutcome match_submit(Book& b, const MatchRequest& req,
                                 uint64_t& match_seq,
                                 std::vector<Fill>& fills,
                                 std::vector<itch::Message>& out) {
    MatchOutcome mo = b.match(req.ref, req.side, req.shares, req.limit,
                              req.market, fills);
    if (mo.result != Result::Ok) return mo;
    for (const Fill& f : fills) {
        itch::OrderExecuted e;
        e.h.stock_locate = req.locate;
        e.h.timestamp    = req.timestamp;
        e.order_ref      = f.resting_ref;
        e.shares         = f.shares;
        e.match_num      = ++match_seq;
        out.push_back(e);
    }
    if (mo.rested) {
        itch::AddOrder a;
        a.h.stock_locate = req.locate;
        a.h.timestamp    = req.timestamp;
        a.order_ref      = req.ref;
        a.side           = req.side == Side::Buy ? 'B' : 'S';
        a.shares         = mo.rested;
        a.price          = req.limit;
        a.stock          = req.stock;
        out.push_back(a);
    }
    return mo;
}

}  // namespace lob
