# CLAIM.md, what this project does and does not claim

Updated 2026-08-03, at the end of the project. An earlier version was written
2026-07-30, deliberately before any model existed, so the narrow defensible
version was on record rather than reconstructed afterwards; it is preserved in
git. **No scope qualifier, threshold, or scoring rule was ever relaxed after
seeing a result.** The only change made in light of results is a demotion,
which narrows the claim.

## The honest summary

**The project failed at what it set out to do.** It aimed to show a language
model could generate order flow reproducing real market statistics. The model
does not sustain a live, two-sided book, so there is no generative result to
compare against anything.

What survives is the apparatus built to test it, and a well-characterised
account of the failure.

## What is claimed

1. **A limit-order-book engine and reconstruction that is correct on real
   data.** Two full NASDAQ BX ITCH days replayed, 23.8M and 109.7M book
   messages, with zero rejects, exact share conservation, and every book
   draining to zero at the close. Invariants (no crossed book, FIFO within
   level, conservation, unknown/duplicate-id rejection) fuzz-tested at 1M
   messages against an independent shadow book. The matching path and adapter
   loop are mutation-verified.

2. **A measurement apparatus that can distinguish memory-bearing order flow
   from memoryless order flow**, on the symbols where that distinction is
   measurable. Flow-sign memory separates real data (slope −0.592, range
   −0.793 to −0.432) from a calibrated memoryless null (−0.018, range −0.229 to
   0.003). The apparatus includes a rule for refusing degenerate estimates,
   which caught two false positives during this project.

3. **A negative generative result.** A 32,000-step factored-token model trained
   on BX training days fails viability on 8 of 8 streams at real-day length,
   with the failure located: it generates sufficient activity but cannot
   maintain a two-sided book, and degrades monotonically with stream length.
   Four candidate causes are eliminated by measurement.

## What is NOT claimed

- **Not that transformers cannot model order flow.** One model, one
  architecture, one deliberately small budget (~90 minutes, 227k parameters,
  n_ctx 320), one generation path. The result is evidence about *this attempt*.
- **Not any positive generative result of any kind.** No stylized facts are
  published for model output. Computing them on a one-sided or dead book would
  measure the harness, not the model.
- **Not "market microstructure".** NASDAQ BX is a thin minority venue whose
  top-of-book can sit several ticks behind the NBBO (SPY median spread 4c on BX
  versus a penny consolidated). The supportable scope is *BX order flow*.
- **Not externally validated distributionally.** Everything rests on this
  repo's own reconstruction. Only one day has field-level external checks; no
  independent reference orderbook exists for any day.

## Scope qualifiers, each load-bearing

- **Venue:** NASDAQ BX only.
- **Data:** 9 days, 6 train, 1 validation, 2 test. Day-to-day spread is large
  (1s kurtosis day-medians span 18.7 to 1655), so targets are ranges, not
  levels.
- **Symbols:** per-symbol scoring on a 13-symbol panel, with a measurability
  rule applied identically to every column. 19 of 49 symbols cannot support the
  flow-memory measurement at all on this venue.
- **Model:** one checkpoint, one budget, chosen deliberately and in advance.

## The scored facts

**Flow-sign memory**, per-symbol ACF(1) at 1 ms collapse plus the log-log
decay slope over lags 1-100. Survives its kill test against the null and
remains the scoring fact of record. **Never exercised against a model**,
because no model produced a viable stream.

**Tick-time volatility-clustering persistence, DEMOTED** from scored fact to
documented limitation. It was ratified on 2026-07-30, soundly on the evidence
then available. What was not known: the generation pipeline destroys the
statistic (tick ACF 0.503 -> −0.000), and the ablation built to explain why is
degenerate by this project's own measurability rule. **It could not have been
scored even with a working model.** This is a limitation of the apparatus,
discovered late, not a fact any model failed. Demoting it narrows the claim
and rescues nothing.

Four other facts were demoted to sanity checks from the start, because the
memoryless null already reproduces them: fat-tail kurtosis level,
aggregational-Gaussianity decay shape, Hill tail index, bounce sign. Passing
them earns nothing.

## The test set was never read

20181228 and 20200130 have not been looked at. The sealed run was authorised,
and I was asked to proceed; the prior gate in that same decision fired instead
- no viable stream means no model column, and the one-shot read would have
produced a table that cannot answer the question it was designed for.

The seal is intact, so the held-out days remain available to a future attempt.
That is worth more than a table.

## Known limitations

- **The pipeline cannot carry volatility clustering** (above).
- **The day split is not exchangeable on price placement.** Leave-one-out
  distance across the 7 train+validation days runs 0.082-0.288, and the
  validation day sits outside the range of all six training days. A single
  day's marginal can sit 0.29 from the pool it came from. **Unresolved**, it
  should be settled before the sealed days are spent.
- **Known resolution loss in the token format.** It cannot express "open a new
  interior price level" (9.40% of real adds), and 20.9% of events price inside
  a wide spread and collapse into one bucket. Both accepted and measured.
- **Replace atomicity is unenforceable**, ITCH `U` is expanded to Delete+Add,
  so a model can emit unpaired halves real flow never contains.
- **All viability numbers recorded before 2026-08-03 are invalid**, having been
  taken at stream lengths too short to discriminate. See `docs/POSTMORTEM.md`.
