// adapter.hpp - the Phase 2 closed-loop boundary between a generative model
// and the matching engine. The model proposes actions; this layer VALIDATES
// them before they can touch the book, applies the survivors through the
// engine (match() for marketable flow, add() for resting limits, remove()
// for cancels), and hands back the book state the model conditions on next.
//
// Three responsibilities, kept separate:
//   1. Validation front-end. The model emits garbage, especially early in
//      training; the engine must never be what discovers this. Every action
//      is classified and, if bad, REJECTED and counted by reason - never
//      applied. Rejection is total: a rejected action leaves the book
//      bit-identical (guaranteed because every engine primitive we call is
//      pre-validated to return Ok, and the primitives themselves reject
//      before mutating - see Book::add/remove).
//   2. State feedback. After each applied action, expose the top-N levels
//      per side plus the touch - the state the model sees next.
//   3. Sampling controls. temperature/top-k live HERE so the engine stays
//      deterministic and the stochasticity is isolated where it can be swept.
//
// This boundary is deliberately at the concrete order-action level (absolute
// price, side, size), NOT the factored-token level: the token<->price decode
// and the 'U' representation fork are tokenizer decisions still open
// (docs/FORMAT_RECONCILIATION.md), and the engine loop must not bake them in.
#pragma once
#include <array>
#include <cstdint>
#include <random>
#include <vector>

#include "match.hpp"

namespace lob {

enum class ActionKind : uint8_t { Limit, Market, Cancel };

// A model-proposed action in engine terms. `price` is ITCH fixed point
// (1e-4 dollars): the limit price for Limit, the target level for Cancel,
// ignored for Market.
struct EmittedAction {
    ActionKind kind   = ActionKind::Limit;
    Side       side   = Side::Buy;
    uint32_t   price  = 0;
    uint32_t   shares = 0;
};

enum class Reject : uint8_t {
    None = 0,
    Unparseable,        // structurally malformed: zero size, limit w/o price
    UnknownReference,   // cancel targets a level with no resting order
    InvariantViolation, // engine would reject on an invariant (defensive)
    EconomicallyAbsurd, // price/size implausible: far off touch, over caps
    kCount
};
const char* to_string(Reject r);

struct Outcome {
    Reject   reject   = Reject::None;
    uint32_t filled   = 0;   // shares crossed (Limit/Market)
    uint32_t rested   = 0;   // limit remainder now resting
    uint32_t canceled = 0;   // shares removed (Cancel)
    bool applied() const { return reject == Reject::None; }
};

struct LevelView { uint32_t price = 0; uint64_t shares = 0; };

// The state handed back to the model after each applied action.
struct BookView {
    std::vector<LevelView> bids;   // best-first, up to feedback_levels
    std::vector<LevelView> asks;
    uint32_t best_bid = 0;
    uint32_t best_ask = 0;
    uint32_t spread   = 0;         // 0 if one-sided
    bool     two_sided = false;
};

struct AdapterConfig {
    // ---- state feedback ----
    // Top-N aggregated levels per side. Default 11 to cover the tokenizer's
    // PRICE_OFF window (occupied levels 0..+10 on the event's side): the
    // model's conditioning state and its emittable price range then align.
    // Full depth (BX peaks ~87 levels) would waste per-step work; a scalar
    // spread/imbalance summary would under-determine PRICE_OFF. Configurable;
    // choice logged in PLAN.md Decisions.
    size_t   feedback_levels = 11;

    // ---- economic plausibility bounds (applied BEFORE the engine) ----
    uint32_t max_shares    = 1'000'000;    // BX max observed add ~350k
    uint32_t abs_price_cap = 100'000'000;  // $10,000; BX max ~$4,090
    double   max_rel_price = 0.5;          // |price/touch - 1| beyond this = absurd

    // ---- sampling controls ----
    double   temperature = 1.0;   // > 0; scales logits before softmax
    int      top_k       = 0;     // 0 = disabled; else keep k highest logits
    uint64_t seed        = 0;
};

// temperature/top-k sample over model logits. Pure in (logits, cfg, rng) so
// it is deterministic and unit-testable; the Adapter owns an RNG so engine
// state stays deterministic and only this call is stochastic. Returns the
// chosen index, or logits.size() if logits is empty.
size_t sample_index(const std::vector<double>& logits,
                    const AdapterConfig& cfg, std::mt19937_64& rng);

class Adapter {
public:
    explicit Adapter(const AdapterConfig& cfg = {})
        : cfg_(cfg), rng_(cfg.seed) {}

    // Validate, then apply if valid. On reject the book is untouched.
    Outcome submit(const EmittedAction& a);

    // State the model conditions on next.
    BookView state() const;

    // Sample a next action index from model logits using this adapter's RNG.
    size_t sample(const std::vector<double>& logits) {
        return sample_index(logits, cfg_, rng_);
    }

    const Book& book() const { return book_; }
    Book&       book()       { return book_; }

    // ---- optional ITCH journal (2026-08-02) --------------------------------
    // Phase 3's LM column needs the generated stream as an ITCH file, because
    // tools/stylized replays ITCH - that is how the real and null columns were
    // built, and the comparison is only honest if all three go through the
    // SAME pipeline. The messages already exist on the marketable path
    // (match_submit emits them, reconstruction-closed and fuzz-verified) but
    // were discarded; resting adds and cancels emit nothing. This records all
    // three, so the journal is the engine's own account of what it did rather
    // than a reconstruction guessing at order refs.
    // OFF by default: every existing caller and the adapter bench are
    // unaffected, and nothing is allocated unless a tool asks for it.
    void journal_enable(uint16_t locate, const itch::Stock& stock) {
        journal_on_ = true;
        journal_locate_ = locate;
        journal_stock_ = stock;
    }
    // Timestamp stamped on messages from the next submit onward. The caller
    // owns the clock (tools/lm_sim advances it by the tuple's DT token).
    void journal_time(uint64_t ns) { journal_ts_ = ns; }
    const std::vector<itch::Message>& journal() const { return journal_; }
    void journal_clear() { journal_.clear(); }
    uint64_t applied()  const { return applied_; }
    uint64_t rejected() const { return total_rejected_; }
    uint64_t reject_count(Reject r) const {
        return reject_[static_cast<size_t>(r)];
    }

private:
    // Resolve the FIFO-head resting order at (side, price); 0 if none.
    uint64_t head_ref_at(Side side, uint32_t price) const;

    AdapterConfig cfg_;
    Book          book_;
    std::mt19937_64 rng_;
    uint64_t next_ref_  = 1;
    uint64_t match_seq_ = 0;
    bool     journal_on_ = false;
    uint16_t journal_locate_ = 0;
    itch::Stock journal_stock_ = {' ',' ',' ',' ',' ',' ',' ',' '};
    uint64_t journal_ts_ = 0;
    std::vector<itch::Message> journal_;
    std::vector<Fill>          fills_;   // reused scratch
    std::vector<itch::Message> emit_;    // reused scratch (unused output)
    uint64_t applied_        = 0;
    uint64_t total_rejected_ = 0;
    std::array<uint64_t, static_cast<size_t>(Reject::kCount)> reject_{};
};

}  // namespace lob
