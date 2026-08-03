# CLAIM.md — what this project claims (updated 2026-08-03)

The original was written 2026-07-30, deliberately BEFORE any LM existed, so
the narrow defensible version was on record rather than reconstructed from
memory afterwards. That version is preserved in git (commit c118506 and
earlier). **Nothing in the scope qualifiers or the scoring rule was relaxed
after seeing a result** — the one change made in light of results is a
DEMOTION (scored fact 2, below), which makes the claim narrower, not wider.

Every qualifier is cited to the RESULTS.md row or PLAN.md Decision behind it.

**On 2026-08-03 the project was also reframed as a benchmark**
(`docs/BENCHMARK.md`), AFTER the negative result below. That reframe changed no
threshold, bar, scored fact, measurability rule, or measured number, and the
failing entry appears in the benchmark's own standings table. This document
remains the claim record; BENCHMARK.md is the front door for anyone wanting to
run an entry.

## The claim, fully qualified

**A matching engine and order-book reconstruction that is provably correct on
real NASDAQ BX data; a pre-registered measurement apparatus that can tell a
memory-bearing generator of BX order flow from a memoryless one on the symbols
where that distinction is measurable; and a NEGATIVE generative result — a
32,000-step factored-token LM trained on BX TRAIN days does not produce a
viable order stream at day scale, with the failure located and four candidate
mechanisms eliminated by measurement.**

Scope qualifiers, each load-bearing:

- **Venue: NASDAQ BX only** — a thin minority venue whose top-of-book can sit
  several ticks behind the NBBO (SPY median spread 4c on BX vs a penny
  consolidated). The supportable claim is "BX order flow", never "market
  microstructure" (RESULTS.md "Stylized facts - real column (Phase 3)";
  PLAN.md Decision 2026-07-30 Step 1).
- **Days: 9 total** — 6 TRAIN + 1 VAL (20190730) for the real baseline, 2 TEST
  (20181228, 20200130) **still sealed and never read** (below). Day-to-day
  spread is large (1s kurtosis day-medians 18.7 to 1655), so targets are
  ranges, not levels.
- **Symbols: per-symbol scoring on a 13-symbol panel** (top-20 on >= 4 of 6
  TRAIN days), with a pre-registered measurability rule applied identically to
  all columns. Unmeasurable cells earn no credit and no penalty (RESULTS.md v3
  per-symbol section).
- **Model: one checkpoint at one deliberately small budget** — 32,000 steps,
  ~90 minutes, ~1.9 epochs of an 88.4M-token corpus, a budget the user CHOSE
  (PLAN.md 2026-08-02 SCORED-RUN BUDGET). The negative result is evidence
  about what that budget and this generation path buy. **It is not evidence
  that the architecture cannot do better.**

## The generative result (2026-08-03)

**NOT VIABLE, 8 of 8 streams, at real-day length** (1,070,002 tokens x 8
streams, ~214k tuples each; RESULTS.md "FINAL: day-scale viability").

The failure is located rather than general: **the model generates enough
ACTIVITY but cannot maintain a TWO-SIDED book.**

- **V1 (>= 500 collapsed signs) PASSED for the first time in the project's
  history** — s3 at 536 signs, four more streams at 366-454. The bar was never
  unreachable; it had never been TESTED, because it needs ~0.5M+ tokens per
  stream and no run before 2026-08-03 produced them.
- **V2 (two-sided >= 90%) fails 8/8 and WORSENS with length.** Best stream:
  100.0% -> 69.0% -> 34.5% -> 23.0% -> 17.2% -> 6.4% two-sided across five and
  a half doublings of stream length. There is no length at which it passes.
- **V3**: 5/8 alive, 3 books dead outright.

**Controls exonerate the harness.** The real BX token stream through the
identical pipeline IMPROVES with length: VAL 99.77% applied / 100% two-sided /
1,844 signs; TRAIN (20191230) 99.90% / 100% / 4,137 signs, both at day scale.
So the bar is reachable, the repaired shim is validated at day scale, and the
decay belongs to the model.

**A methodological correction that invalidates the project's own earlier
numbers**: every viability verdict recorded before 2026-08-03 was taken at
SHORT stream length, which systematically flatters the model — at 50k tokens
the model produces 60 signs against the real stream's 73, nearly
indistinguishable. Viability is a day-scale measurement or it is not a
measurement.

**Four candidate mechanisms eliminated by measurement**, not argument
(RESULTS.md corrections 1 and 2): not the harness; not book drift alone
(fresh-book slice replay does not rescue most slices); not the TYPE mix
(matches real; best and worst slices indistinguishable); not the PRICE_OFF
marginal (the model is CLOSER to its TRAIN pool, TV 0.107, than the median
real day is, TV 0.152 — and the most touch-concentrated real day in the corpus
is fully viable). What remains, unmeasured: the conditional structure — which
action at which level GIVEN the current book.

Two mechanism claims were asserted and then RETRACTED the same day when the
checks they proposed refuted them; both retractions are in RESULTS.md as new
rows naming the old ones. The eliminations are measurements; the mechanism
stories were not, and should be weighted accordingly.

## Scored facts: one stands, one is DEMOTED

1. **Flow-sign memory** — per-symbol ACF(1) at 1ms collapse + log-log decay
   slope over lags 1-100. Real slope -0.592 [-0.793 .. -0.432] vs null -0.018
   [-0.229 .. 0.003]. Survives its kill test against the CST null and remains
   a legitimate scoring target. **Never exercised against an LM**, because no
   LM produced a viable stream to score.

2. **Tick-time volatility-clustering persistence — DEMOTED from scored fact to
   documented limitation** (PLAN.md Decision 2026-08-03). It was ratified as
   the second scored fact on 2026-07-30 and that ratification was sound on the
   evidence then available. What changed: the generation pipeline DESTROYS the
   statistic (tick ACF 0.503 -> -0.000, RESULTS.md Step 8), and the ablation
   built to find out why was INCONCLUSIVE — both variants are degenerate by
   this project's own measurability rule (vartop10 0.9966 and 0.8462, against
   a >= 0.5 UNMEASURED threshold). So the fact could not have been scored even
   if a viable model had existed. Stated plainly: **this is a limitation of
   the apparatus, discovered late, not a fact the model failed.** Demoting it
   narrows what the project claims; it does not rescue any result.

Demoted to sanity checks from the start — the memoryless CST null already
reproduces all four, so passing them earns nothing: fat-tail kurtosis level,
aggregational-Gaussianity decay shape, Hill tail index, bounce sign. Also on
the sanity list: replace-atomicity rate (U = 5.9% of book messages).

## What the null establishes

A fair floor: a memoryless Poisson book with empirical per-bucket intensities
and sizes, TRAIN-calibrated, run through the identical pipeline, self-verified
to the same zero-reject bar as real data. The null cannot fake flow-sign
memory — that is what makes it a scoring fact. The null column is complete and
was never spent, since no LM reached the comparison.

## TEST was never unsealed

20181228 and 20200130 have not been read. The sealed run was AUTHORISED by the
user (2026-08-02) and the user asked to proceed; the prior gate in that same
Decision fired instead — no viable stream means no LM column, and the one-shot
read would have produced a two-column table that cannot answer the
pre-registered question. **The seal is therefore intact and the TEST days
remain available to a future model**, which is worth more than a table
published tonight.

## Known limitations

- **The split is not exchangeable on PRICE_OFF.** Leave-one-out TV across the
  7 TRAIN+VAL days runs 0.082 to 0.288 (median 0.152); VAL's PX+0 (0.254) lies
  outside the range of all six TRAIN days (0.315-0.558). A single day's
  marginal can sit 0.29 from the pool it was drawn from. The pre-registration
  scores per day and refuses to pool, which is the right instinct, but whether
  that spread is inside the tolerance the scoring rule assumes is UNRESOLVED
  and should be settled before any future seal is broken.
- **The pipeline cannot carry volatility clustering** (scored fact 2 above).
- **Untested externally**: everything distributional rests on this repo's
  reconstruction; only 20190730 has field-level external validation, and no
  LOBSTER-style reference orderbook diff exists for any day.
- **n_ctx gate**: the 320-token context cannot express the scored flow-memory
  fact (median 294.5x short). The model was trained anyway, as a deliberate
  budget choice; this bounds what it could ever have reproduced.

## Done / not done

Done:
- **Exchange engine** — two full BX days replayed with ZERO rejects, exact
  share conservation, full drain (23.8M and 109.7M book msgs). Match path and
  adapter loop mutation-verified.
- **Measurement apparatus** — stylized pipeline with pre-registered scoring
  rule, measurability rule, per-symbol structure, and an intact seal protocol.
- **Null column** — complete, 7 seeds, TRAIN-calibrated.
- **Ingest, tokenizer refit, shim, warm start** — round-trip tested; the shim
  is now validated at day scale by the real-stream control.
- **LM column** — exists and is NEGATIVE. The model was trained, sampled at
  day scale, and measured against a pre-registered bar it fails.

Not done, and not claimed:
- **No headline real-vs-LM-vs-null table.** It requires a viable LM column;
  there is none. TEST stays sealed.
- **No positive generative claim of any kind.** Nothing here says a
  transformer can or cannot reproduce BX order flow — only that THIS model, at
  THIS budget, through THIS generation path, does not sustain a book.
- **Phase 4** (Almgren-Chriss / RL execution agent) — out of scope, never
  started.
