# Format reconciliation: LOBSTER tokenizer vs NASDAQ BX ITCH 5.0

Status: AUDIT ONLY (2026-07-30). No tokenizer code is changed by this
document. The deliverable is the per-item scope verdict so the direction can
be chosen deliberately, not discovered mid-implementation.

## Why this exists

`orderflow-lm` (parser, book reconstruction, factored tokenizer) and its
Scalpel-style FST contract were built against **LOBSTER**: CSV rows, LOBSTER
event codes 1-7, an integer-cents price field, a companion orderbook file for
book state, visible-book-only semantics. The `exchange` repo is now committed
to **NASDAQ BX ITCH 5.0**: a binary feed, 7 book message types
(A/F/E/C/X/D/U), 64-bit order references, prices as `uint32` at 1e-4 dollars,
48-bit-ns timestamps, and a `U` replace with no LOBSTER equivalent. The LM's
input representation is therefore aimed at a format we no longer produce.

The headline finding, stated first: **the factored tokenizer scheme survives
the move with no vocabulary redesign.** Its two most format-sensitive choices
already insulate it — price is a level *index* (not an absolute price or a
fixed grid), and order-reference identity is *dropped* — which are exactly the
two places ITCH diverges hardest from LOBSTER. The real work is (1) a new
ITCH-driving adapter that feeds `ApproxEvent`s from the exchange's validated
reconstruction, (2) refitting the frozen SIZE/DT bins on BX TRAIN data (a
retraining decision, not a code change), and (3) one genuine design fork on
how `U` is represented.

## What the tokenizer represents today

Factored, one event -> five tokens from a single flat 52-id vocabulary with
disjoint ID ranges per field: `[TYPE][SIDE][PRICE_OFF][SIZE][DT]`. Full vocab
(`data/tokens/manifest.json`, `include/orderflow/tokenizer.hpp`):

| field | ids | encoding |
|---|---|---|
| specials | 0-6 | UNK, BOS, EOS, SESSION_OPEN, SESSION_CLOSE, HALT, RESUME |
| TYPE | 7-12 | `TYPE_BASE + (MsgType-1)`, MsgType 1..6 = Add, PartialCancel, Delete, ExecVisible, ExecHidden, CrossTrade |
| SIDE | 13-14 | bid / ask (side of the *standing* order) |
| PRICE_OFF | 15-27 | signed occupied-level index on the event's side: 15 = `-1` (inside spread), 16 = at same-side best (`+0`), 17..26 = `+1..+10`, 27 = tail `> +10`; **UNK (0)** when the same side has no occupied level |
| SIZE | 28-35 | 8 per-ticker quantile buckets, edges frozen from the TRAIN split |
| DT | 36-51 | inter-arrival delta-t: 36 = `dt<=0` (shared-timestamp bursts), 37..50 = 14 log-spaced buckets, 51 = tail |

Contract (mirrors Scalpel): encode is a pure application of frozen bins,
decode emits a representative event, `roundtrip_ok()` (tokens -> approx event
-> same tokens) is the shared inverse check. Order-reference identity is
explicitly dropped in v1 — two events with equal factored fields are
indistinguishable, so cancels/executions are not linked to the add they
consume. Book state for PRICE_OFF comes from LOBSTER's orderbook file
(SPEC Decision A), NOT from reconstruction, for LOBSTER data.

## Per-message-type mapping (LOBSTER <-> ITCH)

| LOBSTER (MsgType) | ITCH | relationship | verdict |
|---|---|---|---|
| Add (1) | `A` | direct; both are a new visible limit order | **adapter bridges** (parse A -> Add event) |
| Add (1) | `F` (add + MPID attribution) | `F` = `A` plus a 4-char market-participant id LOBSTER has no field for | **adapter bridges**: map `F` -> Add, drop attribution (already outside the vocab). Preserving the attributed/unattributed distinction would be a *new TYPE token* + retrain — recommend against. |
| PartialCancel (2) | `X` (partial cancel) | direct size reduction | **adapter bridges** |
| Delete (3) | `D` (full delete) | direct full removal | **adapter bridges** |
| ExecVisible (4) | `E` (exec by ref) | direct execution against a resting displayed order | **adapter bridges** |
| ExecVisible (4) | `C` (exec with price, printable flag) | `C` = execution at a price *different from display*. In the factored vocab price is a level index, not an absolute, so `C`'s explicit exec price has no token to land in and collapses to the same PRICE_OFF as `E` | **adapter bridges** (map `C` -> ExecVisible; the printable/price detail is already dropped by the factored scheme). The exchange's own engine never *emits* `C`, but real BX days *carry* it, so ingest must accept it. |
| ExecHidden (5) | `P` (non-cross trade, non-displayed) | both are executions that do not touch the visible book | **adapter bridges**: the exchange already SKIPS `P` (book-unaffected); to emit an ExecHidden token, surface the skip instead of dropping it. Optional. |
| CrossTrade (6) | `Q` (cross trade) | auction / cross print | **adapter bridges**, but MOOT on BX: the panel days carry **zero** `Q` frames. |
| Halt (7) | `H` (trading action) | halt/resume indicator; book-unaffected | **adapter bridges**: HALT/RESUME are specials, not tuples. The exchange currently SKIPS `H`; to tokenize halts, stop skipping and forward the action code. |
| — | **`U` (replace)** | **no LOBSTER equivalent.** ITCH sends one message carrying `orig_ref` + `new_ref` + new size/price; LOBSTER represents the same economic act as two rows (Delete then Add) | **DESIGN FORK — see below.** |

### The `U` fork (the one real representation decision)

ITCH `U` has no LOBSTER analogue and no token slot. Two options:

- **(recommended) Expand in the adapter to Delete + Add**, matching both
  LOBSTER's own representation and the exchange's reconstruction semantics
  (`U` = cancel remainder + add at back of queue, PLAN.md 2026-07-27). The
  TYPE vocabulary needs **no new token**; one `U` becomes two events sharing a
  timestamp (so the second gets `DT_ZERO`). This keeps any future
  BX-trained vocab identical in shape to the LOBSTER one and is the lower-risk
  path.
- **(not recommended) Add a `TYPE_REPLACE` token.** Cleaner provenance, but it
  is a vocabulary change (breaks the frozen 52-id manifest, bumps
  `kBinVersion`) and forces retraining. Only worth it if a downstream probe
  needs to see replaces as atomic.

Either way this is an **adapter/representation choice**, not a parsing
obstacle. Recommend expansion; flagged here so it is chosen, not defaulted.

## Per-item audit (the five things asked about)

### 1. Prices — **unchanged scheme; input source changes; bins re-measured**

The tokenizer does **not** tokenize absolute price at all. PRICE_OFF is the
signed *occupied-level index* on the event's side. So the ITCH facts that look
threatening — `uint32` at 1e-4, whole-penny grid, sub-penny on sub-$1 names —
never reach the vocabulary. There is no integer-cents assumption and no fixed
price grid to break (that assumption was *removed* in v2, which replaced tick
offsets with level indices precisely because tick offsets are
spread-regime-dependent). `tick_size` survives only as unused metadata
(100 = whole cents, which matches BX).

Caveats, both re-measurement not redesign:
- The level index needs a book at each event. For LOBSTER that came from the
  orderbook file; **for ITCH it must come from the exchange's reconstruction**
  (SPEC already anticipated this: "Reconstruction is retained for ITCH, where
  no orderbook file exists"). The exchange exposes exactly what's needed
  (`for_each_level` best-first, `best_bid`/`best_ask`, `Level{price,shares}`).
- The window `-1..+10` and the "-1 is a single bucket because -2/deeper never
  occur" claim were *measured on LOBSTER AAPL/MSFT/SPY*. BX is a thin venue
  whose top-of-book sits several ticks behind the NBBO and whose displayed
  book has large gaps; the occupied-level-index distribution (how much mass
  lands in `PX_TAIL`, whether inside-spread events ever reach `-2`) **must be
  re-measured on BX** before the window is trusted. This is a bin/window refit,
  logged as a retraining decision, not a vocab change.

### 2. Order references — **unchanged (already dropped)**

The tokenizer embeds **no** order identifiers; ref identity is dropped in v1.
So ITCH's 64-bit sparse refs pose no unbounded-vocabulary problem — the
concern the task raises simply does not apply to this design. Refs *are*
needed to link E/C/X/D/U to the standing order they modify, but that linkage
happens in the **exchange's reconstruction**, which already handles 64-bit
refs natively and pointer-stably. Net: refs are consumed by the engine,
invisible to the tokenizer. The cost is the pre-existing v1 limitation
(cancels/execs unlinked in the token stream), unchanged by the format move.

### 3. Timestamps / inter-arrival — **unchanged units; DT bins refit**

DT is a per-event inter-arrival delta, log-bucketed (14 bins + zero + tail),
fit per ticker. LOBSTER `time_ns` and ITCH's 48-bit ns-since-midnight are the
**same unit**, so dt is computed identically. The scheme models inter-arrival
time directly and survives as-is. The DT *edges* were fit on LOBSTER SPY's
arrival-rate regime; BX is thinner and burstier, so the edges **must be refit
on BX TRAIN** (retraining decision). One structural note already handled: the
exchange's merged multi-symbol stream is not timestamp-monotonic *across*
symbols, but the tokenizer is per-symbol and each symbol's substream is
monotonic, so no conflict.

### 4. Size — **unchanged scheme; bins refit**

`uint32` ITCH shares vs `int64` LOBSTER size, same concept. The 8 quantile
buckets are frozen from TRAIN; BX has strong round-lot structure (48.5%
exactly 100, 91.5% multiples of 100 — RESULTS.md) very different from SPY's
edges, so the SIZE edges **must be refit on BX TRAIN** (retraining decision,
not a code change).

### 5. Specials / boundaries — **mostly unchanged**

BOS/EOS/SESSION_OPEN/SESSION_CLOSE are format-agnostic. HALT/RESUME require
the ingest to stop skipping ITCH `H` and forward the action code (optional).
No cross-day boundaries either way.

## Scope verdict

| item | verdict |
|---|---|
| PRICE_OFF vocabulary & scheme | **unchanged** (level-indexed, format-agnostic) |
| PRICE_OFF window / `-1`-only assumption | **re-measure on BX** (retraining decision) |
| Order-reference handling | **unchanged** (dropped by tokenizer; engine owns 64-bit refs) |
| Timestamp/DT scheme | **unchanged** (same ns unit) |
| DT bin edges | **refit on BX TRAIN** (retraining decision) |
| SIZE scheme | **unchanged** |
| SIZE bin edges | **refit on BX TRAIN** (retraining decision) |
| TYPE vocab for A/X/D/E/C/P/Q/H | **adapter bridges** (no new token) |
| TYPE vocab for `U` | **design fork**: expand to Delete+Add (recommended, no vocab change) OR add `TYPE_REPLACE` (vocab change + retrain) |
| Book-state source for PRICE_OFF | **new adapter**: feed exchange reconstruction instead of a LOBSTER orderbook file |
| ITCH binary parse -> `ApproxEvent` | **new code** (parser/adapter), NOT a tokenizer rewrite |
| Any model already trained on LOBSTER-SPY | **does not transfer**: different venue, refit bins, single-ticker SPY corpus — a BX corpus is a fresh training run |

**Does the tokenizer survive? Yes — no redesign.** The bounded scope is: an
ITCH-driving adapter (parse BX -> drive reconstruction -> emit
`[TYPE][SIDE][PRICE_OFF][SIZE][DT]` with PRICE_OFF from the reconstructed
book, `U` expanded to Delete+Add), plus a refit of the frozen SIZE/DT bins and
a re-measurement of the PRICE_OFF window on BX TRAIN. No vocabulary redesign
is required unless the `U` fork is decided toward an atomic `TYPE_REPLACE`
token. The one thing that genuinely does not carry over is the *trained
weights* and *frozen bins*: BX is a different distribution, so Phase 3's LM
column is a from-scratch BX training run, not a transfer of the LOBSTER model.
