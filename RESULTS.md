# RESULTS - Exchange

| date | config | metric | takeaway |
|---|---|---|---|
| 2026-07-27 | commit 58cf5ce, fuzz seeds 1/2 + 10-17, FUZZ_N=1M, invalid_permille 0/50/100 | 0 invariant violations, 0 wrong accepts/rejects across ~2.16M messages; fuzz book == shadow book at end | Book core + codec hold under randomized valid+hostile streams; correctness gate green. Caveat: generator and book share one dev's assumptions - real LOBSTER replay is the honest test. |
| 2026-07-30 | bench_tail (spike attribution) on synth 5M seed 42, Apple M4, per-message timing + bucket-count tracking | max latency = order-pool unordered_map rehash. All 5 largest spikes land exactly on rehash messages, cost doubles with bucket count (12.8k buckets 160us -> 205k 1.67ms -> 411k 10.3ms). With pool pre-reserved (1M buckets): 0 rehashes, max drops 10.3ms -> ~110-170us, p50/p99/p99.9 unchanged. | The unexplained max tail was rehash, the boring candidate. p99.9 (~1.7us) is NOT rehash - only 18 rehashes in 5M msgs, can't populate the 99.9th percentile; it is ordinary deep-book map work + ~2x clock-read overhead per sample. Residual 50-170us spikes (~top 10 msgs of 5M) sit on add-heavy messages at high open counts: allocator refill/page faults on node alloc, not uniform across the run, so not pure preemption. A slab allocator would chase those but cannot move p99.9; not justified by this measurement. |
| 2026-07-30 | multi-symbol fuzz: MultiGenerator (8 symbols, globally unique refs), BookSet under test, seed 3, FUZZ_N=1M, invalid_permille 50; plus seeds 1/2/10-17 single-book unchanged | 0 invariant violations across all 8 books, 0 wrong accepts/rejects, every book == its generator's shadow at end | stock_locate routing holds under fuzz. Globally-unique refs make misrouting observable (wrong book -> UnknownId -> wrong-reject caught), so this exercises routing, not just per-book invariants. Same caveat as ever: generator and book share assumptions; synthetic stream is a message-ratio stand-in, not a real market. |
| 2026-07-30 | replay_itch on data/20190730.BX_ITCH_50 (real NASDAQ BX ITCH 5.0 day, 837MB, 28.73M frames) through BookSet; ctest slice gate = first 200k book msgs with per-message invariant checks | 23,821,586 book msgs applied, 0 rejects, 0 invariant violations, all 7497 book audits clean. Conservation exact: 8.43B shares added = 79.0M executed + 8.35B canceled + 0 resting, and every book drains to open=0 at end of day. Skip histogram (types we don't implement): N 4.64M, P 244k, H 8.9k, Y 9.0k, R 8.8k, L 5.9k, S 6, V 1 | Phase 1 milestone: genuine exchange data replays CLEAN - no semantics bug surfaced, so the result is a validation, not a fix. The zero-final-open drain is the strongest signal: every one of 10.6M adds was exactly consumed by E/C/X/D/U flow, share-for-share. Two assumptions confirmed by construction: 'P' (non-cross trade) executes against non-displayed liquidity and must be skipped, not applied (244k of them; applying them as executes would have produced an UnknownId flood), and reject-on-cross never fired on a real venue's displayed feed. Caveats: BX is a thin venue (peak 270 open orders per book, 87 levels), so deep-book behavior is still only covered by the synthetic stress rows; and with no LOBSTER-style reference orderbook file the check is internal consistency (rejects/invariants/conservation/drain), not an external state diff. |
| 2026-07-30 | Step 1 external validation: tools/field_sanity + tools/bbo_trace on data/20190730.BX_ITCH_50 (decode-offset sanity + market-reproduction check; conservation alone cannot catch a wrong offset because the same wrong field is added and removed) | SHARES on 10.63M A/F: hard spike at 100 (48.5%), 91.5% exact multiples of 100, 6.75% odd-lot tail, median 100, p90 1000, p99 7000, max 350k, mean 622. PRICES: 99.4% whole-penny, 0 zeros, p1 $2.04 / median $48.31 / p99 $301.12, max $4090 (sub-penny only on sub-$1 names, 0.61% of adds). The earlier "~795 shares/order" reconciles: 8.43B added includes 1.81B re-added by 2.05M U-replaces; per add-event mean is 665 vs median 100 - a large-order tail, not a decode artifact. BBO: top-5 by book msgs = SPY 260k, IWM 238k, IWO 206k, AAPL 191k, SOXL 189k; all five two-sided at 100% of 391 60s samples 09:30-16:00, worst one-sided stretch 0 min, median spreads SPY $0.04 / IWM $0.03 / IWO $0.12 / AAPL $0.07 / SOXL $0.21. Wide-spread samples cluster at 09:30 (book populating), 16:00 (liquidity pulled), plus midday thin-touch stretches on AAPL (bid stuck at a stale $204 level, 3.6% of samples >$0.50) | The reconstruction now has EXTERNAL validation, not just internal consistency: the shares field shows exactly the round-lot structure of real displayed equity flow (a wrong offset would give arbitrary values while conservation still balanced perfectly), prices are plausible dollar values, and the top-5 books reproduce a real two-sided market with cent-scale spreads all session. The wide-touch tail on AAPL/SOXL is the thin-venue signature (BX top-of-book can sit far from NBBO when its own near quotes cancel), not a reconstruction defect - SPY/IWM stay at 3-4c essentially every sample. CSVs: out/bbo_{SPY,IWM,IWO,AAPL,SOXL}.csv. |
| 2026-07-30 | Step 2 stylized-facts baseline: tools/stylized on data/20190730.BX_ITCH_50, top 20 symbols by book msgs, continuous hours 09:30-16:00, event-time + 1s/10s/60s calendar sampling, methodology per PLAN.md Decisions | Per-fact table in "Stylized facts - real column (Phase 3)" section below; raw series + ACFs in out/stylized_*.csv, cross-symbol stats in out/stylized_summary.csv | Phase 3 "real" column started BEFORE any generative model exists, so the baseline is not under pressure to agree with anything. Matches Cont 2001 on this day: fat tails, aggregational Gaussianity, no linear autocorrelation (with the documented microstructure lag-1 bounce), volatility clustering (slow ACF(\|r\|) decay vs fast ACF(r) decay), and qualitatively the Lillo-Farmer order-flow long memory. Every mismatch has a boring thin-venue explanation before any exotic one (see table). |
| 2026-07-30 | Step 1 (more days): fetched every remaining downloadable BX ITCH day from emi.nasdaq.com into data/ (8 days: 20181228, 20190130, 20190327, 20190530, 20190830, 20191030, 20191230, 20200130; spread over 14 months). Verified with gzip -t (md5 sidecars 404). Pipeline generalization check: full replay_itch on 20181228, a different year and a 4.6x bigger day | All 8 pass gzip -t. 20181228 replay: 123.8M frames = 109,732,869 book msgs (vs 23.8M on 20190730), ZERO rejects, all audits clean, conservation exact (21.90B added = 140.1M executed + 21.76B canceled + 0 resting), every one of 7220 touched books drains to open=0 at close. Skip histogram: same type set as 20190730 (N/P/H/Y/R/L/S/V), again zero 'Q' cross frames | The reconstruction pipeline generalises past the day it was built on: a different year, different volatility regime (Dec 28 2018 = tail of the Q4-2018 selloff, 4.6x the message volume), zero semantic surprises. The training set for orderflow-lm now exists on disk: 9 independent-ish BX days. Decompressed replay copy deleted after the check (3.6GB; the .gz archives stay, data/ gitignored). |
| 2026-07-30 | Step 2 corrections (2a/2b/2c) on the same day/tool: tick-time \|r\| ACF added, Hill alpha<1 reclassified, sign-collapse rule fixed (windowed, default 1ms; --collapse-ns) | See "Addendum: Step 2 statistical corrections" below the stylized table. Headline deltas: volatility clustering partially survives tick time (1s lag-100 median 0.039 -> tick 0.018; 6/20 symbols go to ~0 = pure activity clustering); Hill median 2.59 -> 2.95 after excluding AAPL 0.64 / MSFT 0.68 as estimator failures; flow lag-1 0.42 -> 0.27, now inside Lillo-Farmer 0.2-0.3 | Three reviewer-grade fixes, each making the baseline weaker-but-truer: part of the calendar-grid "volatility clustering" was bursty activity; alpha<1 means infinite mean, an impossible return distribution, so those are broken estimates not low ones; and an exact-timestamp collapse was splitting single sweeps (measured same-sign inter-fill gaps are bimodal: machine mode ~32us-1ms vs decision mass >=100ms, valley 10-32ms). |
| 2026-07-30 | Step 1 follow-up (collapse sensitivity): tools/stylized --collapse-ns swept over {0, 1us, 10us, 100us, 1ms, 10ms, 100ms} on data/20190730.BX_ITCH_50, top 20 symbols; per-symbol ACF(1) + medians in out/collapse_sensitivity.csv; interpretation rule fixed BEFORE running (plateau = principled choice, smooth slide = parameter) | Flow-sign ACF(1) cross-symbol median (symbols with >= 500 signs at each window; the fixed 9-symbol comparable set gives identical values from 100us up): 0.414 / 0.414 / 0.403 / 0.319 / 0.270 / 0.216 / 0.168. Flat ONLY over 0-10us; slides smoothly through the entire machine-scale range 10us-1ms and keeps sliding to 100ms - NO plateau. The log-log decay slope (lags 1-100) is the robust quantity instead: median -0.61 / -0.61 / -0.59 / -0.68 / -0.63 for windows <= 1ms, degrading only at decision-scale windows (-0.53 at 10ms, -0.47 at 100ms) | VERDICT (per the pre-registered rule): the lag-1 LEVEL is a parameter choice, not a measurement. Mechanism is visible in the gap histogram itself: the two timescales overlap (machine mode spans 1us-1ms; the 10-32ms valley is a ~35% dip with ~6.8k pairs per bucket, not empty), so every window inside the overlap splits some sweeps and merges some decisions, moving ACF(1) ~ -0.05 per decade of window. The default STAYS 1ms because the gap histogram justifies it (conservative side of the valley) - explicitly NOT because 0.27 lands inside Lillo-Farmer 0.2-0.3. Honest headline: "ACF(1) = 0.27 at the 1ms window; defensible windows span 0.32-0.17". The 2c row's "robust 100us-10ms (0.32-0.22)" was too generous - a factor-of-2 slide is sensitivity, not robustness. What IS robust, and what a generative model should be scored on: the sign of the autocorrelation and the power-law decay slope ~ -0.6, stable across every machine-scale window. |
| 2026-07-30 | Dataset split made mechanical + enforced: src/dataset.hpp assigns all 9 BX days (TEST = 20181228 + 20200130, VAL = 20190730, TRAIN = the other 6); dataset::enforce() wired into stylized/bbo_trace/field_sanity/gap_hist/replay_itch; tests/test_dataset.cpp pins the constraints and the refusal | Guard verified live: stylized on data/20181228.BX_ITCH_50.gz refuses with exit 3 and names the override (--i-am-running-the-final-comparison); an unassigned day (20250101) refuses even WITH the override. 38 assertions in 6 dataset test cases green | Leakage hazard closed before any training exists: the 9 days were both the intended training set and the Phase 3 "real" column source. 20190730 - the day the entire published stylized table was computed on - landed in VAL (it is the methodology-development day and could never honestly be TEST), so the published numbers are NOT contaminated by held-out data; they are one draw, and the TRAIN+VAL multi-day table below supersedes them as the baseline. Prior TEST exposure on record: 20181228 was replayed once for engine validation (conservation/rejects only, no distributional stats) before the split existed; 20200130 never read past gzip -t. |
| 2026-07-30 | Multi-day "real" column + per-day spread: tools/stylized (defaults: top 20, 1ms collapse) on all 7 TRAIN+VAL days -> out/multiday/<day>/*.csv + per_day_summary.csv; TEST days excluded by the guard | Cross-day medians of cross-symbol medians (day-median range in brackets): kurt 1s 108 [18.7 .. 1655], 10s 18.9 [3.1 .. 85.9], 60s 5.1 [1.3 .. 11.2]; Hill 1s excl failures 2.95 [2.50 .. 3.82], failures 0-4 per day, always vol products / stale-touch names; ACF(r) 1s lag-1 -0.18 [-0.39 .. -0.15]; ACF(\|r\|) 1s lag-10 0.060 [0.047 .. 0.115], lag-100 0.026 [0.018 .. 0.056]; tick lag-10 0.120 [0.077 .. 0.171], lag-100 0.033 [0.015 .. 0.048]; flow-sign ACF(1) at 1ms 0.274 [0.108 .. 0.341]; sign log-log slope -0.56 [-0.63 .. -0.44] | FINDING: the one-day baseline was much weaker than even the written caveats implied, but unevenly so. 1s kurtosis day-medians span TWO ORDERS OF MAGNITUDE (18.7 to 1655) - the level is dominated by a handful of extreme single-second moves per day, some real (20190130 carries the 14:00 FOMC statement: SPY +38bp in 1s at 14:00:10), some venue flaps (same day, 225bp SPY spike at 14:52 reversed within a second) - so a kurtosis LEVEL is nearly meaningless as a scoring target; the aggregational-Gaussianity SHAPE (kurtosis falling monotonically 1s -> 10s -> 60s) holds on all 7 days and is the robust fact. Hill-excl-failures (2.5-3.8, cubic-law band) and vol-clustering tick lag-10 (0.08-0.17, always positive) are stable enough to score against as ranges. The flow-sign slope, previously called "the robust quantity" vs the collapse window, swings -0.44 .. -0.63 ACROSS DAYS - robust to the estimator, not to the draw; sign ACF(1) has one outlier day (20190130, 0.108 vs 0.27-0.34 elsewhere). Model scoring must target cross-day ranges, not any single day's level. 20190730 sits near the middle on most facts - the original baseline was a lucky draw, except its kurtosis (108) which the spread shows was never a stable number. |
| 2026-07-30 | CST null model (src/cst.hpp + tools/cst_calibrate + tools/cst_sim): memoryless Poisson order flow with empirical per-bucket intensities (lambda(side,d), theta(side,d) per order, mu(side), d = ticks from opposite best, 1..100), empirical size draws, executed through Book::match. Calibrated on the 6 TRAIN days, 13-symbol panel (top-20 on >=4/6 TRAIN days); 7 seeded days through the SAME tools/stylized pipeline (1ms collapse, same everything); real column restricted to the identical panel for comparability. Every generated stream self-verifies: reconstruction replay, 0 rejects, exact conservation (3.65M msgs/day). Interpretation rule pre-registered in PLAN.md Decisions BEFORE the run | Flow signs - null: ACF(1) median -0.009 [seed range -0.017 .. 0.000], log-log slope -0.018 [-0.229 .. 0.003]; real (same panel): ACF(1) 0.277 [day range 0.160 .. 0.331], slope -0.592 [-0.793 .. -0.432]. Vol clustering - null: 1s ACF(\|r\|) lag-10 0.005 [0.001 .. 0.009], lag-100 0.000; tick lag-10 0.046, lag-100 0.002; real: 0.070/0.049 (1s), 0.120/0.037 (tick). Null DOES reproduce: 1s kurtosis 246 vs real 163 (inside the real day-range), kurtosis decay shape 246 -> 42.9 -> 7.8 monotone on all 7 seeds, Hill 2.63 [2.39 .. 2.92] vs real 2.34 [2.10 .. 3.59] including alpha<1 estimator failures (QQQ/IJH/IWN), and the negative ACF(r) lag-1 (-0.059 vs -0.162, right sign, ~3x weak). Full table in "null column" section below; per-seed CSVs in out/cstnull/ | VERDICT (per the pre-registered rule): the Lillo-Farmer decay slope DISCRIMINATES - the memoryless null lands at ~0, nowhere near -0.6; the seed spread [-0.23 .. 0.00] is pure estimator noise on ~1k signs (the slope fits log of sampling noise there) and does not overlap the real day range [-0.79 .. -0.43]. The slope stays a legitimate scoring target; the null's floor is recorded as slope >= -0.23 / ACF(1) ~= 0. Volatility clustering discriminates too (disjoint ranges, 1s and tick). The list that reshapes Phase 3: fat-tail LEVELS, the aggregational-Gaussianity SHAPE, the Hill index (failures included), and the SIGN of the bounce come free from book mechanics + empirical sizes - a null with no memory reproduces all four, so none of them can distinguish a trained LM from noise. Phase 3's headline comparison must score on flow-sign memory (level + slope) and volatility clustering; the distributional facts are demoted to sanity checks. |
| 2026-07-30 | Tick-time volatility-clustering split characterised per symbol-day: tools/stylized extended with per-symbol covariates (n_msgs, two-sided fraction, median spread in ticks, tick-relative-to-price, exec count, cv of \|r_tick\|, vartop10 = share of the centered sum of squares of \|r_tick\| in its 10 largest terms), all 7 TRAIN+VAL days rerun; significance = acf10 > 2/sqrt(n_tick); full table in out/multiday/persym_analysis.txt | THE THINNESS HYPOTHESIS FAILS: insignificant tick ACF occurs at n_tick up to 250k (MDY 20190327: n=250,083, acf10=0.003; XLK 20190830: n=138k; IWM 20190730: n=74k, acf10=0.000) while strong clustering is detected at n as low as 11.8k (GDX z=12). Detectability floor 2/sqrt(n) = 0.021 at the panel's smallest n vs typical present-effect 0.16 - there is NO n threshold; n never binds. What DOES separate the zeros: variance concentration. Zero days have vartop10 median 0.98 (16 of 21 have >= 0.70) vs 0.26 for significant days. The 2a six (IWM XLV XLP XLE XLI IWB on 20190730) share exactly this: vartop10 0.93-0.99, tight 2-5 tick spreads, fully two-sided, NOT thin (IWM: 229k msgs, 2nd most active that day). Three of the six (XLE z=7.5, IWB z=4.3, XLI z=3.3) were actually small-but-significant - the 2a "collapse to ~0" eyeball classification mixed degenerate estimates with weak positives. Classification with the pre-stated rule (UNMEASURED if vartop10 >= 0.5): 87 PRESENT / 4 ABSENT / 49 UNMEASURED of 140 symbol-days (cutoff 0.3: 70/3/67; cutoff 0.7: 97/6/37). Measured-absent is rare and boring: OILX twice (an ETN with ZERO executions on this venue - no trading, nothing to cluster), IWB 20191030, MDY 20190327 (borderline at 0.50) | FINDING, and a correction to 2a's framing: "vol clustering survives on 11/20 and vanishes on 6/20" was the wrong split. When the top-10 outliers carry ~all the variance of \|r_tick\|, the ACF is an outlier-placement statistic, not a volatility measurement - a mid pinned at a tight spread moves mostly by identical half-ticks, so its variance IS a handful of stale-touch flaps (the same venue artifact documented for kurtosis). Those symbol-days are UNMEASURED, not negative: "not enough data to tell" and "no volatility clustering" are different claims, and 45 of the 49 zero-or-suspect days are the former. Where the estimator is valid, clustering is present in 87 of 91 symbol-days (96%). Consequence for scoring: beware trusting PRESENT verdicts on vartop10-dominated days too (XLK 20190730 shows acf10=0.175 with vartop10=1.00 - ten points talking to each other); the classifier excludes them from both sides. Phase 3 comparison restructured per-symbol (v3 section below); pooled medians hid all of this. |

| 2026-07-30 | Effective N of the headline comparison, from TRAIN+VAL per-symbol data. Measurable-for-flow = >= 500 collapsed signs; measurable-for-vol-clustering = vartop10 < 0.5 and n_tick >= 1000. Counts = symbols measurable on at least K days | Flow-memory measurable on >= K days (all 49 symbols): K1..7 = 30/20/14/8/6/5/3. Vol-clustering: 42/23/12/6/5/3/0. Symbols measurable for BOTH: 26/14/6/4/4/2/0. On the 13-symbol CST panel: flow 9/9/9/8/6/5/3, vol-clustering 12/10/8/6/5/3/0, BOTH 8/6/5/4/4/2/0. Intersection at K>=3 = {EEM,QQQ,SPY,TLT,UVXY,VXX} (6; 5 in panel: QQQ,SPY,TLT,UVXY,VXX); at K>=5 = {QQQ,SPY,TLT,UVXY} (4, all in panel) | FINDING: the two scoring facts have ANTICORRELATED measurability. Flow memory needs many executions (active names); vol clustering needs a non-degenerate mid (not pinned) - and many active BX names ARE pinned at a tight touch (IWM: flow-measurable all 7 days, vol-clustering-measurable 0 days; SOXL: flow 0, vc 6). So a comparison that demands BOTH facts on the SAME symbol collapses to ~5-6 symbols. But the headline table does not need that intersection: each fact is scored on the symbols where THAT fact is measurable. Per-fact effective N (robust K>=3 days): flow memory 14 symbols overall / 9 in panel; vol-clustering-persistence 12 overall / 8 in panel. RECOMMENDATION for Step 3: score each fact on its own measurable set, not the intersection; the 13-symbol CST panel is the right calibration/comparison universe but the SCORED set per fact is its measurable subset (report which symbols, and count unmeasured-on-both as neither credited nor penalized). Stated plainly: the flow-memory result rests on ~9-14 symbols, not 6; only an ill-posed both-facts-per-symbol requirement would reduce it to 6. |

| 2026-07-30 | Is vartop10 confounded by sample size? (Mechanical worry: ten points are a smaller fraction of a bigger series, so larger n could lower vartop10 arithmetically, and the measurability filter could then admit null symbol-days it would reject at real-sized n.) 1a: median n_tick per CST-panel symbol, real (across TRAIN+VAL days present) vs null (across 7 seeds). 1b: vartop10 vs n_tick across all 140 TRAIN+VAL symbol-days. Interpretation branches fixed before looking (material gap = null > ~2x real; 1c n-matched recomputation only if triggered). Evidence: out/multiday/n_confound_analysis.txt; scripts scratchpad-only (plain aggregation over existing summary CSVs, per precedent) | 1a: NO material n gap, and the sign is OPPOSITE the worry - pooled median n_tick real 68,609 vs null 48,625 (null/real 0.71); per-symbol ratios span 0.39 (SOXL) .. 1.44 (IJH), no symbol near 2x. 1b: NO systematic n dependence - Spearman rho(n_tick, vartop10) = -0.078 over 140 symbol-days; OLS of vartop10 on log10(n_tick): slope -0.15/decade, R^2 = 0.019; binned medians non-monotone (0.35 / 0.24 / 0.30 / 0.39 / 0.23 for n < 20k / 20-40k / 40-80k / 80-160k / >= 160k); within-symbol (>= 4 days) median rho -0.20 with mixed signs (VXX +0.80, XLE -0.70) | VERDICT (pre-fixed branch 1 of 3): the confound is NOT operating. At these n (21k-346k) vartop10 is dominated by the tail structure of \|r_tick\|, not by arithmetic - an i.i.d. light-tailed series WOULD show its top-10 share falling like ~1/n, and this data does not, which is exactly what "variance concentration is a property of the book, not of the sample size" predicts. The 0.5 cutoff therefore means the same thing at n = 10k as at n = 250k, on both columns. The null-density finding STANDS, with one sharpening: the null's lower vartop10 (0.18 vs real 0.38) cannot be a bigger-n artifact because the null's n_tick is SMALLER than real's - "the mid moves in many moderate steps" is a claim about the SIZE distribution of mid moves (fatter body, no stale-touch flaps), not their count; the null in fact produces FEWER mid changes than the real book. 1c not triggered; the lag-100 disjointness stands as measured. Step 2 (persistence ratification) is CLEARED. |

## Stylized facts - real column (Phase 3)

**Empirical base is WEAK: one day (2019-07-30) of one thin venue (NASDAQ BX,
minority share of consolidated volume; its mid can be stale or wide relative
to the NBBO). This is a first baseline, not validated empirical ground truth.
It needs more days and ideally a consolidated/deeper feed before it can carry
the headline real-vs-sim comparison.** Top 20 symbols by book-message count,
continuous hours only; values are cross-symbol medians [range]. Sampling and
zero-return treatment per PLAN.md Decisions (2026-07-30, Step 2).

| stylized fact | measured (this venue, this day) | Cont 2001 expectation | verdict + boring explanation first |
|---|---|---|---|
| Fat tails (excess kurtosis of mid log-returns) | 1s: median 108 [7.7 .. 19340]; clean liquid names: SPY 17.2, IWM 10.2, XLE 7.9, SMH 9.9. Event-time: median ~5448 (70% zero returns inflate it; tick-time variant in summary CSV) | Strongly leptokurtic at fine scales, kurtosis >> 0 | MATCH. The huge outliers are venue artifacts, not exotic tails: QQQ 19340 -> 10.6 and IWB 20429 -> 16.0 after trimming the first/last 5 min (a single 2.4% mid move at 09:30:02 while the book populates, resp. a 2.3% jump into the 16:00 close); MSFT/GDX/UVXY stay high after trimming because a shallow BX side flaps midday (MSFT mid swings +-2.9% around 10:00 when a near level appears/cancels). |
| Tail index (Hill, top 5% of nonzero \|r\|) | 1s: median 2.59 [0.64 .. 8.67] | alpha ~ 3 ("cubic law"), typically 2-5, finite variance | BROADLY CONSISTENT. Low outliers (AAPL 0.64, MSFT 0.68) are the same stale-touch jumps polluting the tail, not evidence of infinite-variance returns. |
| Aggregational Gaussianity (kurtosis vs sampling interval) | median falls 108 (1s) -> 18.9 (10s) -> 5.1 (60s); monotone for 15/20 symbols | Kurtosis decreases as the interval grows; distribution approaches normal | MATCH - the decay curve itself is the fact. Non-monotone symbols are the artifact carriers above; 390 points at 60s also makes that cell noisy. |
| No linear autocorrelation in returns | ACF(r) 1s lag-1 median -0.15 [-0.52 .. -0.01], near zero by lag 2-3 (SPY: +0.009 at lag 2, +0.004 at lag 50). Event-time lag-1 median -0.25 | Insignificant beyond very short lags; tick-scale lag-1 negative from microstructure | MATCH. The negative lag-1 is bid-ask bounce / quote flicker (reported, not smoothed away); beyond it, autocorrelation is economically negligible. |
| Volatility clustering (ACF of \|r\|, 1s) | lag 1: 0.166, lag 10: 0.060, lag 50: 0.042, lag 100: 0.039 - still positive at lag 100, decaying slowly (power-law-ish), while ACF(r) is dead by lag 2 | Positive, slowly decaying ACF of \|r\|; the contrast with fast-decaying ACF(r) is the fact | MATCH. The contrast is clean on the medians and on SPY individually (0.166/0.108/0.077/0.081 at lags 1/10/50/100 vs raw-return ACF ~ 0.01). |
| Order-flow autocorrelation (signs of aggressive flow, Lillo-Farmer) | lag 1: 0.42, lag 10: 0.15, positive out to ~lag 100 (median 0.017 across the 11 symbols with >= 500 market orders); log-log slope over lags 1-100: median -0.61 [-1.30 .. -0.26] | Positive, long-range, power-law decay with gamma ~ 0.5 persisting over 1000s of trades | QUALITATIVE MATCH, WEAK BASE. Sign, magnitude, and slow decay agree (slope -0.61 vs gamma ~ 0.5), but BX executes so little (10-2945 collapsed market orders per symbol-day; OILX 0) that lag-1000 memory is unmeasurable here - the requested lags 1-1000 exceed n/4 for every symbol. This fact NEEDS the deeper feed before it can discriminate an LM from a naive generator. SUPERSEDED on the lag-1 number by the 2c correction in the addendum below: the collapse rule was splitting sweeps; corrected lag-1 is 0.27. |

## Addendum 2026-07-30: Step 2 statistical corrections (2a/2b/2c)

The table above is kept as first written (append-only); these rows correct it.
Tool: tools/stylized with tick-time \|r\| ACF columns and a windowed sign
collapse (--collapse-ns, default 1ms). Same day, same top-20 symbols.

| correction | measured | verdict |
|---|---|---|
| 2a Volatility clustering vs activity clustering: ACF(\|r\|) recomputed on the tick series (every observation a genuine mid change; zeros absent by construction, so bursty-arrival clustering cannot masquerade as volatility clustering) | 1s-grid medians (unchanged): 0.166 / 0.060 / 0.042 / 0.039 at lags 1/10/50/100. Tick-time medians: 0.413 / 0.083 / 0.016 / 0.018. Tick lag-1 is inflated by quote flicker (a level blinking off/on produces consecutive equal \|r\|), so lags 10-100 are the informative range. Per-symbol split: ~11/20 keep tick lag-10 >= 0.05 with genuinely slow decay on the liquid names (SPY 0.256/0.199/0.154 at lags 10/50/100, n=41k, se~0.005; AAPL, IWN, IJH, IWO, XLU similar); 6/20 (IWM, XLV, XLP, XLE, XLI, IWB) collapse to ~0 at every tick lag | PARTIAL SURVIVAL. Volatility clustering is real on roughly half the symbols - positive, significant, slowly decaying in tick time where activity clustering is impossible by construction. But the lag-50/100 plateau on the 1s grid (~0.04) roughly halves in tick time (~0.017), and 6 symbols lose it entirely: a substantial share of what the calendar-grid statistic measured was bursty trading activity, not volatility. The original table's "MATCH" overstates it; honest statement = both effects present, volatility clustering weaker than the 1s numbers suggested and absent on ~a third of the panel. |
| 2b Hill alpha < 1 reclassified as estimator failures | AAPL 0.64 and MSFT 0.68 imply infinite MEAN - no return series has that; the cause is the documented stale-touch jumps dominating the top-5% order statistics, i.e. a broken estimate, not a low measurement. Cross-symbol median without them: 2.59 -> 2.95 over 18 symbols, range [1.58 .. 8.67] | CORRECTED. The "Tail index" row's "[0.64 .. 8.67]" range mixed measurements with failures; corrected median 2.95 sits square in the cubic-law band 2-5. Rule going forward: alpha < 1 is reported as a failure with its reason, never averaged in. |
| 2c Sign-collapse rule: was exact-timestamp only; one order sweeping several levels produces fills with distinct ns timestamps and was being counted as several decisions | Measured same-sign inter-fill gap distribution (448k pairs): bimodal, machine-scale mode ~32us extending to ~1ms, decision-scale mass >= 100ms, valley at 10-32ms -> collapse window default 1ms (conservative side of the valley). Lag-1 sign ACF median: 0.416 (11 syms >= 500 signs) -> 0.270 (9 syms); on the identical 9-symbol set 0.414 -> 0.270. Sensitivity: 100us -> 0.319, 10ms -> 0.216. Log-log slope stable (-0.61 -> -0.63); collapsed sign counts 19.1k -> 15.5k | FIXED, boring explanation confirmed. Landing above the literature after collapsing was indeed sweep-splitting; the corrected lag-1 0.27 sits inside Lillo-Farmer's typical 0.2-0.3 and is robust to a 100x window sweep (0.32-0.22). The long-memory slope is unchanged, so the earlier qualitative claim stands; only the lag-1 level moves. |

## Stylized facts - real column v2 (TRAIN+VAL multi-day, 2026-07-30)

Supersedes the one-day table above as the Phase 3 "real" column (that table
stays as written, append-only; it was computed on 20190730, which the
dataset split placed in VAL, so it used no held-out data - it is superseded
for being one draw, not for contamination). Basis: the 7 TRAIN+VAL days
(20190130..20191230 + VAL 20190730), top 20 symbols per day, defaults
(1ms collapse). TEST (20181228, 20200130) untouched, guard-enforced.
Values are cross-day medians of cross-symbol medians; brackets are the
range of the 7 day-medians - the honest error bar a one-day baseline never
had. Per-day numbers: out/multiday/per_day_summary.csv.

| stylized fact | TRAIN+VAL value [day-median range] | day-to-day verdict |
|---|---|---|
| Fat tails, excess kurtosis of 1s mid returns | 108 [18.7 .. 1655] | LEVEL UNSTABLE (2 orders of magnitude): dominated by each day's few largest single-second moves - genuine macro jumps (20190130 = FOMC statement day, SPY +38bp in 1s at 14:00:10) and venue flaps (same day: 225bp SPY spike at 14:52, reversed next second). Not a scoring target; score the decay SHAPE below. |
| Aggregational Gaussianity | 1s 108 -> 10s 18.9 [3.1 .. 85.9] -> 60s 5.1 [1.3 .. 11.2] | ROBUST as a shape: kurtosis falls monotonically with scale on ALL 7 days, whatever the 1s level. |
| Tail index (Hill, 1s, alpha<1 excluded as failures) | 2.95 [2.50 .. 3.82]; 0-4 failures/day, always vol products (VXX, TVIX, UVXY, SVXY) or stale-touch names | STABLE: every day-median in the cubic-law band 2-5. Scoreable as a range. |
| No linear autocorrelation (ACF(r) 1s lag-1) | -0.18 [-0.39 .. -0.15] | Negative (bounce/flicker) on all 7 days; magnitude varies ~2.5x. |
| Volatility clustering, 1s grid ACF(\|r\|) | lag-10 0.060 [0.047 .. 0.115]; lag-100 0.026 [0.018 .. 0.056] | Positive at lag 100 on all 7 days. |
| Volatility clustering, tick time (2a discriminating version) | lag-10 0.120 [0.077 .. 0.171]; lag-100 0.033 [0.015 .. 0.048] | Positive on all 7 days - volatility clustering proper survives in tick time on every day, at roughly half the 1s-grid apparent strength. |
| Order-flow sign ACF(1), 1ms collapse | 0.274 [0.108 .. 0.341] | 6 of 7 days in 0.27-0.34; one outlier (20190130: 0.108). Window-conditional per the sensitivity sweep AND day-varying. |
| Order-flow log-log decay slope (lags 1-100) | -0.56 [-0.63 .. -0.44] | Consistently negative power-law-ish, but the earlier "robust quantity" claim needs narrowing: robust to the collapse window, NOT to the draw - it swings ~0.2 across days. Score as a range, not a point. |

## Stylized facts - null column (CST, 2026-07-30)

Cont-Stoikov-Talreja memoryless null, calibrated on the 6 TRAIN days,
13-symbol panel, 7 seeded days through the identical tools/stylized
pipeline. The real column here is the TRAIN+VAL days RESTRICTED to the same
13 symbols (medians over different symbol sets would not be comparable), so
its numbers differ slightly from the v2 table above. Values are cross-day
(cross-seed) medians of cross-symbol medians [range of day/seed medians].

| stylized fact | real (panel) | CST null | discriminates? |
|---|---|---|---|
| Fat tails, 1s excess kurtosis | 163 [19.6 .. 3815] | 246 [164 .. 410] | NO - the null's kurtosis sits inside the real day-range. Fine-scale fat tails come from thin-book mechanics + empirical order sizes, not from strategic flow. |
| Aggregational Gaussianity | 163 -> 13.6 -> 4.6 | 246 -> 42.9 -> 7.8 (monotone on all 7 seeds) | NO qualitatively - the decay shape comes free. Null decays slower in level, but real day-ranges ([3.7 .. 54.4] at 10s, [1.3 .. 8.5] at 60s) overlap the null's. |
| Hill tail index (alpha<1 excluded) | 2.34 [2.10 .. 3.59] | 2.63 [2.39 .. 2.92] | NO - squarely overlapping; the null even reproduces the alpha<1 estimator-failure mode (QQQ/IJH/IWN vs the real day's vol products). |
| ACF(r) 1s lag-1 (bounce) | -0.162 [-0.257 .. -0.096] | -0.059 [-0.078 .. -0.039] | WEAKLY - sign comes free (any two-sided book flickers); the real magnitude is ~3x the null's and the ranges nearly touch. Sanity check, not a scoring target. |
| Volatility clustering, 1s ACF(\|r\|) lag-10 / lag-100 | 0.070 [0.032 .. 0.104] / 0.049 [0.016 .. 0.052] | 0.005 [0.001 .. 0.009] / 0.000 [-0.002 .. 0.004] | YES - disjoint ranges. Poisson rates cannot cluster volatility. |
| Volatility clustering, tick time lag-10 / lag-100 | 0.120 [0.103 .. 0.175] / 0.037 [0.019 .. 0.064] | 0.046 [0.036 .. 0.057] / 0.002 [0.000 .. 0.004] | YES - disjoint at both lags (the null's small tick lag-10 is mechanical flicker, dead by lag 100). |
| Flow-sign ACF(1), 1ms collapse | 0.277 [0.160 .. 0.331] | -0.009 [-0.017 .. 0.000] | YES - the null is exactly zero (i.i.d. signs by construction); every real day is far above it. |
| Flow-sign log-log slope, lags 1-100 | -0.592 [-0.793 .. -0.432] | -0.018 [-0.229 .. 0.003] | YES - the decisive one, per the pre-registered rule: the null lands near zero, not near -0.6; its seed spread is estimator noise (fitting log of ~N(0,1/n) values) and does not overlap the real range. Slope is a legitimate scoring target; null floor = slope >= -0.23, ACF(1) ~= 0. |

## Stylized facts - null column CORRECTION (measurability rule applied, 2026-07-30)

SUPERSEDES the two "Volatility clustering" rows of the null column above.
Those rows compared a MEASURABILITY-FILTERED real column against an UNFILTERED
null column - not a comparison. Here the identical vartop10 rule (UNMEASURED
when vartop10_absr_tick >= 0.5) and cutoff are applied to BOTH sides, on the
13-symbol CST panel, tick time. Per-symbol PRESENT medians then across symbols
(the v3 structure). Evidence: out/cstnull/seed*/ regenerated through the
current stylized (the earlier null CSVs predated the vartop10 column); scripts
in scratchpad, not repo-worthy.

**The predicted pathology did NOT occur, and its opposite did.** The worry was
that the null's kurtosis overshoot (246 vs 163) meant its mid flickers more
than reality, so most null symbol-days would come back UNMEASURED. Measured:
null is *less* variance-concentrated than the real book - null vartop10 median
0.18 vs real 0.38, null UNMEASURED 15/91 (16%) vs real 31/75 (41%). Finding
about the null: its continuous Poisson placement fills the book more densely
than a real thin BX venue, so its mid moves in many moderate steps, not a few
stale-touch flaps. The kurtosis overshoot is a fatter *body* of moderate
moves, not giant jumps - which is exactly why vartop10 is LOWER, not higher.
Measurability sensitivity (real / null P/A/U): cutoff 0.3 = 33/0/42 / 56/3/32;
0.5 = 44/0/31 / 70/6/15; 0.7 = 52/1/22 / 78/8/5.

Measurable-only (PRESENT) tick ACF(|r|), cross-symbol median [range], real vs
null, cutoff 0.5:

| lag | real (panel) | CST null | separation |
|---|---|---|---|
| 10 | 0.154 [0.051 .. 0.316] (12 sym) | 0.044 [0.014 .. 0.096] (13 sym) | **OVERLAP** - only 9/12 real symbols exceed the null max; 8/13 null symbols sit below the real min. The lag-10 LEVEL does not cleanly discriminate per symbol. |
| 50 | 0.074 [0.015 .. 0.199] | 0.004 [-0.002 .. 0.017] | near-disjoint (10/12 real > null max; 12/13 null < real min) |
| 100 | 0.050 [0.015 .. 0.160] | 0.002 [-0.005 .. 0.007] | **DISJOINT** - 12/12 real symbols >= 0.015, 13/13 null symbols <= 0.007. No overlap. |

**Verdict - mixed, and reported as mixed rather than forced into the
pre-registered binary.** The original null table's "disjoint at both lags"
claim is FALSIFIED at lag 10: on measurable-only per-symbol data the lag-10
level ranges overlap. What survives, cleanly, is PERSISTENCE: every real
PRESENT symbol keeps positive tick ACF(|r|) out to lag 100 while every null
symbol has decayed to ~0 by lag 50 - the 12/12-vs-13/13 separation at lag 100
is complete. This is the textbook volatility-clustering signature (slow ACF
decay), and lags 50/100 were part of the pre-specified clustering statistic
(the v3 table), NOT a fact hunted after the fact. But because the outcome is
NOT the clean "disjoint" case the pre-registered rule anticipated - it
overlaps at the lag the original claim led with - the decision to adopt
PERSISTENCE (rather than the lag-10 level) as the volatility-clustering
scoring statistic is deferred to the user, per the rule's "log it and let me
decide." Not adopted unilaterally this session, and NO third fact was hunted
to replace it. Net for Step 3 pre-registration: flow-sign memory remains the
unambiguous discriminator; volatility-clustering-as-persistence is a
candidate second discriminator pending that ratification.

## Stylized facts - per-symbol structure (v3, 2026-07-30)

Supersedes the v2 pooled-median table as the headline structure (v2 stays,
append-only). A fact true for half the panel is a per-symbol property, and
a pooled median cannot detect a model that matches the average while
getting every symbol wrong - so the real column, and the eventual LM and
null columns, are scored PER SYMBOL. Unit of observation = symbol-day
(140: union of daily top-20s over the 7 TRAIN+VAL days, 49 distinct
symbols); per-symbol values are medians over the days the symbol appears
(1-7). Vol-clustering measurability rule (stated before classifying):
a symbol-day is UNMEASURED when vartop10_absr_tick >= 0.5 - when ten
points carry half the variance, the ACF measures outlier placement, not
volatility. Sensitivity of the 87P/4A/49U split to that cutoff: 0.3 ->
70/3/67, 0.7 -> 97/6/37; the ABSENT set stays tiny under every cutoff.
Full per-symbol table: out/multiday/persym_analysis.txt.

| fact | how many symbols exhibit it | dispersion of per-symbol medians | structure |
|---|---|---|---|
| Fat tails (1s kurtosis > 0) | 49/49 symbols, every measured day | q25 28 / med 278 / q75 1727, range [2.1 .. 21307] | UNIFORM in sign, wildly symbol-dependent in level (4 orders of magnitude). Level is not scoreable per symbol either - it is artifact-dominated (see kurtosis rows above). |
| Hill tail index (1s, alpha >= 1) | 39/47 symbols with medians in the cubic-law band 2-5; failures (alpha < 1) on 10/140 symbol-days, concentrated in AAPL, MSFT, and the vol products | q25 2.25 / med 3.08 / q75 4.62, range [1.01 .. 22.98] | MOSTLY UNIFORM; the failure MODE is itself symbol-structured (stale-touch-dominated names) and a model should reproduce failures on the same kind of symbol. |
| No linear autocorrelation / bounce (ACF(r) 1s lag-1 < 0) | 49/49 symbols negative median; significant on 137/140 symbol-days | q25 -0.325 / med -0.212 / q75 -0.121, range [-0.502 .. -0.006] | UNIFORM in sign, 80x spread in magnitude - per-symbol magnitude is a real scoring axis. |
| Volatility clustering (tick time, lag 10) | PRESENT on 87/91 measured symbol-days (96%); ABSENT only OILX (2 days, zero executions on this venue), IWB (1 day), MDY (1 borderline day); UNMEASURED 49/140 symbol-days - concentrated in tight-spread pinned names (IWM 0 measurable days of 7; XLI 0/3; XLP, SDY, DUST, EFA never measurable) | over PRESENT symbols: q25 0.107 / med 0.173 / q75 0.282, range [0.023 .. 0.995] | NEAR-UNIVERSAL where measurable. The "half the panel lacks it" reading is dead: the split was measurement validity (variance concentration from venue flicker), not a property split. An LM must be scored per symbol WITH the same measurability rule applied to its output; matching the pooled median is worthless. |
| Order-flow memory (sign ACF(1), >= 500 signs) | measurable for 30/49 symbols (the rest execute too little on BX); significant-positive on 76/78 measurable symbol-days; per-symbol median ACF(1) positive for 30/30 | ACF(1): q25 0.207 / med 0.298 / q75 0.375, range [0.037 .. 0.517]. Slope: q25 -0.63 / med -0.52 / q75 -0.42, range [-0.95 .. -0.22] | UNIFORM in presence where measurable, symbol-dependent in strength. 19 symbols are unmeasured for flow memory on this venue - the LM cannot be scored on them and must not be penalized or credited there. |

## Context-length gate: can n_ctx=320 express the scored fact? (2026-07-30)

Question (measurement only, no fix proposed): the planned LM (tape repo
config: 4 layers, vocab 52, n_ctx=320) holds 320 tokens = 64 events at 5
tokens/event. The pre-registered scoring fact is flow-sign ACF out to lag
100 in COLLAPSED-SIGN space (1ms collapse). How many tokens of context does
lag 100 actually require on BX?

Method: computed from the existing per-day stylized summaries
(out/multiday/<day>/stylized_summary.csv, columns n_msgs_w = applied
in-window book messages and n_signs = 1ms-collapsed market-order signs;
cross-checked against console.txt signN). No raw data read; TEST untouched.
Days: 20190130, 20190530, 20191230 (TRAIN). Panel: the 13 CST panel
symbols; 9 of 39 (symbol,day) cells absent from that day's top-20 (panel
membership is >=4 of 6 days). tokens(lag k) = k * (n_msgs_w / n_signs) * 5,
i.e. signs treated as uniformly spaced in event time (real signs cluster,
so tail lags are ragged; does not change the order of magnitude).
Independently recomputed from the CSVs (all 39 cells): exact agreement.

| symbol | day | msgs_w | signs | events/sign | tok lag1 | tok lag10 | tok lag100 | ×320 |
|---|---|---|---|---|---|---|---|---|
| IWM | 20190130 | 432898 | 5468 | 79.2 | 396 | 3958 | 39585 | 123.7 |
| SPY | 20190130 | 355151 | 5667 | 62.7 | 313 | 3134 | 31335 | 97.9 |
| XLK | 20190130 | 278569 | 1579 | 176.4 | 882 | 8821 | 88211 | 275.7 |
| QQQ | 20190130 | 261602 | 1544 | 169.4 | 847 | 8472 | 84716 | 264.7 |
| IWO | 20190130 | 242604 | 43 | 5642.0 | 28210 | 282098 | 2820977 | 8815.6 |
| IJH | 20190130 | 309591 | 124 | 2496.7 | 12484 | 124835 | 1248351 | 3901.1 |
| XLE | 20190130 | 255855 | 861 | 297.2 | 1486 | 14858 | 148580 | 464.3 |
| XLV | 20190130 | 237850 | 572 | 415.8 | 2079 | 20791 | 207911 | 649.7 |
| IWM | 20190530 | 201095 | 1429 | 140.7 | 704 | 7036 | 70362 | 219.9 |
| SPY | 20190530 | 172927 | 1917 | 90.2 | 451 | 4510 | 45104 | 140.9 |
| XLK | 20190530 | 163658 | 816 | 200.6 | 1003 | 10028 | 100281 | 313.4 |
| QQQ | 20190530 | 174451 | 1716 | 101.7 | 508 | 5083 | 50831 | 158.8 |
| UVXY | 20190530 | 145965 | 1344 | 108.6 | 543 | 5430 | 54302 | 169.7 |
| SOXL | 20190530 | 156803 | 55 | 2851.0 | 14255 | 142548 | 1425482 | 4454.6 |
| TLT | 20190530 | 127038 | 1239 | 102.5 | 513 | 5127 | 51266 | 160.2 |
| IJH | 20190530 | 141932 | 189 | 751.0 | 3755 | 37548 | 375481 | 1173.4 |
| VXX | 20190530 | 126622 | 735 | 172.3 | 861 | 8614 | 86137 | 269.2 |
| IWM | 20191230 | 390996 | 1945 | 201.0 | 1005 | 10051 | 100513 | 314.1 |
| SPY | 20191230 | 584750 | 4263 | 137.2 | 686 | 6858 | 68584 | 214.3 |
| XLK | 20191230 | 186054 | 610 | 305.0 | 1525 | 15250 | 152503 | 476.6 |
| QQQ | 20191230 | 428751 | 1917 | 223.7 | 1118 | 11183 | 111829 | 349.5 |
| IWO | 20191230 | 156156 | 66 | 2366.0 | 11830 | 118300 | 1183000 | 3696.9 |
| UVXY | 20191230 | 177240 | 4622 | 38.3 | 192 | 1917 | 19174 | 59.9 |
| SOXL | 20191230 | 135202 | 42 | 3219.1 | 16095 | 160955 | 1609548 | 5029.8 |
| TLT | 20191230 | 137637 | 984 | 139.9 | 699 | 6994 | 69938 | 218.6 |
| IJH | 20191230 | 127852 | 57 | 2243.0 | 11215 | 112151 | 1121509 | 3504.7 |
| XLE | 20191230 | 152517 | 1497 | 101.9 | 509 | 5094 | 50941 | 159.2 |
| XLV | 20191230 | 122439 | 248 | 493.7 | 2469 | 24685 | 246853 | 771.4 |
| IWN | 20191230 | 192313 | 72 | 2671.0 | 13355 | 133551 | 1335507 | 4173.5 |
| VXX | 20191230 | 187297 | 3116 | 60.1 | 301 | 3005 | 30054 | 93.9 |

Absent cells (not in that day's top-20): UVXY/SOXL/TLT/IWN/VXX on 20190130;
IWO/XLE/XLV/IWN on 20190530.

Pooled over the 30 valid rows: median events/sign 188.5; median tokens for
lag 100 = 94,246; n_ctx growth factor for lag 100: median 294.5x, min
59.9x (UVXY 20191230, the sign-densest cell), max 8815.6x (IWO 20190130).

ANSWER: NO - the planned n_ctx=320 cannot express the pre-registered
scoring fact, and not marginally: the context spans lag 100 in sign space
for 0 of 30 symbol-days. It is worse than that: the MEDIAN tokens needed
to span even lag 1 is 942 (> 320 for 27 of 30 rows), so a 320-token
context typically contains ZERO complete prior market-order signs - the
model would have to reproduce sign autocorrelation at lags it can never
condition on, i.e. via marginal statistics rather than memory. Even the
best cell needs ~60x growth for lag 100. This is a measurement, not a
proposal: the architecture decision (n_ctx, tokenization, or otherwise)
is the user's; the pre-registration is NOT amended by this row.

## BX bin refit + PRICE_OFF window re-measurement (Phase 5, 2026-07-30)

Retraining decisions, measured on BX TRAIN (panel symbols; pooled fit over
3 TRAIN days spanning regimes: 20190130, 20190530, 20191230; VXX on the
latter two only - the old VXX series matured Jan 2019 and is absent from
the 20190130 stock directory). Fit sample = the in-window expanded event
stream the tokenizer emits (tools/itch_tokenize_fit, frac 1.0; the real
train/eval split is by day). Evidence: out/tokens/manifest.json,
out/tokens/phase5/ (per-symbol fit logs + 38 per-(symbol,day) PRICE_OFF
histograms).

SIZE edges (7 quantile edges, k/8): LOBSTER-SPY-2012 does NOT transfer.
BX-SPY pooled: [7,30,100,101,102,200,500] vs LOBSTER [100,200,201,387,
500,501,1000] - BX carries heavy sub-100 odd-lot mass LOBSTER SPY lacked.
Per symbol the edges differ strongly (IWO [200..205,400]; TLT
[100..103,240,800,1300]); a single-day fit also differs from the pooled
fit (1-day SPY: [100..104,500,501]) - day mix matters. Round-lot
discreteness makes several adjacent edges collapse to consecutive
integers (the enforce_edge_invariants +1 bumps), so some buckets are
1-share-wide and per-day occupancy is lumpy (e.g. IWO buckets 2-5 carry
ZERO events on all 3 days; SOXL 20191230 puts 100% in 3 buckets).
FINDING, not changed here: 8 per-ticker quantile buckets on a
round-lot-quantized venue yield degenerate near-empty buckets; if bucket
occupancy matters for training efficiency, the bin RULE (not the count)
would need rethinking - that is a tokenizer-design decision, not a refit.

DT edges (14 log-spaced, p0.1-p99.9 of nonzero gaps): BX is 1-2 orders of
magnitude burstier/thinner than LOBSTER SPY 2012. Upper anchor 1.9-13.6
SECONDS across the panel vs LOBSTER's 0.26s; lower anchors comparable
(~200-260ns). dt_zero (shared-timestamp bursts incl. U-expansion add
halves) spans 0.5-21% by symbol-day (SOXL ~20%). Refit was mandatory, as
predicted by FORMAT_RECONCILIATION.

PRICE_OFF window (-1..+10, single -1 bucket) re-measured on 8.03M
in-window panel events:

| sym | events | unk% | -1% | 0..+10% | >+10% | >+30% | max |
|---|---|---|---|---|---|---|---|
| IWM | 1033305 | 0.00 | 20.31 | 79.28 | 0.41 | 0.000 | 24 |
| SPY | 1158926 | 0.00 | 18.02 | 80.54 | 1.44 | 0.002 | 32 |
| XLK | 636870 | 0.00 | 10.60 | 89.37 | 0.03 | 0.000 | 17 |
| QQQ | 890971 | 0.00 | 26.84 | 73.09 | 0.07 | 0.000 | 28 |
| IWO | 552567 | 0.00 | 34.13 | 65.66 | 0.21 | 0.000 | 23 |
| UVXY | 499461 | 0.00 | 27.07 | 72.86 | 0.07 | 0.024 | 39 |
| SOXL | 494330 | 0.00 | 24.62 | 75.30 | 0.08 | 0.000 | 28 |
| TLT | 467654 | 0.00 | 9.22 | 90.59 | 0.19 | 0.000 | 17 |
| IJH | 586035 | 0.00 | 24.42 | 74.79 | 0.79 | 0.000 | 22 |
| XLE | 539311 | 0.00 | 13.42 | 86.55 | 0.02 | 0.000 | 15 |
| XLV | 461498 | 0.00 | 14.62 | 85.35 | 0.03 | 0.000 | 15 |
| IWN | 391287 | 0.00 | 35.59 | 64.39 | 0.01 | 0.000 | 15 |
| VXX | 315893 | 0.00 | 12.07 | 87.91 | 0.02 | 0.000 | 26 |
| ALL | 8028108 | 0.00 | 20.86 | 78.78 | 0.37 | 0.002 | - |

Verdict, two-sided: (1) the WINDOW BOUNDS survive - 99.63% of events land
in [-1,+10] pooled (worst symbol 98.56%), UNK is 0%, and mass beyond +30
is negligible, so no window widening is demanded. (2) the "-1 is a single
bucket because inside-spread placement is rare/one-tick" assumption,
measured on LOBSTER where it held, is INVERTED on BX: 20.9% of all panel
events (9-36% by symbol) price INSIDE the spread, because BX spreads sit
several ticks wide, leaving room the tokenizer collapses into one
undifferentiated bucket. One fifth of the event mass loses its
where-inside-the-spread placement. FINDING reported, per the phase rule -
NOT changed silently: splitting -1 into depth-graded inside buckets is a
vocab change (new tuple semantics + retrain) and is the user's call. The
LM can be trained on the current vocab; the cost is expressiveness on
exactly the venue-idiosyncratic feature (wide spreads) the claim scope
already flags.

## PIPELINE TEST: pilot training run (Phase 6, 2026-07-30) - NOT A MODEL

Label repeated because it matters: this is a PIPELINE PROOF, deliberately
small and short, trained this session. It is NOT the scored LM column, no
stylized facts were computed on its output, and no comparison to the null
is made or implied. Nothing here is a headline result.

Setup: tape's minigpt (4 layers, d_model 64, 4 heads, d_mlp 256, n_ctx
320, vocab 52 - the architecture the context-length gate row shows CANNOT
express the scored fact; irrelevant here, the question is only whether
the pipeline runs), trained via tape.model_loader.train_spy UNCHANGED on
this repo's BX tokens: SPY 20190130 (TRAIN day), pilot manifest with a
within-day 80/20 chronological split (bins fit on the train prefix only,
tools/itch_tokenize_fit --train-frac 0.8; the panel bins are untouched -
separate manifest). 1000 steps, batch 16, lr 3e-3 cosine, CPU. Driver:
out/tokens/pilot_train.py; artifacts in out/tokens/ (gitignored).

Tokens in -> checkpoint out: train split 301,140 events (1,505,704
tokens), eval split 75,285 events (376,429 tokens). Held-out loss
(deterministic non-overlapping-window sweep, full splits): 1.1961 vs
train 1.1567, gap +0.039 and widening slowly (0.007 at step 250) -
normal early-training behavior, no anomaly. Throughput: 4.43 steps/sec =
22,685 tokens/sec (CPU; the tape loader accepted this repo's OFTK bin +
manifest with zero changes, which was the point).

Sampling -> adapter: 15,002 tokens sampled autoregressively (temperature
1.0, seed 0) from BOS SESSION_OPEN, written as an OFTK bin, and driven
through the token<->action shim into the adapter (tools/shim_drive, book
seeded two-sided at $100.00 +/- 1 tick):
  3,043 tuples, 53 resyncs (misaligned garbage tokens), 2 specials.
  applied 41 (1.35%) | Unparseable 74 (2.43%) | UnknownReference 2,928
  (96.22%) | InvariantViolation 0 | EconomicallyAbsurd 0.
  Book end state: audit clean, invariants held throughout.
Reading, and only this much: the 1000-step model over-emits deletes
(sampled TYPE histogram: 1551 ADD / 1417 DELETE / 34 EXEC / 2 CANCEL),
drains the seeded book, and from then on every tuple that needs a price
reference fails UnknownReference on an empty book - the expected garbage
of a short run, and still a successful pipeline test per the phase
definition: tokens in, checkpoint out, sampled stream in, adapter
classifies every tuple, zero invariant violations. The pipeline is
end-to-end proven; model quality was not the question.

## Size-bucket confound quantified (Step 0d, 2026-07-31)

Context: the architecture Decision of 2026-07-31 (PLAN.md) logs the
size-bucket confound so it can be REPORTED, not used to discount a loss.
Numbers, from exact in-window size histograms of the 13 panel symbols
pooled over the 3 bin-fit TRAIN days (out/tokens/phase5/szhist_*.csv,
itch_tokenize --szhist; 8.03M events):

Occupancy per SIZE bucket (b0..b7, % of events; manifest edges): the
quantile promise of ~12.5% per bucket fails badly on round-lot-discrete
data - upper_bound edges cannot split point masses. Pooled:
[7.3, 28.0, 16.4, 13.5, 3.4, 5.6, 9.5, 16.1]. Extremes: IWO puts 81.7%
in bucket 1 with buckets 2-5 at 0.0%; SOXL 79.8% in bucket 1; UVXY 67.9%
in bucket 2. Full per-symbol table in the analysis output (occupancy
rows reproduced in the szhist CSVs + manifest edges; recompute:
scratchpad szconfound.py logic - bisect edges over the histogram).

Variance preserved by the 8-level quantization (within-symbol, pooled =
1 - sum SSE / sum SS):
- with the DECODER's representatives (what the LM can emit): R2 = 0.619
  pooled; per symbol 0.181 (UVXY) .. 0.979 (IJH), median ~0.63.
- with per-bucket conditional means (the ceiling of ANY 8-level
  quantizer at these edges): R2 = 0.779 pooled.
So the LM's emittable size distribution carries ~62% of the within-
symbol size variance the CST null draws exactly (the null samples the
FULL empirical histogram). Asymmetry on record, with the standing
caveat: this touches the DISTRIBUTIONAL SANITY CHECKS only; the two
scored facts (flow-sign memory, vol-clustering persistence) do not
depend on size resolution, and a loss on a scored fact is not explained
by this row.

## Generated-stream VIABILITY BAR (Step 2, 2026-07-31) - recorded BEFORE any model run

The pilot showed the failure mode training does not automatically cure:
a slight delete bias compounds over a long generation, drains the book,
and every later tuple rejects UnknownReference - while the stylized
facts need thousands of market orders from a book that is still alive.
tools/sim_health drives a generated OFTK stream through the shim into
the adapter and records book health over generation time (CSV per K
tuples: open_orders, level counts, touch, spread, two-sidedness,
cumulative rejects by category, collapsed signs).

THE BAR, fixed now, before the diagnostic has been run against any
model output: a generated stream is VIABLE for stylized-fact
measurement iff
  (V1) it produces >= 500 collapsed market-order signs - the project's
       standing measurability threshold - using the same 1ms same-sign
       collapse as everywhere else, on the stream's PSEUDO-CLOCK
       (accumulated decoded DT bucket representatives; generated
       streams carry no real timestamps - decision logged);
  (V2) the book is two-sided at >= 90% of sampled checkpoints; and
  (V3) open_orders never reaches 0 (dead book) before the 500th sign.
Also reported, not part of the bar: open_orders DRIFT (OLS slope per 1k
tuples) - a stream can pass while drifting toward collapse, and the
drift says whether a longer generation survives. Signs are counted from
applied actions with filled shares > 0 (Market or marketable Limit),
sign = aggressor side.

Known-bad check (run AFTER the bar above was recorded): the pilot
checkpoint's sampled stream (out/tokens/pilot_sampled.tokens.bin) is
NOT VIABLE on all three conditions - book DEAD at tuple 29 (the model
deletes the seed orders almost immediately), 0 collapsed signs,
two-sided at 0.0% of checkpoints, 96.2% UnknownReference. The
diagnostic catches the known failure mode; that was the point of
testing it on a known-bad stream. Series: out/tokens/pilot_health.csv.

## TRAIN corpus built (Step 1, 2026-07-31)

itch_tokenize, frozen panel bins (out/tokens/manifest.json), 13 panel
symbols x 6 TRAIN days -> 76 OFTK bins (VXX absent from the 20190130 and
20190327 stock directories - old series matured Jan 2019; noted, not an
error) + 13 VAL-day (20190730) bins for held-out loss. One day
decompressed at a time, deleted after. 188MB in out/tokens/corpus/
(gitignored; deterministic - rebuild via out/tokens/corpus_build.sh).

Per symbol (TRAIN days pooled; ev/sign = in-window events per collapsed
sign over the days where the sign count is known from the stylized
summaries - top-20 coverage in the sign-days column):

| sym | days | events | tokens | sign-days | signs | ev/sign |
|---|---|---|---|---|---|---|
| IWM | 6 | 1790604 | 8953044 | 6 | 12508 | 143.2 |
| SPY | 6 | 2714505 | 13572549 | 6 | 22512 | 120.6 |
| XLK | 6 | 1577673 | 7888389 | 6 | 6517 | 242.1 |
| QQQ | 6 | 1782996 | 8915004 | 6 | 10003 | 178.2 |
| IWO | 6 | 1253049 | 6265269 | 5 | 359 | 3125.5 |
| UVXY | 6 | 1105543 | 5527739 | 5 | 13215 | 70.5 |
| SOXL | 6 | 1215243 | 6076239 | 5 | 285 | 3766.8 |
| TLT | 6 | 1101460 | 5507324 | 5 | 6001 | 153.1 |
| IJH | 6 | 1322682 | 6613434 | 4 | 703 | 1573.2 |
| XLE | 6 | 1134130 | 5670674 | 4 | 4249 | 208.1 |
| XLV | 6 | 886756 | 4433804 | 4 | 1941 | 371.8 |
| IWN | 6 | 948855 | 4744299 | 4 | 489 | 1544.8 |
| VXX | 4 | 848794 | 4243986 | 4 | 7936 | 107.0 |
| ALL | - | 17682290 | 88411754 | | | |
| VAL 20190730 | 1 | 1924809 | 9624097 | | | |

Context-gate cross-reference: at 5 tokens/event the corpus is 88.4M
TRAIN tokens; ev/sign spans 70.5 (UVXY) to 3766.8 (SOXL), so a 320-token
(64-event) context holds a whole sign interval only for the densest
symbols on their densest days - the Step 0 architecture Decision's
premise, now measured on the actual training corpus.

## Step 3 (2026-07-31): throughput, INTERRUPTED budget run, viability

3a. THROUGHPUT at the 320-ctx config (4L/d64/h4/mlp256, batch 16,
fwd+bwd+opt on random tokens; out/tokens/bench_step.py):

| device | steps/s | tokens/s | 10k steps | 50k steps | 200k steps |
|---|---|---|---|---|---|
| cpu | 5.44 | 27,836 | 31m | 2.6h | 10.2h |
| mps | 9.95 | 50,968 | 17m | 1.4h | 5.6h |

Realized training rate is lower than the bench (CPU window-gather +
host->device transfer per step): ~6.4 steps/s sustained on MPS, so the
90-minute budget fits ~32,000 steps (~164M training tokens ~= 1.9 epochs
of the 88.4M-token corpus). A first launch at 45k steps was killed by me
at step 600 once the realized rate showed it would blow the budget.

3b. BUDGET-BOUNDED RUN - INTERRUPTED, reported as what it is. The
32,000-step run on MPS was externally stopped twice (step 19,900 of the
first attempt - no checkpoint survived, a design gap since fixed by
periodic checkpointing; step 2,300 of the second attempt - the step-2,000
periodic checkpoint survived). Not restarted a third time against an
apparent deliberate stop signal; the user was asked and did not respond
in time. What exists is therefore a 2,000-STEP checkpoint
(out/tokens/bounded_run.pt): VAL held-out loss 1.1008 (vs 1.1432 at step
1,000; running train loss ~1.06 at the stop). For calibration, the first
(lost) attempt had reached train loss ~0.90 at step 19,900, so 2,000
steps is far short of where 90 minutes lands. THE INTENDED QUESTION -
does a 90-minute-scale model produce a viable stream - IS NOT ANSWERED;
what follows answers it only for the 2,000-step scale.

3c. VIABILITY of the 2,000-step checkpoint (sample.py: 4 streams x
20,002 tokens, temperature 1.0, top-k 0, seed 0; sim_health, bar as
pre-recorded): ALL FOUR STREAMS NOT VIABLE - V1/V2/V3 all FAIL. Book
dead at tuples 12 / 72 / 195 / 916; ZERO collapsed signs in any stream;
two-sided at 0.0% of checkpoints in all four (the ask side dies
immediately and is never rebuilt - the model under-emits resting asks
relative to deletes/executions at this training scale); longest-lived
stream applied 7.6% of tuples, 91.5% UnknownReference. No stylized facts
computed, no null comparison. Series: out/tokens/samples/health_*.csv.

## Step 1 (2026-07-31): THE COLD-START CONTROL - prior viability results measured the HARNESS

Question, asked before any further training: are the viability failures a
property of the model or of the initialization? Control: run the REAL
token stream - by construction what a perfect model emits - through
sim_health under the IDENTICAL two-order seeding every previous viability
result used.

VERDICT: THE REAL STREAM IS NOT VIABLE COLD-STARTED. Every viability
result recorded before this row measured the harness, not the model.

| stream (real BX tokens) | tuples | applied | signs | book dead at | verdict |
|---|---|---|---|---|---|
| SPY 20190130 (TRAIN) | 376,425 | 6,872 (1.83%) | 0 | tuple 3 | NOT VIABLE |
| SPY 20191230 (TRAIN) | 595,169 | 7,742 (1.30%) | 0 | tuple 39 | NOT VIABLE |

Rejection breakdown, SPY 20190130 (sim_health --why, added with this row;
shim::Why names the RULE that refused, not just the Reject category):

| rule | count | % of tuples |
|---|---|---|
| level_absent (PRICE_OFF 0..+10 names a level that does not exist) | 285,901 | 75.95% |
| inside_no_best (-1 add with no same-side best) | 76,072 | 20.21% |
| tail_non_add (PX_TAIL on a cancel/delete) | 4,387 | 1.17% |
| tail_no_depth (PX_TAIL add with < 11 occupied levels) | 3,193 | 0.85% |
| unparseable / invariant / absurd | 0 | 0.00% |

level_absent by requested index: adds asked for index 0 29,558 times and
ALL 29,558 failed; likewise every index 1..10 at a 100% failure rate.
Applied by kind: add 0, cancel 2, exec 6,870 (the execs "apply" against an
empty book and fill nothing, which is why signs = 0).

MECHANISM. shim::resolve maps PRICE_OFF 0..+10 to "the price of that
occupied same-side level, must exist else UnknownReference". Seeding with
two resting orders leaves ONE occupied level per side, so only indices 0
and -1 can resolve. Phase 5 measured 20.9% of panel events pricing inside
the spread, so roughly 80% of a correctly-learned add distribution targets
levels that are not there at generation start. Adds fail, deletes succeed,
the book drains - and an EMPTY book is an ABSORBING state for this shim:
every resolution rule needs an existing reference, so nothing can ever
resolve again. Death at tuple 3 is not a slow drift; it is the seed being
consumed. CST never had this problem: it places at absolute tick distances
from the touch, so it can build depth from nothing, and it got an
08:00-09:30 warm-up. The LM emits level indices, which are parasitic on
existing depth, and got no warm-up.

## Step 2 (2026-07-31): WARM START - fixes the cold start, does NOT make the real stream viable

Harness change (NOT a model or scoring change): tools/warm_book replays a
TRAIN day's pre-open flow through the SAME ingest (ingest::ingest_day +
strict ingest::replay_events) up to 09:30 and writes the resting book as a
snapshot; sim_health and shim_drive gained --warm-start to load it instead
of seeding two orders. This makes the LM's starting conditions COMPARABLE
to CST's 08:00-09:30 warm-up rather than advantaging it: the LM starts
from a real book, exactly as the null effectively did.

The real 09:30 BX book is thin: SPY 20191230 = 24 resting orders over 10
bid / 11 ask levels, best 322.93 / 323.00 (7-tick spread). It barely spans
the tokenizer's PRICE_OFF window at all.

Control re-run, SPY 20191230 real tokens, same stream both ways:

| seeding | applied | signs | first side empty | book dead at | verdict |
|---|---|---|---|---|---|
| cold (2 orders) | 7,742 (1.30%) | 0 | - | tuple 39 | NOT VIABLE |
| warm (real 09:30 book) | 12,384 (2.08%) | 37 | ask at 4,518 | tuple 8,265 | NOT VIABLE |

Warm-starting works as designed - the healthy phase is real (first 500
tuples: 82.2% applied, two-sided at 100% of checkpoints, open_orders drift
+31 per 1k tuples, V2 and V3 both PASS) - but the stream still collapses.
Per the pre-registered branch of the Step 2 instruction, that means
something deeper is wrong in the shim. It is named below.

THE DEFECT: THE PRICE_OFF INVERSE IS NOT A STRUCTURAL INVERSE.
ingest::detail::level_index records the COUNT of strictly-better occupied
levels, so "join occupied level k" and "open a NEW level just better than
occupied level k" encode to the SAME token; shim::resolve's inverse always
picks "join level k". Measured on real in-window adds (itch_tokenize now
reports this classification), TRAIN 20191230:

| symbol | adds | at-level (inverse exact) | NEW interior (inverse WRONG) | inside spread (collapsed to 1 tick) | NEW below bottom |
|---|---|---|---|---|---|
| SPY | 297,118 | 56.46% | 9.40% | 34.14% | 0.00% |
| IWM | 196,127 | 53.44% | 5.25% | 41.30% | 0.00% |
| QQQ | 216,535 | 49.44% | 6.49% | 44.06% | 0.00% |
| XLK | 89,062 | 57.67% | 3.58% | 38.74% | 0.00% |
| UVXY | 80,580 | 7.27% | 10.50% | 82.23% | 0.00% |

CONSEQUENCE - A DEPTH RATCHET. Level DESTRUCTION is unrestricted (any
delete or execution can empty any level). Level CREATION has exactly two
channels: a -1 add (one tick inside the same-side best) and a PX_TAIL add,
and PX_TAIL REQUIRES >= 11 occupied levels already. So once a side falls
below 11 levels the only way to make a new level is at the touch, while
losses happen everywhere. Observed on the real warm-started stream: the
ask side goes 11 levels -> 4 within 400 tuples while the order COUNT stays
healthy (23 -> 39); indices 8/9/10 and PX_TAIL then reject en masse
(12.4% of tuples in the first 500, all of them adds), which is exactly
what would have rebuilt the depth; ask side empty at tuple 4,518.

Boring explanations checked and ELIMINATED by measurement, not assumed:
- partial-cancel-as-full-delete: PartialCancel is 0.02% of SPY events (93
  of 595,169). Not the drain.
- add/delete imbalance in the stream itself: adds 297,118 vs deletes
  290,230, net +6,795. The stream should GROW the book.
- adds landing below the book's bottom: 0.00% on every symbol measured.
- economic-absurdity or invariant rejects: 0 in every run.

WHY THIS IS A CEILING NO TRAINING CAN REACH PAST. out/tokens/sample.py
generates autoregressively from the model's own token history alone - the
sampler passes NO book state to the model. So a trained model, like the
real stream, will request PRICE_OFF indices without knowing whether the
simulated book has that many levels. The real token stream is therefore a
fair UPPER BOUND on what any model trained on this tokenization can
achieve through this shim, and it does not clear the bar.

This does NOT touch the pre-registration: no scored fact, threshold, or
failure condition is changed by this row. It is a statement about the
generation harness, recorded before any model was scored.

Honest scope note: replaying a fixed real token stream open-loop is not
literally "a perfect model in closed loop" - once the simulated book
diverges from the real one, recorded level indices refer to a book that no
longer exists. Two things keep the control load-bearing anyway: divergence
cannot explain a death at tuple 3 (Step 1), and the divergence is itself
STARTED by the defect above (the first misplaced interior add).
