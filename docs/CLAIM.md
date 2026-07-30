# CLAIM.md — what this project claims (2026-07-30)

One page, written before any LM exists, so the narrow defensible version is
on record rather than reconstructed from memory later. Every qualifier is
cited to the RESULTS.md row (or PLAN.md Decision) that backs it. Nothing
here is a promise about the LM; there is no LM column.

## The claim, fully qualified

**A matching engine + reconstruction that is provably correct on real
data, plus a pre-registered measurement apparatus that can tell a
memory-bearing generator of NASDAQ BX order flow from a memoryless one,
on the symbols where that distinction is measurable.**

Scope qualifiers, each load-bearing:

- **Venue: NASDAQ BX only** — a thin minority venue whose top-of-book can
  sit several ticks behind the NBBO (SPY median spread 4c on BX vs a penny
  consolidated). The supportable claim is "BX order flow", never "market
  microstructure" (RESULTS.md "Stylized facts - real column (Phase 3)"
  preamble: "a first baseline, not validated empirical ground truth";
  PLAN.md Decision 2026-07-30 Step 1 "orderflow-lm TRAINS ON BX FLOW").
- **Days: 9 total** — 6 TRAIN + 1 VAL (20190730) for the real baseline (7
  days, cross-day medians with per-day spread; RESULTS.md "Multi-day
  'real' column" + v2 section), 2 TEST (20181228, 20200130) sealed behind
  dataset::enforce() until the one final comparison (RESULTS.md dataset-
  split row). Day-to-day spread is large (1s kurtosis day-medians 18.7 to
  1655), so targets are ranges, not levels.
- **Symbols: per-symbol scoring on a 13-symbol panel** (top-20 on >= 4 of
  6 TRAIN days), with a pre-registered measurability rule applied
  identically to all columns: flow memory needs >= 500 collapsed signs
  (measurable for 30/49 symbols; 19 unmeasurable on this venue);
  vol-clustering needs vartop10 < 0.5 and n_tick >= 1000 (49/140
  symbol-days unmeasured). Unmeasurable cells earn no credit and no
  penalty (RESULTS.md v3 per-symbol section; "Effective N" row).

## Scored facts (2) and why the other four were demoted

Scored, per the pre-registration (PLAN.md c06df11 + its amendment; both
survive their kill tests against the CST null):

1. **Flow-sign memory** — per-symbol ACF(1) at 1ms collapse + log-log
   decay slope over lags 1-100. Real slope -0.592 [-0.793 .. -0.432] vs
   null -0.018 [-0.229 .. 0.003]; ACF(1) real 0.277 vs null ~0
   (RESULTS.md null-column section). The lag-1 LEVEL is window-conditional
   (no collapse-window plateau; "collapse sensitivity" row), so the slope
   and sign carry the load. Lag-1000 (Lillo-Farmer) is unmeasurable on BX
   (max 2945 signs/symbol-day).
2. **Tick-time volatility-clustering persistence** — tick ACF(|r|) at lags
   10 and 100 (lag 50 reported unscored), ratified as the second scored
   fact after the vartop10 n-confound resolved (RESULTS.md "Is vartop10
   confounded by sample size?": no n gap, rho -0.08). At lag 100 real and
   null are DISJOINT (12/12 real >= 0.015 vs 13/13 null <= 0.007); at lag
   10 they overlap, and the pre-registered prediction is that lag 10 does
   NOT separate (RESULTS.md null-column CORRECTION; PLAN.md ratification
   Decision).

Demoted to sanity checks — the memoryless CST null, calibrated on TRAIN
only, already reproduces all four, so passing them earns an LM nothing
(RESULTS.md null-column section): fat-tail kurtosis level (null 246 inside
real day-range), aggregational-Gaussianity decay shape (monotone on all 7
null seeds), Hill tail index (overlapping, including the alpha<1 failure
mode), bounce sign (ACF(r) lag-1 negative comes free from book mechanics).
Also on the sanity list: the replace-atomicity rate (PLAN.md U-fork
Decision; U = 5.9% of book messages, measured, PLAN.md "U fork,
quantified").

## What the null establishes

A fair floor: a memoryless Poisson book with empirical per-bucket
intensities and sizes, TRAIN-calibrated, run through the identical
pipeline, self-verified to the same zero-reject bar as real data. Any LM
must beat its [q10, q90] seed envelope per symbol per fact; the
falsifiable failure sentence is in the pre-registration amendment
(PLAN.md). The null cannot fake flow-sign memory or lag-100 persistence —
that is what makes them scoring facts.

## Done / not done

Done:
- **Exchange engine** — external validation on real data: two full BX days
  replayed with ZERO rejects, exact share conservation, full drain
  (23.8M and 109.7M book msgs; RESULTS.md replay rows); field-level
  external checks on 20190730 (round-lot structure 91.5%, 99.4%
  whole-penny prices, two-sided top-5 books all session; "Step 1 external
  validation" row). Match path + adapter loop mutation-verified.
- **Measurement apparatus** — stylized pipeline with pre-registered
  scoring rule, measurability rule, per-symbol structure, and seal
  protocol (TEST read exactly once). Internal consistency plus the
  external field checks above; the stylized numbers themselves are
  measurements on one venue, not validated ground truth.
- **Null column** — complete (7 seeds, TRAIN-calibrated, RESULTS.md).

Not done:
- **The LM column does not exist.** No model has been trained on BX; no
  generative result of any kind is claimed. The tokenizer-facing ingest,
  BX-refit bins, and token->action shim are build items (PLAN.md).
- **n_ctx gate**: the planned 320-token context cannot express the scored
  flow-memory fact (median 294.5x short; RESULTS.md "Context-length
  gate"). Architecture decision pending (user's).
- **Untested externally**: everything distributional rests on this repo's
  reconstruction; only 20190730 has field-level external validation, and
  no LOBSTER-style reference book diff exists for any day.
