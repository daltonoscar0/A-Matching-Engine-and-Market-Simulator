# BX Order-Flow Benchmark

A benchmark for generative models of limit-order-book flow, built on real
NASDAQ BX ITCH 5.0 data, with a correct exchange engine, a calibrated
memoryless floor, a pre-registered scoring rule, and two sealed held-out days.

**Framing note, dated 2026-08-03 and kept deliberately visible.** This repo was
built to answer a modelling question — can a language model trained on real
order flow reproduce real market statistics? The model it produced FAILED (see
"Standings"). The benchmark framing was adopted AFTER that result. Nothing
about the data, bars, thresholds, scoring rule, or measurements was changed in
the reframe: the failing entry stays in the standings, and the sealed test set
stays sealed. The reframe is a statement about what the artifact is most useful
for, not a rescue of a result. `docs/CLAIM.md` records the claim as such, and
PLAN.md carries the dated Decision.

---

## The task

Given a real order book warm-started at 09:30, generate a stream of order-flow
events for one symbol that

1. **sustains a live, two-sided book** for a full trading day, and
2. **reproduces the statistical signature** of real BX flow better than a
   memoryless generator does.

Events are factored 5-tuples `[TYPE][SIDE][PRICE_OFF][SIZE][DT]` over a 52-id
vocabulary. `PRICE_OFF` is a signed *occupied-level index* on the event's side,
not an absolute price — so a model never has to learn a price grid, and order
reference identity is dropped entirely (the engine owns refs).

---

## Data

Nine real NASDAQ BX ITCH 5.0 days, split mechanically in `src/dataset.hpp`, not
by convention:

| split | days | use |
|---|---|---|
| TRAIN | 20190130, 20190327, 20190530, 20190830, 20191030, 20191230 | fit anything |
| VAL | 20190730 | tune anything — sampling params, hyperparameters |
| TEST | 20181228, 20200130 | **sealed**, one read, ever |

`dataset::enforce()` is called by every day-reading binary and refuses TEST
without `--i-am-running-the-final-comparison`. TEST spans a stress regime
(20181228, Q4-2018 selloff, 4.6x volume) and a calm forward-in-time day
(20200130), deliberately.

**TEST has never been read.** It remains available.

---

## Evaluation is two stages

### Stage 1 — VIABILITY (entry gate)

Does the generated stream produce a book that stays alive and two-sided? A
model that fails here cannot be scored at all: stylized facts computed on a
one-sided or dead book measure the harness, not the model.

| criterion | bar |
|---|---|
| **V1** | >= 500 collapsed aggressor signs |
| **V2** | book two-sided at >= 90% of checkpoints |
| **V3** | book alive through the 500th sign |

**Streams MUST be measured at day scale — >= ~1.07M tokens (~214k tuples).**
This is not a formality. Short streams systematically flatter models: at 50k
tokens the failing entry below produces 60 signs against real data's 73, nearly
indistinguishable, and its book is 100% two-sided. By day scale it is at 6.4%.
Every viability number recorded in this project before 2026-08-03 was taken too
short and was, in retrospect, measuring nothing. Do not report viability at
100k tokens.

### Stage 2 — SCORING (only for entries that pass Stage 1)

**Scored fact: flow-sign memory** — per-symbol ACF(1) at 1 ms collapse, and the
log-log decay slope over lags 1-100.

An entry BEATS the null on a symbol iff its cross-seed median clears the null's
`[q10, q90]` seed envelope on the real side AND clears an absolute floor that
noise cannot:

    ACF(1) > q90  AND  ACF(1) >= 0.10
    slope  < q10  AND  slope  <= -0.30

Verdicts are a three-way partition, per fact per TEST day:

- **PASS** — beats on >= 2/3 of that fact's measurable symbols
- **FAIL** — inside the envelope on >= 1/2 of measurable symbols
- **INCONCLUSIVE** — neither; reported conservatively as "not shown to beat the
  null", never as a partial pass

**Measurability** is applied identically to every column: flow memory is scored
only where a symbol-day has >= 500 collapsed signs. A cell unmeasurable on
either the entry or the real column is excluded — no credit, no penalty. 19 of
49 symbols are unmeasurable for flow memory on this venue.

**Seeds:** exactly 7 for the entry, 7 for the null. Statistic is the cross-seed
median; seed spread is estimator noise and is used only to build envelopes.

**Sanity checks that earn nothing** — the memoryless null already reproduces
all of them, so passing is not evidence: fat-tail kurtosis level,
aggregational-Gaussianity decay shape, Hill tail index, bid-ask bounce sign.
Gross failure of one is a basic-validity problem noted separately.

The full pre-registration, including the falsifiable failure sentence, is in
PLAN.md (Decision 2026-07-30 and its amendment). It was written and committed
before any model existed.

---

## Standings

**Viability (Stage 1), measured at day scale, warm-started from a real book:**

| entry | applied | two-sided | signs | verdict |
|---|---|---|---|---|
| Real BX flow, VAL 20190730 | 99.77% | 100.0% | 1,844 | **passes** (reference) |
| Real BX flow, TRAIN 20191230 | 99.90% | 100.0% | 4,137 | **passes** (reference) |
| `budget32k_v2` — 4-layer, d=64, n_ctx=320, 227k params, 32k steps | 50.77% | 6.4% | 536 | **NOT VIABLE 8/8** |

The real-data rows are the reference that makes the bar credible: the same
harness, the same warm start, the same criteria. **The bar is reachable.**

**Scoring (Stage 2):** no entry has reached it. The null column is built and
waiting (CST, 7 seeds, TRAIN-calibrated); the real column is built for
TRAIN+VAL; TEST is unspent.

### Notes on the failing entry

Worth reading before submitting anything, because the failure is specific:

- It generates enough **activity** — V1 passes for the first time in the
  project's history (536 signs) — but cannot hold a **two-sided book**.
- The failure **worsens monotonically with length**: 100.0% -> 69.0% -> 34.5%
  -> 23.0% -> 17.2% -> 6.4% two-sided across five and a half doublings.
- Four candidate causes are **eliminated by measurement**: not the harness
  (real data passes), not book drift alone (fresh-book slice replay doesn't
  rescue most slices), not the TYPE mix (matches real; best and worst slices
  indistinguishable), not the PRICE_OFF marginal (the model sits closer to its
  TRAIN pool than the median real day does, and the most touch-concentrated
  real day is fully viable).
- What remains, unmeasured: **conditional structure** — which action at which
  level *given the current book*.
- It was trained on a deliberately small budget (~90 min). The result is
  evidence about that budget and this generation path, **not** about what the
  architecture can do.

---

## Running an entry

Build (zero external dependencies beyond a vendored Catch2):

    cmake -B build && cmake --build build -j

Tokenize a day to the benchmark format:

    ./build/itch_tokenize data/20190130.BX_ITCH_50 --ticker SPY \
        --manifest out/tokens/manifest.json -o SPY_20190130.tokens.bin

Make a warm-start book snapshot at 09:30:

    ./build/warm_book data/20190730.BX_ITCH_50 --ticker SPY \
        -o SPY_20190730_0930.book

Generate a stream from your model, then check Stage 1:

    ./build/sim_health mystream.tokens.bin \
        --manifest out/tokens/manifest.json --ticker SPY \
        --warm-start SPY_20190730_0930.book --why

`--why` reports which rule rejected each unapplied tuple.

If viable, convert tokens to an ITCH stream and score it:

    ./build/lm_sim mystream.tokens.bin --manifest out/tokens/manifest.json \
        --ticker SPY --warm-start SPY_20190730_0930.book --out mystream.itch
    ./build/stylized mystream.itch out/mystream --top 20

Compare against the null:

    ./build/cst_sim null_seed0.itch 0 out/cstnull/calib_*.csv
    ./build/stylized null_seed0.itch out/null_seed0 --top 20

A reference sampler with a KV cache is in `pylm/kvcache.py` and
`out/tokens/sample.py`. Note that on models this small, **CPU beats MPS by
~10x** — the model is far too small to amortise GPU dispatch.

---

## What the harness guarantees

- **The engine is correct on real data.** Two full BX days replay with zero
  rejects, exact share conservation, and every book draining to zero at the
  close (23.8M and 109.7M book messages). Any reject on real data is treated as
  a bug in this repo, never tolerated.
- **Generated streams are held to the same bar.** Every synthetic stream
  self-verifies through the reconstruction path — zero rejects, audits, exact
  conservation — before it is scored.
- **Invariants are fuzz-tested at depth.** 1M-message randomized runs check no
  crossed book, FIFO within level, share conservation, and unknown/duplicate-id
  rejection after every message, cross-checked against a shadow book.
- **The measurement apparatus refuses degenerate statistics.** A symbol-day's
  tick ACF is marked UNMEASURED when its ten largest terms carry >= 50% of the
  variance, so an outlier-placement artifact cannot be reported as a result.

---

## Known limitations

Read these before drawing conclusions from any number here.

- **Venue.** NASDAQ BX is a thin minority venue whose top-of-book can sit
  several ticks behind the NBBO (SPY median spread 4c on BX vs a penny
  consolidated). The supportable claim is about **BX order flow**, never
  "market microstructure".
- **The split is not exchangeable on PRICE_OFF.** Leave-one-out TV across the 7
  TRAIN+VAL days runs 0.082-0.288 (median 0.152), and VAL's PX+0 (0.254) lies
  outside the range of all six TRAIN days (0.315-0.558). A single day's
  marginal can sit 0.29 from the pool it came from. **Unresolved** — it should
  be settled before the seal is broken.
- **The pipeline cannot carry volatility clustering.** Tick-time
  volatility-clustering persistence was ratified as a second scored fact and
  then demoted, because the generation path destroys the statistic (tick ACF
  0.503 -> -0.000) and the ablation built to explain why is degenerate by this
  repo's own measurability rule. It is a limitation of the apparatus, not a
  fact any model failed.
- **`PRICE_OFF` has known resolution loss.** It cannot express "open a new
  interior level" (9.40% of real SPY adds), and 20.9% of events price inside a
  wide BX spread and collapse into a single `-1` bucket. Both are accepted,
  measured limitations of the 52-id vocabulary.
- **Replace atomicity is unenforceable.** ITCH `U` is expanded to Delete+Add,
  so a model can emit unpaired halves that real flow never contains. The cost
  is confined to cancel/replace timing, a sanity-check quantity.
- **No external orderbook reference.** Everything distributional rests on this
  repo's own reconstruction. Only 20190730 has field-level external validation
  (round-lot structure, whole-penny prices, two-sided top-5 books all session);
  no LOBSTER-style reference book diff exists for any day.
- **One venue, nine days, one symbol panel.** Day-to-day spread is large (1s
  kurtosis day-medians 18.7 to 1655). Targets are ranges, not levels.
