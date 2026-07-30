// Limit order book, price-time priority. Prices are ITCH fixed point
// (uint32_t, 4 implied decimals), one book per instrument.
//
// Levels sit in a std::map keyed by price, bids descending and asks
// ascending, so begin() is the best level. Orders sit in an unordered_map
// by ref and are chained into an intrusive FIFO per level. Both containers
// are pointer-stable; the intrusive links depend on that.
//
// Two entry paths share this structure:
//   RECONSTRUCTION (add/execute/cancel/remove/replace): executes and
//     cancels arrive as explicit events from a real feed; an add or replace
//     priced through the opposite side is rejected - crossing flow executed
//     at the exchange, it never rests. Validated against a full real ITCH
//     day (zero rejects); do not change its semantics.
//   MATCHING (match): the Phase 2 inverse - an incoming aggressive order
//     walks the opposite side in price-time priority and the engine DECIDES
//     the fills. Built on the same primitives (execute + add), so the
//     ledger invariant is shared: added == resting + executed + canceled.
//     The aggressor's filled shares never enter the ledger - they never
//     rest; only the resting side's executions are counted, exactly as a
//     real feed reports them.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace lob {

enum class Side : uint8_t { Buy, Sell };

enum class Result : uint8_t {
    Ok,
    DuplicateId,
    UnknownId,
    ZeroShares,
    TooManyShares,   // execute/cancel more than resting
    WouldCross,      // add/replace priced through the opposite side
    BadReplace,      // replace with same id for orig and new
};
const char* to_string(Result r);

struct Level;

struct Order {
    uint64_t ref    = 0;
    uint32_t shares = 0;         // remaining
    uint32_t price  = 0;
    Side     side   = Side::Buy;
    uint64_t seq    = 0;         // arrival sequence, for FIFO assertions
    Order*   prev   = nullptr;   // intrusive FIFO links within level
    Order*   next   = nullptr;
    Level*   level  = nullptr;
};

struct Level {
    uint32_t price        = 0;
    uint64_t total_shares = 0;
    uint32_t order_count  = 0;
    Order*   head         = nullptr;  // oldest (front of queue)
    Order*   tail         = nullptr;  // newest
};

// One fill decided by match(). The price is the RESTING order's price:
// price improvement accrues to the aggressor.
struct Fill {
    uint64_t resting_ref = 0;
    uint32_t shares      = 0;
    uint32_t price       = 0;
};

struct MatchOutcome {
    Result   result   = Result::Ok;  // validation failures; fills untouched
    uint32_t filled   = 0;           // total shares crossed, == sum of fills
    uint32_t rested   = 0;           // limit remainder now resting under ref
    uint32_t canceled = 0;           // market remainder dropped (never rested,
                                     //   so it appears in no ledger and emits
                                     //   no message)
};

class Book {
public:
    // --- event application -------------------------------------------------
    Result add(uint64_t ref, Side side, uint32_t shares, uint32_t price);
    Result execute(uint64_t ref, uint32_t shares);          // E and C
    Result cancel(uint64_t ref, uint32_t shares);           // X (partial)
    Result remove(uint64_t ref);                            // D
    Result replace(uint64_t orig_ref, uint64_t new_ref,
                   uint32_t shares, uint32_t price);        // U

    // --- matching (Phase 2 path; reconstruction above is untouched) --------
    // Match an incoming aggressive order against the opposite side in strict
    // price-time priority (best price first, oldest first within a level).
    // Fills execute at the RESTING order's price. A limit order's unfilled
    // remainder rests at `limit`; a market order's remainder is canceled
    // (`market` = true; `limit` is then ignored). Fills are appended to
    // `fills` (cleared first). No self-trade prevention (see PLAN.md).
    MatchOutcome match(uint64_t ref, Side side, uint32_t shares,
                       uint32_t limit, bool market, std::vector<Fill>& fills);

    // --- queries -----------------------------------------------------------
    const Order* find(uint64_t ref) const;
    // 0 if side empty.
    uint32_t best_bid() const { return bids_.empty() ? 0 : bids_.begin()->first; }
    uint32_t best_ask() const { return asks_.empty() ? 0 : asks_.begin()->first; }
    bool     crossed()  const {
        return !bids_.empty() && !asks_.empty() && best_bid() >= best_ask();
    }
    size_t   open_orders()   const { return orders_.size(); }
    size_t   bid_levels()    const { return bids_.size(); }
    size_t   ask_levels()    const { return asks_.size(); }
    // Diagnostic: hash-table bucket count of the order pool (rehash tracking).
    size_t   order_buckets() const { return orders_.bucket_count(); }
    // Pre-size the order pool to avoid rehashes while it grows to `expected`.
    void     reserve(size_t expected) { orders_.reserve(expected); }
    uint64_t shares_added()    const { return shares_added_; }
    uint64_t shares_executed() const { return shares_executed_; }
    uint64_t shares_canceled() const { return shares_canceled_; }
    uint64_t shares_resting()  const { return shares_resting_; }

    // O(1) conservation + cross check. Called cheaply after every message.
    bool invariants_fast() const {
        return !crossed() &&
               shares_added_ == shares_resting_ + shares_executed_ +
                                shares_canceled_;
    }
    // Deep structural audit: walks every level and FIFO chain. O(n).
    // Returns empty string if clean, else a description of the violation.
    std::string audit() const;

    // Visit levels best-first. Visitor: (side, const Level&) -> bool keep_going.
    void for_each_level(const std::function<bool(Side, const Level&)>& f) const;

private:
    using BidMap = std::map<uint32_t, Level, std::greater<uint32_t>>;
    using AskMap = std::map<uint32_t, Level, std::less<uint32_t>>;

    Level& get_level(Side s, uint32_t price);
    void   push_back(Level& lvl, Order& o);
    void   unlink(Order& o);           // detach from level; erase empty level
    void   erase_order(Order& o);      // unlink + drop from pool

    BidMap bids_;
    AskMap asks_;
    std::unordered_map<uint64_t, Order> orders_;
    uint64_t seq_             = 0;
    uint64_t shares_added_    = 0;
    uint64_t shares_executed_ = 0;
    uint64_t shares_canceled_ = 0;
    uint64_t shares_resting_  = 0;
};

}  // namespace lob
