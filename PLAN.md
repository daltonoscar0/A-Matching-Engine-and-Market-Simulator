# PLAN - Exchange

## Phase 1: Matching engine core (weeks 1-3)
- [x] Order book data structure (price levels, FIFO queues); add/cancel/replace/execute
- [x] ITCH-style binary message codec + fuzz tests (A, F, E, C, X, D, U)
- [x] Property tests: no crossed book, FIFO within level, share conservation,
      unknown/duplicate-id rejection; randomized fuzzer checks all four after
      every message (1M-message runs clean, `FUZZ_N=1000000 ./tests "fuzz*"`)
- [x] BENCH.md: msgs/sec + p50/p99 - closed 2026-07-30 with a real-data row:
      full NASDAQ BX ITCH day, 23.8M book msgs across 7497 symbols (plus the
      earlier synthetic rows for deep-book stress).
Milestone: MET 2026-07-30 (revised) - replayed a full real ITCH day
(NASDAQ BX 2019-07-30) with zero rejects, zero invariant violations, exact
share conservation, and every book draining to open=0 at the close. The
original wording ("LOBSTER day byte-identically") is unmeetable as stated:
LOBSTER is purchase-gated now, and raw ITCH has no reference orderbook file
to be byte-identical against, so the correctness bar became internal:
a correct book applying genuine ITCH produces zero rejects (see Decisions).

## Phase 2: Generative agent loop (weeks 4-6)
- [x] Adapter: orderflow-lm emits messages -> engine executes -> state feeds
      back. src/adapter.{hpp,cpp} (2026-07-30): validate -> route
      (match_submit for marketable, add for resting, remove for cancel) ->
      top-N BookView feedback. Rejections classified + counted:
      Unparseable / UnknownReference / InvariantViolation / EconomicallyAbsurd.
- [x] Sampling controls (temperature, top-k) + rejection of malformed
      messages. sample_index() (temperature + top-k over model logits,
      seeded RNG owned by the adapter so the engine stays deterministic);
      malformed-message rejection is the validation front-end above.
Milestone: MET 2026-07-30 - tests/test_adapter.cpp runs 50k generated steps
with zero invariant violations (audit clean throughout, invariants_fast
after every action applied OR rejected), every rejection category fires on a
hand-built bad action, rejection is bit-identical-total, and a valid stream
routes to the same book as a direct construction. bench_adapter: 3.38M
steps/sec incl. feedback construction (BENCH.md, not comparable to
replay/match).

## Phase 3: Stylized-fact validation (weeks 7-9)
- [x] "Real" column: tools/stylized computes fat tails, aggregational
      Gaussianity, return ACF, volatility clustering, order-flow sign ACF
      from the replayed BX day (RESULTS.md "Stylized facts - real column",
      2026-07-30). Built before any model exists, so the baseline is not
      under pressure to agree with anything.
      Still needed before it can carry the headline comparison: more days
      (one day = one draw), and a deeper venue or consolidated feed - BX
      executes too little for the Lillo-Farmer lag-1000 flow memory
      (max 2945 market orders per symbol-day) and its thin top-of-book
      pollutes tail estimates on some symbols.
      2026-07-30 update: the "more days" gap is closed as far as this venue
      allows - the "real" column is now cross-day medians over the 7
      TRAIN+VAL days (RESULTS.md), with per-day spread reported. The split
      is mechanical (src/dataset.hpp); TEST = {20181228, 20200130} is
      sealed until the final comparison (see Decisions).
- [x] Fat tails (return kurtosis), volatility clustering (ACF of |r|),
      order-flow autocorrelation, square-root impact fit - for the LM sim
      and null columns once Phase 2 exists.
      CLOSED 2026-08-03, with three of the four resolved and one dropped:
      * NULL column - DONE (7 seeds, TRAIN-calibrated, all four facts).
      * LM column - the tooling exists (tools/lm_sim + stylized) and was
        exercised, but NO STYLIZED FACTS ARE PUBLISHED FOR THE LM: its
        stream is NOT VIABLE 8/8 at day scale, and computing stylized
        facts on a one-sided or dead book would be measuring the harness,
        not the model. The LM result is the VIABILITY result.
      * Volatility clustering - DEMOTED to a documented limitation
        (Decision 2026-08-03): the pipeline destroys it and the ablation
        that would have shown why is degenerate by our own rule.
      * SQUARE-ROOT IMPACT FIT - DROPPED, explicitly rather than quietly.
        It was listed here in week 7 and then never entered the
        2026-07-30 pre-registration, which fixed the scored facts as flow
        memory + persistence and the four sanity checks. It was therefore
        never a scoring criterion, and BX executes far too little to fit
        an impact curve per symbol (max 2,945 market orders per
        symbol-day; 19 of 49 symbols cannot even reach 500 signs). Adding
        it now, after results exist, would be exactly the target-moving
        the pre-registration forbids. Recorded as not done and not
        claimed.
- [x] Compare vs Cont-Stoikov-Talreja null model - null COLUMN done
      2026-07-30 (src/cst.hpp, tools/cst_calibrate + cst_sim; calibrated on
      TRAIN, 7 seeds, same stylized pipeline). Verdict: flow-sign memory
      (ACF(1) + slope) and volatility clustering discriminate; fat-tail
      levels, kurtosis-decay shape, Hill, and the bounce sign come free
      from book mechanics and are demoted to sanity checks (RESULTS.md
      null-column section). The real-vs-LM-vs-null table still waits on
      Phase 2.
Milestone: table of stylized facts, real vs LM-sim vs null - the headline
result. NOT MET, and closed as NOT MET on 2026-08-03 rather than left open:
the table needs an LM column, and no model produced a viable stream to build
one from. What the project publishes instead is the NEGATIVE result - a
validated apparatus, a reachable bar, and a model that fails it in a located
way - with TEST unspent and available to a future model. See docs/CLAIM.md.

## Phase 4 (stretch): Execution agent
- [ ] Almgren-Chriss baseline; RL or policy-gradient agent inside the sim

## Status
- Read this cold (2026-08-03). SUPERSEDES everything below, which is kept
  for the record.
- THE HEADLINE, and it is a NEGATIVE RESULT that is now solid enough to
  publish: the 32,000-step model is NOT VIABLE, 8 of 8 streams, and the
  failure is CHARACTERISED - the book decays MONOTONICALLY with stream
  length (best stream: 98.5% applied / 100% two-sided at 50k tokens ->
  60.1% / 23.0% at 300k). More sampling makes it worse, so there is no
  length at which it passes.
  MEASURED AT DAY SCALE, run completed: 1,070,002 tokens x 8 streams,
  ~214k tuples each, 34.1 min on CPU. NOT VIABLE 8/8. Best two-sided 6.4%
  (down from 17.2% at 400k - the decay continues), 3 books dead, and the
  s3 curve is monotonic across five and a half doublings: 100.0% ->
  69.0% -> 34.5% -> 23.0% -> 17.2% -> 6.4% two-sided.
  V1 PASSED FOR THE FIRST TIME EVER (s3, 536 signs >= 500; four more
  streams at 366-454). The bar was never unreachable - it was never
  TESTED, because it needs ~0.5M+ tokens/stream and no run before tonight
  had them. The "V1 is not testable" caveat standing since 2026-07-31 is
  retired, and the threshold looks calibrated rather than arbitrary.
  So the failure is precisely located: the model generates enough ACTIVITY
  at day scale but cannot maintain a TWO-SIDED book, and the gap widens
  the longer it runs.
- THE CONTROL THAT MAKES THAT TRUSTWORTHY: the REAL BX token stream
  through the same harness IMPROVES with length (99.77% applied, 100%
  two-sided, 1,844 signs at 213,754 tuples) and would PASS all three
  criteria. So the harness is exonerated, the bar is reachable, and the
  repaired shim is validated at day scale for the first time.
- WHAT THAT INVALIDATES: every earlier viability verdict here was taken at
  SHORT stream length, which systematically flatters the model (at 50k
  tokens the model gives 60 signs vs the real stream's 73 - nearly
  indistinguishable). The 2026-07-31 "5 of 8 keep the book alive" row and
  this session's 60k-token comparison are length artifacts.
- TEST IS STILL SEALED. The user authorised the sealed run and asked to
  proceed; the prior gate in that same Decision fired instead (no viable
  stream = no LM column). Two independent blockers: viability, and the
  unsettled second scored fact.
- SECOND SCORED FACT STILL OPEN. The order-identity ablation
  (tools/ablate --mode noref, new) is INCONCLUSIVE: both variants are
  degenerate by this project's own vartop10 >= 0.5 measurability rule.
  It blocks the seal independently of the model.
- TOOLING BUILT TONIGHT: pylm/kvcache.py (KV cache, 4.7x, with its
  semantic cost documented), all-seeds-in-one-batch sampling with per-seed
  RNG streams, sample.py checkpointing (a kill no longer costs the run),
  tools/ablate --mode noref. Gates green.
- ENVIRONMENT: Anaconda was deleted while freeing disk; everything now
  runs on /usr/bin/python3 (torch 2.4.1). CPU is 10.8x FASTER than MPS for
  this 227k-param model - the device default was never measured and was
  worth more than the cache.
- THE MECHANISM IS OPEN, and one attractive explanation was TESTED AND
  REFUTED the same night rather than left standing. Hypothesis: generation
  is open-loop (the model sees no book state, yet PRICE_OFF is a level
  index RELATIVE to the book), so errors compound and retraining would not
  help. Test (no new code): cut one stream into consecutive 50k slices and
  replay each from a FRESH real book - if only book drift were at fault,
  every slice would score like the first. It does not: applied runs 98.5%,
  66.9%, 52.6%, 79.2%, 13.5%, 49.7%, 98.5%, 15.1%. A healthy book does not
  rescue most slices, so the strong hypothesis is dead and the claim that
  retraining cannot help is WITHDRAWN.
  WHAT IT SHOWED INSTEAD - INTERMITTENCY. The slice at tokens 300k-350k
  scores 98.45% applied / 100% two-sided, as good as the opening slice.
  The model still emits real-quality flow deep into a stream; it does not
  SUSTAIN it. Local quality oscillates 13%-98% within one stream, while
  the FULL-stream curve decays monotonically - reconciled by the one-way
  book ratchet already on record (level destruction unrestricted, creation
  only at the touch): a bad stretch permanently thins the book, a good
  stretch cannot rebuild it, so the decay is the INTEGRAL of the damage,
  not a trend in the tokens.
  Caveat kept with the finding: the slice test conflates token quality
  with model-book/real-book mismatch (PRICE_OFF is relative), so it
  refutes cleanly but proves nothing positive. The confound-free signal is
  the VARIANCE across slices.
- FOUR CANDIDATE MECHANISMS ARE NOW ELIMINATED BY MEASUREMENT, which is
  the real progress of the night even though each elimination retracted
  something I had just asserted:
  (a) NOT the harness - real streams pass at day scale on both a VAL day
      (99.77% applied, 1,844 signs) and a TRAIN day (99.90%, 4,137 signs).
  (b) NOT book drift alone - replaying 50k slices from a FRESH real book
      does not rescue most of them.
  (c) NOT the TYPE mix - ADD/CANCEL/DELETE proportions match real and are
      INDISTINGUISHABLE between the best (98.5% applied) and worst (13.5%)
      slices.
  (d) NOT the PRICE_OFF marginal - the model matches its TRAIN corpus
      (TV 0.107) and the real TRAIN day with the MOST extreme
      touch-concentration in the corpus (PX+0 0.558, beyond the model's
      0.465) is fully viable at 2.98M tokens.
  WHAT REMAINS, unmeasured: the CONDITIONAL structure - which action at
  which level GIVEN the current book, and sequence-level dependence that
  first-order marginals cannot see.
- A SPLIT PROBLEM SURFACED, and it is about the SPLIT, not the model:
  TRAIN and VAL differ sharply on PRICE_OFF (TV 0.272), VAL's PX+0 (0.254)
  lies OUTSIDE the range of all six TRAIN days (0.315-0.558), and the
  TRAIN days themselves span that whole range. The days are far from
  exchangeable on this statistic. If the TEST days differ from TRAIN as
  much as VAL does, the headline comparison would score the model on a
  distribution it never saw. BETTER ASKED BEFORE THE SEAL IS BROKEN THAN
  AFTER - and answerable without unsealing, since it is a question about
  the TRAIN/VAL corpus bins.
- THE PROJECT IS FINISHED ON THE NEGATIVE RESULT. Option (a) chosen by the
  user 2026-08-03: publish what exists rather than attempt a fix first.
  Closed out: Phase 3 milestone recorded as NOT MET with the reason;
  square-root impact fit DROPPED explicitly (never in the
  pre-registration, and BX cannot support it); scored fact 2 DEMOTED to a
  documented limitation; docs/CLAIM.md rewritten around the negative
  result; BENCH.md row for the sampler. TEST NEVER UNSEALED and therefore
  still available to a future model - which is worth more than a table
  published now.
- IF SOMEONE PICKS THIS UP LATER, the target is narrow and named: the
  model generates enough ACTIVITY (V1 passes) but cannot hold a TWO-SIDED
  book, and the marginals are ruled out, so the failure is in the
  CONDITIONAL structure - which action at which level GIVEN the book. Two
  cheap experiments before any retrain: (1) close the loop - the Phase 2
  adapter already exposes top-N BookView feedback and the sampler does not
  use it; (2) test whether the shim's one-way ratchet is repairable, since
  depth creation happens only at the touch, which is what makes book
  damage permanent. Neither needs a training run.
- ALSO UNRESOLVED, and it should be settled before any future seal: the
  split is not exchangeable on PRICE_OFF (leave-one-out TV 0.082-0.288
  across TRAIN+VAL; VAL outside the TRAIN range entirely).
- Read this cold (2026-08-02, session ended by the user to resume tomorrow).
  SUPERSEDES the 2026-07-31 status below, which is kept for the record.
- THE BUDGET TRAINING RUN IS DONE. 32,000 steps, VAL held-out 0.9249,
  train-sample 0.9045, gap +0.0204 (out/tokens/run32k/budget32k_v2.pt).
  The fourth kill was NOT memory pressure: train_corpus.py and sample.py
  imported `tape` from a per-session /private/tmp scratchpad that is wiped
  between sessions. Repointed at ~/tape and verified identical (strict
  state_dict load + exact corpus counts). The final checkpoint carries NO
  optimizer state, so 32k is terminal - extending means starting over.
- THE HEADLINE TABLE WAS BLOCKED ON A MISSING TOOL, now built. tools/lm_sim
  turns a token stream into an ITCH file so tools/stylized can score the LM
  the same way real and null were scored; tools/ablate isolates which
  quantization does what. Both are committed (c118506) with all three gates
  green.
- WHAT THE PIPELINE DOES TO THE SCORED FACTS, measured before any model
  output existed (RESULTS.md Steps 8 and 9): flow-sign memory SURVIVES
  (-0.881 -> -0.917), volatility clustering DOES NOT (tick ACF 0.503 ->
  -0.000). The ablation control reproduces the real column to 3 decimals,
  and `pxadd` - the vocabulary's loss alone - leaves BOTH scored facts
  standing, so the 52-id vocab is exonerated and the keep-it decision now
  rests on evidence. Leading hypothesis for the real cause, UNTESTED: the
  shim has no order identity, so a cancel takes a level's FIFO head rather
  than the order the real stream cancelled.
- WHERE IT STOPPED: sampling from the 32k checkpoint, killed by the user at
  100,000 of 500,000 tokens. sample.py writes only at the END, so nothing
  was kept - a restart starts over. Throughput 33 steps/s => ~4.2 h for
  500k x 8 streams. RESTART COMMAND:
    caffeinate -i /opt/anaconda3/bin/python3 out/tokens/sample.py \
      --ckpt out/tokens/run32k/budget32k_v2.pt \
      --out-prefix out/tokens/sample32k/T1.0_k0 --n-tokens 500000 \
      --batch 8 --temperature 1.0 --top-k 0 --seed 0 --ticker SPY
- NEXT, in order, once those tokens exist:
  1. Viability warm-started (build/sim_health --warm-start
     out/tokens/warm/SPY_20190730_0930.book) - the FIRST viability verdict
     on the repaired decoder; every earlier model row is on the old one.
  2. The LM column: build/lm_sim on the sampled tokens -> build/stylized,
     then the real vs LM vs null table. Score on flow-sign memory ONLY;
     report volatility clustering as a pipeline ceiling, not a model result.
  3. The sealed TEST run on {20181228, 20200130} - AUTHORISED by the user
     2026-08-02, but LAST, and it goes back to them if the stream is not
     viable (see the Decision).
- TWO THINGS FOR THE USER, both raised and neither actioned: the LM column
  will be ~half day-scale (100k tuples vs a real day's 213k) unless the
  sampler gets KV caching (~5-20x, but it edits ~/tape and needs a
  sample-identity check); and out/ is GITIGNORED, so train_corpus.py and
  sample.py - today's fix included - are not under version control, which
  is how the scratchpad-path bomb survived.
- Read this cold (2026-07-31, second session of the day). Everything up to
  the model is DONE: engine, measurement apparatus, null column, ingest
  (round-trip tested), shim, stationarity diagnostic with a pre-registered
  viability bar, the 88.4M-token TRAIN corpus (76 bins) + 13 VAL bins, the
  architecture decision, and now a WARM-START path so generation can begin
  from a real book.
- THE ONE HEADLINE FACT A COLD READER NEEDS, and it CHANGED today: the
  earlier headline ("no model has produced a viable stream; the book dies
  immediately with zero signs") was substantially AN ARTIFACT OF THE
  HARNESS. The cold-start control proved it: the REAL BX token stream -
  what a perfect model emits by construction - is ALSO not viable under
  the old two-order seeding, dead at tuple 3. Every viability number
  recorded before today measured initialization, not a model.
- Where it actually stands now, warm-started, on a 7,000-step checkpoint:
  STILL NOT VIABLE, 8 of 8 streams - but 5 of 8 keep the book alive for
  12,000 tuples with POSITIVE order drift, the best applies 75.6% of
  tuples and ends with 270 resting orders, and V3 passes for the first
  time. V2 (two-sided >= 90%) is the real model failure: the book grows
  one-sided. V1 (>= 500 signs) is NOT TESTABLE at the stream lengths ever
  sampled - it needs ~0.5M+ tokens per stream and has never had them.
- TWO THINGS BLOCK EVERYTHING DOWNSTREAM, both needing you (details in
  Blocked on you): (1) the training run has now been killed THREE times,
  and the cause is diagnosed - memory pressure with 379 MiB of free disk
  and no room to grow swap, not a deliberate stop; (2) the shim has a
  NAMED STRUCTURAL DEFECT that caps what any model can achieve through it
  (the PRICE_OFF inverse cannot create interior price levels, so book
  depth ratchets down). Neither is fixable by training.
- THE SEALED RUN MUST NOT BE SCHEDULED. Nothing about TEST changed; TEST
  was not read this session, not with the override.
- Next 3 tasks:
  1. Decide the shim-defect question (Blocked on you 5). Until then a
     viability verdict on any model is bounded by a harness ceiling.
  2. Complete a budget training run on a machine with headroom - it needs
     the machine to ITSELF plus disk, not a different venue (Blocked on
     you 1b). Resume support does not exist yet: train_corpus.py always
     starts from scratch, so a fourth kill costs everything again.
  3. Then viability warm-started at ~0.5M tokens/stream (so V1 is
     testable), and - only if viable - the VAL-only sampling sweep
     (out/tokens/sweep.sh, which now REFUSES to run cold-started).
## Decisions
- 2026-07-27 Catch2 v2.13.10 vendored (third_party/catch.hpp), the one
  allowed dependency. Fuzz hot loop uses raw aborts, not CHECKs - Catch2
  macro overhead would dominate a 1M-message run.
- 2026-07-27 ITCH layouts (A/F/E/C/X/D/U = 36/40/31/36/23/19/35 bytes,
  big-endian, 48-bit ns timestamps) written from the public TotalView-ITCH
  5.0 spec from memory; the PDF excerpt named in README.md was not
  available. Diff against it before Phase 1 sign-off.
- 2026-07-27 File framing is a 2-byte big-endian length prefix per message
  (BinaryFILE), not SoupBinTCP. Revisit only if we ever speak the wire live.
- 2026-07-27 The book reconstructs, it does not match. Executions arrive as
  explicit E/C events; adds/replaces priced through the opposite side are
  rejected, since crossing flow executes upstream and never rests. Phase 2's
  generative loop needs a real match() path - new code, not a change here.
- 2026-07-27 Levels in std::map (bids desc, asks asc, begin() = best),
  orders in unordered_map, intrusive FIFO per level. O(1) cancel/delete by
  id, O(log L) adds, both containers pointer-stable. 5.48M msgs/sec on the
  container; nothing fancier is justified yet.
- 2026-07-27 Conservation ledger in the book (added = resting + executed +
  canceled, O(1)) plus a deep O(n) audit(). Fuzzer runs the ledger check
  every message, audit every 4096.
- 2026-07-27 Replace ('U') = cancel remainder + add at back of queue,
  remainder counted as canceled. ITCH semantics; time priority is lost.
- 2026-07-27 stock_locate ignored in Phase 1: one Book per process.
  Superseded 2026-07-30 by BookSet.
- 2026-07-27 Generator owns a shadow Book, so valid messages are valid by
  construction; the fuzzer replays the same stream through
  encode->decode->apply into an independent Book and cross-checks state.
- 2026-07-30 Tail latency measured, not guessed: bench/tail.cpp times every
  message, tracks order-pool bucket counts, and buckets spikes per
  500k-message window to separate size-correlated causes from preemption.
  Kept in-repo so the measurement is reproducible.
- 2026-07-30 Tail fix is Book::reserve(n), called by the bench with 1<<20.
  Not in the constructor: Phase C runs many books per process, and a
  megabucket table per instrument is the wrong default.
- 2026-07-30 No slab allocator for Order/Level: it could only chase ~100us
  allocator noise on ~10 messages in 5M and cannot move p99.9 (RESULTS.md
  2026-07-30).
- 2026-07-30 BookSet is a vector<unique_ptr<Book>> indexed by stock_locate:
  a 16-bit key, so at worst 512KB of pointers buys hashless O(1) routing.
  Books created on first touch; the single-book path is untouched.
- 2026-07-30 Multi-symbol synth interleaves per-symbol Generators, one
  locate each, refs globally unique via per-symbol base ((i+1)<<40, clear of
  the 0xDEAD... unknown-id range). Unique refs make misrouting visible to
  the fuzzer as UnknownId/DuplicateId. Defaults keep single-symbol output
  byte-identical (verified by md5 of the seed-42 5M file).
- 2026-07-30 Merged multi-symbol stream is not timestamp-monotonic across
  symbols. Accepted: no consumer reads cross-symbol time order.
- 2026-07-30 Ground truth switched from LOBSTER (now gated behind an
  email + purchase-proof request flow) to a genuine NASDAQ BX ITCH 5.0 day
  (data/20190730.BX_ITCH_50, 837MB, hand-verified framing). What changes:
  LOBSTER would have given an external per-row orderbook file to diff
  against ("byte-identical"); raw ITCH has no such reference, so the
  correctness criterion becomes internal - a correct book applying real
  exchange data produces ZERO rejects, holds all invariants, conserves
  shares exactly, and (observed) drains to open=0 at the close. Stronger on
  authenticity (raw exchange feed, all symbols), weaker on independent
  cross-checking. The zero-reject bar is absolute: any WouldCross/UnknownId/
  DuplicateId/TooManyShares on real data is OUR bug, never tolerated.
- 2026-07-30 FrameReader distinguishes unknown-type (skip + per-type-byte
  histogram; a real day carries ~20 types, we implement 7) from malformed
  frame (zero length, length past buffer, known type failing decode =
  desynced stream, still fatal). Collapsing these would let a desynced
  replay "succeed" on garbage; the fuzzer still relies on malformed = fatal.
- 2026-07-30 'R' stock directory parsed via parse_stock_directory into a
  side table, NOT added to the Message variant: it names instruments rather
  than mutating a book, and keeping the variant to the 7 book types keeps
  the hot decode path and fuzz surface unchanged. The replayed file's own R
  messages are the authoritative locate->ticker map (8849 entries on this
  day); no external reference file. Locate 0 is reserved and owns no book.
- 2026-07-30 'P' (trade, non-cross) is a SKIP, not an execute: it reports
  trades against non-displayed liquidity and must not touch the visible
  book. Confirmed empirically - 244k P frames skipped and the day still
  conserves exactly and drains to zero; applying them would have produced
  an UnknownId flood.
- 2026-07-30 The ITCH-5.0-PDF spec diff (2026-07-27 decision) is closed by
  stronger evidence: 23.8M real messages decoded with zero rejects and
  exact end-of-day conservation empirically confirms the A/F/E/C/X/D/U
  layouts, 2-byte BE framing, and 48-bit timestamps against the real wire.
- 2026-07-30 (Step 1) External validation is distributional, not a state
  diff: conservation cannot catch a wrong decode offset because the same
  wrong field is added and later removed, balancing perfectly. The checks
  that CAN catch it: shares on A/F must show round-lot structure (they do:
  48.5% exactly 100, 91.5% multiples of 100), prices/1e4 must be plausible
  dollars (99.4% whole-penny, median $48.31), and top-symbol books must be
  two-sided at cent spreads all session (they are).
- 2026-07-30 (Step 2) Auction exclusion = the time filter 09:30-16:00
  alone: this BX day carries zero 'Q' (cross trade) frames in the skip
  histogram, i.e. the venue ran no opening/closing auctions, so there are
  no auction prints to strip beyond the window. All messages are still
  APPLIED whatever their timestamp (books must be correct all day); only
  the sampled series are windowed.
- 2026-07-30 (Step 2) Event time = one mid observation per applied book
  message on that symbol while two-sided; zero returns are KEPT in every
  primary series (they are what sampling a thin book produces) with the
  zero fraction reported alongside, plus a mid-change-only ("tick")
  kurtosis so the zero distortion is visible instead of silently picked.
  Hill tail index uses nonzero |r| only (zeros are undefined there),
  cutoff = top 5% of order statistics, k >= 10.
- 2026-07-30 (Step 2) ACF(r) lag-1 is negative at event scale (median
  -0.25): bid-ask bounce / quote flicker, microstructure noise, reported
  as-is and not smoothed away. ACFs are computed at event scale AND on the
  1s calendar grid; the 60s grid (390 points/day) is too short for
  lag-100 ACFs and is used for kurtosis only.
- 2026-07-30 (Step 2) Aggressive-flow signs come from E/C fills: the
  resting order's side names the aggressor (resting ask hit -> +1 buy).
  Consecutive fills with identical timestamp and sign collapse into one
  market order (one order walking the book is one decision, not many).
  BX executes so little that lag 1000 exceeds n/4 for every symbol
  (max 2945 signs/day), so the sign ACF runs to min(1000, n/4) and the
  long-memory claim rests on the log-log slope over lags 1-100.
- 2026-07-30 (Step 2) Open/close transients are kept in the primary
  numbers (they are continuous-market samples); a trimmed 09:35-15:55
  sensitivity is quoted in RESULTS.md to attribute the kurtosis blowups
  (QQQ 19340 -> 10.6, IWB 20429 -> 16.0) to the 09:30 book-population
  and 16:00 liquidity-drain windows rather than to genuine tails.
- 2026-07-30 (Step 2 corrections) 2a: ACF(|r|) on a 70%-zero 1s series
  cannot distinguish volatility clustering from bursty-activity clustering
  (quiet stretches cluster in time), so the tick series (mid-changes only,
  zeros impossible by construction) carries the discriminating version;
  both are reported. Tick lag-1 is discounted as flicker-inflated (a level
  blinking off/on gives consecutive equal |r|); lags 10-100 are the
  informative range.
- 2026-07-30 (Step 2 corrections) 2b: Hill alpha < 1 implies infinite mean
  and is therefore an estimator failure (here: stale-touch jumps dominating
  the top-5% order statistics), never a measurement. Failures are excluded
  from cross-symbol medians and reported with their reason.
- 2026-07-30 (Step 2 corrections) 2c: sign collapse is windowed, not
  exact-timestamp: one sweep's fills carry distinct ns timestamps. Window
  chosen from the measured same-sign inter-fill gap distribution on this
  day (bimodal: machine mode ~32us-1ms, decision mass >= 100ms, valley
  10-32ms) = 1ms default, on the conservative (under-collapsing) side of
  the valley so residual autocorrelation is real, not an artifact of
  merging separate decisions. --collapse-ns flag for sensitivity; result
  is robust 100us-10ms (lag-1 0.32-0.22).
- 2026-07-30 (Step 2) Limitation written down: BX is a minority venue;
  its mid can be stale or wide relative to the NBBO, and one day is one
  draw. The stylized-facts table is the facts on THIS venue THIS day - a
  first baseline, not validated empirical ground truth.
- 2026-07-30 (Step 1) Decision: orderflow-lm TRAINS ON BX FLOW. The
  stylized-facts baseline is therefore correctly aimed - it measures the
  same distribution the model will be scored on. Consequence, written down
  so nobody overstates it later: the claim this project can support is
  "reproduces NASDAQ BX order flow", NOT "reproduces market
  microstructure". BX's top of book sits several ticks behind the NBBO
  (SPY median spread 4c on this venue while the consolidated NBBO was a
  penny), so BX flow is a real but idiosyncratic slice of the market.
  Narrower, honest claim.
- 2026-07-30 (Step 1) Training set: 8 more BX days fetched (20181228,
  20190130, 20190327, 20190530, 20190830, 20191030, 20191230, 20200130 -
  every remaining downloadable day on emi.nasdaq.com; the 2018-01..10
  listings are md5-stub-only). Spread over 14 months = closer to
  independent draws than one week. Verified by gzip -t, not md5 (the
  .md5sum sidecars 404). data/ stays gitignored.
- 2026-07-30 (Step 3) match() structure: a new entry point ON Book
  (Book::match), not a wrapper class - matching needs FIFO-head and
  best-level access the public API doesn't expose, and building it from the
  validated primitives (execute() per fill, add() for the remainder) means
  the ledger and removal logic are shared, not duplicated. The
  reconstruction path is untouched; both coexist and all prior tests pass
  unchanged. Emission lives in src/match.hpp (match_submit), the mirror of
  feed.hpp: feed applies decided executions, match_submit decides and
  reports them.
- 2026-07-30 (Step 3) Fill price = the RESTING order's price, price
  improvement accrues to the aggressor (the easy-to-invert convention;
  pinned by a dedicated property test on both sides). Limit remainders rest
  at their own limit via add(); market remainders are canceled and emit
  nothing - the order never rested, so there is no 'D' to send and it
  appears in no ledger. Emitted fills are always 'E', never 'C': every fill
  here executes at the resting display price, and ITCH reserves 'C' for
  executions away from display, which this engine never produces.
- 2026-07-30 (Step 3) No self-trade prevention, logged deliberately: in a
  single-agent simulation everything is nominally a self-trade, so STP
  would either no-op or forbid all matching. This becomes a REAL design
  question if Phase 4 puts a second agent in the sim - revisit then
  (cancel-oldest vs cancel-newest vs decrement semantics).
- 2026-07-30 (Step 3) Crossing replaces are out of scope for match(): the
  Phase 2 adapter decomposes a replace that would cross into cancel + new
  aggressive order. The reconstruction path's WouldCross reject on U stays
  authoritative for resting-side modifies.
- 2026-07-30 (Step 1 follow-up) The 2c collapse window is a free parameter
  and the sweep proves it: flow-sign ACF(1) has NO machine-scale plateau
  (flat only 0-10us, then sliding ~ -0.05 per decade of window all the way
  to 100ms; out/collapse_sensitivity.csv + RESULTS.md row). Pre-registered
  interpretation applied: the lag-1 level is reported window-conditional
  (0.27 at 1ms; defensible windows span 0.32-0.17), the default stays at
  the gap-histogram-justified 1ms and was NOT tuned toward the literature,
  and model scoring must target the robust quantities - the sign and the
  log-log decay slope ~ -0.6, stable for every window <= 1ms - not the
  lag-1 level. The earlier 2c wording "robust 100us-10ms (0.32-0.22)" was
  too generous: a factor-of-2 slide is sensitivity, not robustness. Cause
  is in the gap histogram: the machine mode spans 1us-1ms and the 10-32ms
  valley is a ~35% dip, not empty - the timescales overlap, so no window
  separates them cleanly.
- 2026-07-30 (Step 2 follow-up) Evidence tools live in-repo: gap_hist.cpp
  (the measurement behind the 2c window decision) moved from a
  garbage-collected scratchpad into tools/ + CMakeLists, same reasoning
  that kept bench/tail.cpp. Verified the repo build reproduces the bimodal
  histogram. Scratchpad sweep found nothing else decision-bearing (only
  one-shot download scripts whose procedure the session log already
  records).
- 2026-07-30 (Step 3) The strong test (emit -> encode -> decode ->
  reconstruct -> byte-identical fingerprint) was mutation-verified: an
  emit bug invisible to every per-match assertion (A carrying the original
  size instead of the remainder - the per-match checks only type-check the
  A) is caught by the reconstruction replay within ~40 messages
  (WouldCross). That is the bug class this test exists for.

- 2026-07-30 (CST null) Implementation choices for the Cont-Stoikov-Talreja
  null model (src/cst.hpp, tools/cst_calibrate, tools/cst_sim), all serving
  one goal - a FAIR memoryless null, not a strawman:
  (a) Rates are EMPIRICAL per bucket, not a fitted power law: limit-order
  intensity lambda(side, d) and per-order cancel intensity theta(side, d)
  at distance d ticks from the OPPOSITE best (the CST convention), d =
  1..100, calibrated by counting events and integrating order-second
  exposure exactly between symbol messages. The null matches the marginal
  intensities as closely as the data allows and differs ONLY in having no
  memory. (b) Adds beyond 100 ticks from the opposite best are excluded
  from the model (they cannot move the mid); cancels/exposure keep a tail
  bucket so drifted orders still die. theta buckets with under 1
  order-second of exposure get rate 0 (no rate estimate from no exposure).
  (c) X, D, and the cancel half of U each count as one cancel decision;
  U's re-add counts as an add. The sim emits full deletes only.
  (d) Market-order decisions are calibrated with the SAME 1ms same-sign
  collapse stylized uses (symmetry between calibration and measurement);
  sizes are i.i.d. draws from the empirical per-decision-size histogram,
  executed through Book::match, so sweeps and partial fills are decided by
  the real engine and multi-level fills share one timestamp (stylized's
  collapse merges them back into one decision - no artificial sign memory
  from sweep-splitting, no artificial suppression either). (e) Panel = the
  13 symbols in the top-20 on >= 4 of 6 TRAIN days (IWM SPY XLK QQQ IWO
  UVXY SOXL TLT IJH XLE XLV IWN VXX), rates pooled across all 6 TRAIN days;
  VAL and TEST untouched by calibration. (f) Sim warm-up 08:00-09:30 from a
  seeded two-order book at the calibrated median spread - outside
  stylized's window, so the transient is never sampled; day ends 16:05.
  (g) Every generated stream self-verifies through the reconstruction path
  (zero rejects, audits, exact conservation) before being written - the
  same bar real data is held to. (h) The null-vs-real comparison is
  computed on the SAME 13-symbol panel (real column restricted to panel
  symbols from the existing per-day summaries) - medians over different
  symbol sets would not be comparable. (i) READING_LIST.md is referenced in
  the task but absent from the repo; implemented from the published model
  (Cont, Stoikov, Talreja 2010, "A stochastic model for order book
  dynamics", Operations Research 58(3)) - noted, not blocking.
  Pre-registered interpretation rule (fixed by the user BEFORE the run,
  restated here before results existed): if CST's flow-sign log-log decay
  slope is materially different from -0.6 (steeper than -0.9 or near
  zero), the slope discriminates and the null's value is the floor a
  generative model must beat; if CST also lands near -0.6, the Lillo-Farmer
  slope does NOT discriminate and dies as a scoring criterion - and no
  variant of the statistic gets hunted for in the same session to rescue
  it.

- 2026-07-30 (per-symbol restructure) The Phase 3 comparison is scored PER
  SYMBOL from now on; pooled cross-symbol medians are demoted to summary
  statistics. Trigger: the 2a "vol clustering survives on 11/20, vanishes
  on 6/20" split dissolved under characterisation - the vanishing was
  MEASUREMENT DEGENERACY, not absence. Rule, stated before classifying: a
  symbol-day's tick ACF(|r|) is UNMEASURED when vartop10 (share of the
  centered sum of squares of |r_tick| in its 10 largest terms) >= 0.5,
  because ten points carrying half the variance make the ACF an
  outlier-placement statistic; 0.5 is a judgment call and the P/A/U counts
  are reported at 0.3/0.5/0.7 (the ABSENT set stays tiny under all three).
  The thinness hypothesis was tested and FAILED: zeros at n_tick up to
  250k, detection at n_tick 11.8k, detectability floor 0.021 << typical
  effect 0.16 - there is no n threshold and n is not the binding
  limitation. tools/stylized gained per-symbol covariate columns (appended
  after existing ones: n_msgs_w, twosided_frac, med_spread_ticks, med_mid,
  rel_tick_bp, n_exec, cv_absr_tick, vartop10_absr_tick) so the
  classification is reproducible from the summary CSV alone. Consequence
  for scoring the LM: per-symbol comparison, the measurability rule applied
  identically to model output, no credit or penalty on unmeasured
  symbol-facts (19 symbols unmeasurable for flow memory on BX, pinned
  names unmeasurable for tick clustering). A model matching pooled medians
  while wrong per-symbol must fail, and now can.

- 2026-07-30 (split guard, trainer side) Audited ~/orderflow-lm end to end
  for how it selects input files. Findings: EVERY file is chosen by an
  explicit CLI path argument (tokenize_main/fit_main/validate_book/
  book_source_compare all take positional <messages.csv> <orderbook.csv>)
  or pinned in data/tokens/manifest.json (LOBSTER paths only). The one
  enumeration is scripts/fetch_lobster_sample.sh, hardcoded to LOBSTER
  2012-06-21 AAPL/MSFT/SPY. There is NO glob over a data directory, NO
  hardcoded day list in code, and - the load-bearing fact - NO ITCH/BX
  data-loading code of any kind: data/itch/ holds only .gitkeep, the parser
  is LOBSTER CSV, and nothing in the repo can open a .BX_ITCH_50 file. The
  "split" tokens in that repo are the tokenizer's chronological train/eval
  time split within one file, unrelated to our TRAIN/VAL/TEST day split.
  Conclusion: a BX TEST day is currently unreachable from orderflow-lm
  because BX data is unreachable, period - there is no bypass to close
  because there is no loader. Per the standing rule (don't invent a fix for
  code that doesn't exist), the requirement is RECORDED instead: when the
  ITCH ingest is written (SPEC Phase 1 "then NASDAQ ITCH 5.0"), it MUST
  obtain its day list through this repo's src/dataset.hpp - which is
  header-only and stdlib-only, so orderflow-lm can #include it directly (a
  symlink or a git submodule of exchange, NOT a transcribed copy that can
  drift) - and call dataset::enforce() before opening any day, exactly as
  tools/stylized etc. do. One source of truth: src/dataset.hpp. A matching
  note was added to orderflow-lm/SPEC.md so the requirement lives in that
  repo's source of truth too.

- 2026-07-30 (format reconciliation) Audited the orderflow-lm/Scalpel
  factored tokenizer against BX ITCH 5.0; full write-up in
  docs/FORMAT_RECONCILIATION.md. Headline: the tokenizer SURVIVES with no
  vocabulary redesign, because its two most format-sensitive choices already
  insulate it - PRICE_OFF is a signed occupied-LEVEL INDEX (not an absolute
  price or a fixed grid; the tick-offset scheme that WOULD have broken was
  already dropped in v2), and order-reference identity is DROPPED (so ITCH's
  64-bit sparse refs never threaten the vocab; the engine owns refs). Bounded
  scope: (1) a new ITCH-driving adapter that parses BX, drives the exchange
  reconstruction, and emits [TYPE][SIDE][PRICE_OFF][SIZE][DT] with PRICE_OFF
  read from the reconstructed book (LOBSTER used its orderbook file; SPEC
  already anticipated reconstruction for ITCH) - NOT a tokenizer rewrite;
  (2) refit the frozen SIZE and DT bins on BX TRAIN and re-measure the
  PRICE_OFF window / "-1-only" assumption on BX (retraining decisions, not
  code); (3) one design fork - ITCH 'U' has no LOBSTER equivalent: expand to
  Delete+Add in the adapter (recommended, no vocab change, matches both
  LOBSTER and our reconstruction) OR add a TYPE_REPLACE token (vocab change +
  retrain). Other type maps are adapter bridges (F->Add drop MPID, C->Exec
  price already dropped by the level-index scheme, P/Q/H as skips/specials).
  What does NOT transfer: the LOBSTER-SPY trained weights and frozen bins - BX
  is a different distribution, so Phase 3's LM column is a from-scratch BX
  run. NOT starting the tokenizer change this session; the audit is the
  deliverable, direction is the user's to pick.

- 2026-07-30 (adapter) State-feedback shape: the model conditions on the
  top-N aggregated levels per side (price + total shares) plus best bid/ask
  and spread, default N=11. Reasoning logged because it determines what the
  model can condition on: N=11 covers the tokenizer's PRICE_OFF window
  (occupied levels 0..+10 on the event side, docs/FORMAT_RECONCILIATION.md),
  so the model's conditioning state and its emittable price range align.
  Full depth (BX peaks ~87 levels) would waste per-step work; a scalar
  spread/imbalance summary would under-determine PRICE_OFF. Configurable via
  AdapterConfig.feedback_levels.
- 2026-07-30 (adapter) The boundary is at the concrete order-action level
  (absolute price/side/size), NOT the factored-token level: the token<->price
  decode and the 'U' representation fork are still-open tokenizer decisions
  (FORMAT_RECONCILIATION), and the engine loop must not bake them in. A thin
  token<->action shim will sit above this once the tokenizer direction is
  chosen.
- 2026-07-30 (adapter) A Cancel's `shares` field is unused - it removes the
  FIFO-head resting order at (side, price) in full - so only Limit/Market
  require a positive size; a zero-size Limit/Market is Unparseable. Crossing
  limits are NOT rejected: a limit priced through the opposite touch routes
  to match() (marketable), only away-limits rest, so WouldCross never fires
  from the adapter. Economic-absurdity bounds (size > 1M shares, price >
  $10k absolute, or > 50% off the opposite touch) are applied BEFORE the
  engine, since the engine would accept them; thresholds are AdapterConfig
  fields and are training diagnostics, not correctness limits.

- 2026-07-30 PRE-REGISTRATION of the headline real-vs-LM-vs-null table.
  Written and committed ALONE, before any LM exists, so the timestamp
  establishes it predates the model. This is the last unwritten
  methodological choice and the one where the temptation to move the target
  will be highest, so it is fixed now. Nothing below may be revised once an
  LM has produced TEST output except by a new dated Decision that names this
  one and states what changed and why.

  (a) DATA PER COLUMN. All three columns are computed on the sealed TEST
  set, and only TEST:
    - real-TEST: tools/stylized run directly on the 2 TEST days
      (20181228 high-vol, 20200130 calm), via the override flag. Measured,
      not simulated.
    - null-TEST: CST calibrated on the 6 TRAIN days ONLY (already done,
      out/cstnull/calib_*.csv), simulated fresh, run through the identical
      stylized pipeline. The null is a TRAIN-calibrated memoryless floor
      evaluated against TEST reality - it never sees TEST. 7 seeds.
    - LM-TEST: model trained on TRAIN, all hyperparameters AND sampling
      temperature/top-k chosen on VAL (20190730) - never TEST - then sampled
      fresh and run through the same stylized pipeline. TEST real flow is
      never shown to the model. >= 7 sampling seeds. The LM's data loader
      MUST call dataset::enforce(); TEST is reachable only in this one final
      pass.
    Justification for all-on-TEST: the question is whether the LM reproduces
    HELD-OUT reality, not TRAIN reality it could memorise; the null and LM
    are both TRAIN-fit and TEST-evaluated, so the three columns are compared
    on the same held-out draws.

  (b) DAYS / SEEDS / SPREAD. 2 TEST days, scored per day (NOT pooled: the two
  are deliberately different regimes). Null 7 seeds, LM >= 7 seeds. Per
  (symbol, fact) the reported statistic is the cross-seed / cross-day MEDIAN;
  the seed spread is reported as a min-max envelope and is ESTIMATOR NOISE,
  not structure (the flow slope on ~1k signs fits the log of sampling
  noise - CST seed row confirms this), and is used only to build the null
  envelope in (e), never interpreted as a finding.

  (c) SCORING FACTS. Locked scoring fact: FLOW-SIGN MEMORY - per-symbol
  ACF(1) (1ms collapse) and log-log decay slope over lags 1-100. Conditional
  second scoring fact: TICK-TIME VOLATILITY-CLUSTERING PERSISTENCE (per-symbol
  tick ACF(|r|) at lags 50 and 100), included as a SCORED fact only if the
  user ratifies it per the Step 1 correction (the lag-10 level overlapped;
  only persistence separated cleanly); until ratified it is a sanity check.
  SANITY CHECKS, explicitly NEVER scoring criteria (the memoryless null
  already reproduces all four, RESULTS null column): fat-tail kurtosis level,
  aggregational-Gaussianity decay shape, Hill tail index, ACF(r) bounce sign.
  A gross failure of a sanity check (LM produces no fat tails, or
  infinite-variance nonsense, or a one-sided/empty book) is a basic-validity
  failure noted separately; PASSING a sanity check earns the LM nothing.

  (d) MEASURABILITY. The identical rule is applied to ALL THREE columns:
  flow memory scored only where >= 500 collapsed signs on that TEST day;
  vol-clustering persistence scored only where vartop10_absr_tick < 0.5 and
  n_tick >= 1000. Each fact is scored on the symbols where THAT fact is
  measurable (per Step 2: the two measurable sets are anticorrelated, so the
  scored set differs by fact and is NOT their intersection). An
  (LM,symbol,fact) cell unmeasurable on either the LM or the real column is
  excluded - neither credit nor penalty. The scored symbol set per fact is
  named in the published table.

  (e) LM BEATS THE NULL (per symbol, per fact). Build the null envelope for
  each measurable (symbol, fact) = [min, max] of the null's per-seed values
  on that TEST symbol. The LM BEATS the null on a symbol for flow memory iff
  its cross-seed median ACF(1) AND slope BOTH land strictly on the real side
  of the null envelope AND clear an absolute floor that noise cannot:
  ACF(1) > null_max AND ACF(1) >= 0.10; slope < null_min AND slope <= -0.30.
  Aggregate: the LM "beats the null on flow memory" iff it beats on BOTH
  quantities for >= 2/3 of the flow-measurable TEST symbols. A STRONGER,
  separately-reported bar - "reproduces real" - additionally requires the
  LM's per-symbol value to fall within the real TEST symbol's value +/-
  tolerance (0.10 on ACF(1), 0.20 on slope). Beating the null is the pass
  bar; reproducing real is the stretch bar; both are reported.

  (f) LM FAILS (the falsifiable sentence, verbatim):
  "The LM FAILS the headline comparison if, on the flow-sign memory fact,
  its per-symbol ACF(1) and log-log decay slope fall inside the CST null's
  seed-envelope - not on the real side of it - for at least half of the
  flow-measurable TEST symbols; that is, if it is not reliably
  distinguishable from a memoryless generator on a majority of the symbols
  where the fact can be measured. Reproducing the pooled cross-symbol median
  while failing this per-symbol bar is also a failure, not a partial pass."

  (g) SEAL PROTOCOL. TEST is read EXACTLY ONCE, via
  --i-am-running-the-final-comparison, and all three columns are computed in
  that single pass. The numbers are published whatever they say - a failing
  LM is a publishable result. No re-runs, no "let me just check one thing,"
  no re-tuning after seeing TEST. If a genuine bug is discovered after the
  seal is broken, unsealing again requires a new dated Decision that names
  this pre-registration, states the bug, and states why a re-run is not
  target-moving - it is not a silent redo. Until the final pass, 20181228
  and 20200130 stay sealed for every purpose.

- 2026-07-30 (U fork, quantified) Measured `U` on 3 TRAIN days (replay_itch):
  5.9% of book messages pooled (4.6-8.7% by day), NOT the ~12% the task
  estimated - that 12% matches the post-expansion figure (D/A pair-halves =
  11.2% of the expanded event stream). Delete+Add expansion lets the model
  emit unpaired deletes/adds that real (atomic-replace) BX flow never
  contains, and the adapter CANNOT enforce pairing (both halves decode to
  plain D/A with no marker) - but each half is individually valid so the
  book stays correct, and the cost is confined to cancel/replace TIMING, a
  sanity-check quantity: replaces generate no aggressor signs, so flow-sign
  memory (the scored fact) is untouched. TYPE_REPLACE does NOT absorb into
  the factored 5-tuple (a replace has two locations - original + new - vs one
  PRICE_OFF slot), so it forces a structural tuple change, not just a vocab
  add; retrain is required either way (bins refit). Recommendation logged in
  docs/FORMAT_RECONCILIATION.md: EXPAND to Delete+Add; revisit TYPE_REPLACE
  with a 6-field tuple only if replace-timing fidelity later matters for a
  probe. Direction is the user's; not implemented.

- 2026-07-30 (Step 2, ratification) TICK-TIME VOLATILITY-CLUSTERING
  PERSISTENCE is RATIFIED by the user as the SECOND SCORED FACT of the
  headline comparison, upgrading it from the conditional status in the
  2026-07-30 pre-registration (c). Gate check first: the ratification was
  explicitly conditioned on the vartop10 sample-size confound resolving
  (RESULTS.md 2026-07-30 "Is vartop10 confounded by sample size?"), and it
  resolved on branch 1 - no n gap (null n_tick is SMALLER than real, 0.71
  pooled), no n dependence of vartop10 (rho -0.08, R^2 0.02) - so the
  lag-100 disjointness is a fact about book dynamics, not arithmetic, and
  ratification proceeds. Conditions of record, imposed so that the
  post-hoc selection of the load-bearing lag is VISIBLE rather than
  laundered:
  (2a) BOTH lag 10 and lag 100 of tick ACF(|r|) are SCORED per measurable
  symbol; lag 50 is computed and reported unscored. Lag 100 alone is not
  the fact.
  (2b) Pre-registered PREDICTION, recorded before any LM output exists:
  "lag 10 is expected NOT to separate LM from null, on the basis of the
  CST comparison where real and null lag-10 ranges overlapped; lag 100 is
  expected to separate. If the LM separates at lag 10, that is a finding
  beyond what the null comparison predicted and must be reported as such."
  This makes the outcome-dependence of the original selection part of the
  record, and gives the fact something it can fail at.
  (2c) Stated plainly: lags 50/100 were in the pre-specified clustering
  statistic from the v2 table onward - they were not invented after the
  fact - and what changed post-hoc was only WHICH lag is treated as
  load-bearing. Both halves of that sentence matter: the statistic was
  pre-specified, and the emphasis within it was outcome-dependent.
  Aggregation into the fact-level verdict (implementation call, Claude's,
  logged for veto): the persistence fact's PASS rests on the lag-100 beat
  count (the lag the null comparison predicts separates), with the lag-10
  beat count reported alongside and interpreted via 2b - lag-10
  separation, if it occurs, is a beyond-prediction finding, and lag-10
  non-separation is the predicted outcome, not a failure. Requiring a
  lag-10 beat for the pass would contradict 2b (the fact would be
  pre-registered as expected to fail); scoring lag 100 alone would hide
  the selection 2a exists to expose. Absolute floor for the lag-100 beat,
  added on the same reasoning as the flow-memory floors (an envelope near
  zero must not be beatable by noise): tick ACF(|r|) lag-100 >= 0.015,
  the minimum across all 12 real PRESENT panel symbols in the CST
  comparison (RESULTS.md measurable-only table; null max was 0.007).
  Set before any LM output exists.

- 2026-07-30 AMENDMENT to the pre-registration (commit c06df11, the
  2026-07-30 "PRE-REGISTRATION of the headline real-vs-LM-vs-null table"
  Decision). Per that Decision's own protocol: a new dated Decision naming
  the original, stating what changed and why; the original text is NOT
  edited. Both defects are fixed BEFORE any LM output exists - that is the
  point of doing it now.

  DEFECT 1 (envelope is seed-count dependent). The original (e) built the
  null envelope as [min, max] over null seeds. The min-max of k draws does
  not converge as k grows - it diverges, so adding seeds makes the LM's
  bar strictly harder; worse, the original fixed the null at 7 seeds while
  allowing the LM ">= 7", an asymmetry in the same direction. FIX:
  (i) The null envelope per measurable (symbol, quantity, TEST day) is now
  the 10th/90th PERCENTILE of the null's 7 per-seed values. Quantile
  convention pinned (there are several): linear interpolation between
  order statistics at position p*(k-1)+1, so at k=7 the 10th percentile is
  x(1) + 0.6*(x(2) - x(1)) and the 90th is x(6) + 0.4*(x(7) - x(6)).
  Quantiles chosen over mean +/- 2 SD, argued: the null slope's per-seed
  distribution is a nonlinear fit of sampling noise and visibly skewed
  (seed range [-0.229 .. 0.003], median -0.018), so a symmetric +/-2SD
  band puts mass where the distribution has none; an SD estimated from 7
  points is itself unstable; and quantiles are distribution-free, bounded
  by the support, and CONVERGE in k instead of diverging. Honest note: at
  k=7 the interpolated [q10, q90] sits numerically close to [min, max]
  (1.6th/6.4th order statistics), so today's bar barely moves - what the
  fix changes is the RULE: more seeds now stabilizes the envelope instead
  of widening it.
  (ii) LM sampling seeds = EXACTLY 7, the null's count (was ">= 7").
  Symmetric by construction; raising both counts together would require a
  new dated Decision and, after the seal is broken, is barred by (g).

  DEFECT 2 (pass and fail bars leave a gap). Beating needed >= 2/3 of
  measurable symbols; failing needed >= 1/2 inside the envelope; an
  outcome like 60% beating / 35% inside was neither - and an ambiguous
  zone is where interpretation creeps in once results exist. FIX: an
  explicit INCONCLUSIVE verdict, chosen over forcing the two conditions
  complementary, argued: complementarity (FAIL = not PASS) would erase a
  distinction the data can express - an LM beating on 60% of symbols and
  an LM sitting inside the envelope on 90% are different results, and
  collapsing both to FAIL just relocates the interpretation creep into
  prose ("failed, but only just"). Instead the verdict space is a
  three-way PARTITION, mutually exclusive (2/3 + 1/2 > 1) and now
  exhaustive, with the ambiguous zone pre-committed to a conservative
  reading rather than left to future judgment:
  - PASS: beats (per the amended beat condition) on >= 2/3 of that fact's
    measurable TEST symbols.
  - FAIL: all scored quantities inside their [q10, q90] envelopes on
    >= 1/2 of measurable symbols.
  - INCONCLUSIVE: neither. Reporting requirement: published with the full
    per-symbol table (counts of beat / inside-envelope / neither-side,
    i.e. real-side-but-below-floor or wrong-side-of-envelope), and the
    headline claim MUST use the conservative reading - an INCONCLUSIVE
    fact is reported as "not shown to beat the null", never as a partial
    pass. What would resolve it, stated honestly: nothing within this
    project's data and seal protocol - more TEST days do not exist (every
    downloadable BX day is already allocated) and re-running with more
    seeds after the seal is broken is barred by (g). INCONCLUSIVE is a
    terminal, publishable verdict; resolution would take a follow-up on a
    deeper venue or consolidated feed under a fresh pre-registration.

  OPERATIVE AMENDED SCORING RULE, restated in full so it is readable
  without diffing (sections (a), (b), (c), (d), (g) of c06df11 are
  unchanged except where stated; this supersedes (e) and (f)):
  1. Scored facts: FLOW-SIGN MEMORY (per-symbol ACF(1) at 1ms collapse +
     log-log decay slope over lags 1-100) and TICK-TIME
     VOLATILITY-CLUSTERING PERSISTENCE (ratified 2026-07-30: per-symbol
     tick ACF(|r|) at lags 10 and 100 both scored, lag 50 reported
     unscored; fact-level verdict rests on the lag-100 count; the lag-10
     no-separation PREDICTION of the ratification Decision is on record).
  2. Measurability per (d), unchanged: flow memory needs >= 500 collapsed
     signs; persistence needs vartop10_absr_tick < 0.5 AND n_tick >=
     1000; identical rule on all three columns; a cell unmeasurable on
     either the LM or the real column is excluded, no credit or penalty;
     scored symbol sets named in the published table.
  3. Seeds: null 7, LM exactly 7. Per (b): 2 TEST days scored per day,
     never pooled; reported statistic = cross-seed median per (symbol,
     quantity); seed spread is estimator noise, used only for envelopes.
  4. Null envelope: [q10, q90] of the null's 7 per-seed values, per
     (symbol, quantity, day), quantile convention as above.
  5. LM BEATS the null on a symbol:
     - flow memory: median ACF(1) > q90[ACF(1)] AND ACF(1) >= 0.10, AND
       median slope < q10[slope] AND slope <= -0.30 (floors unchanged
       from the original).
     - persistence, lag 100: median tick ACF(|r|) lag-100 >
       q90[lag-100] AND >= 0.015 (floor from the ratification Decision:
       the real PRESENT panel minimum; the null envelope here hugs zero,
       so a floor is required for the same reason as flow memory's).
     - persistence, lag 10 (scored, reported against the 2b prediction):
       median tick ACF(|r|) lag-10 > q90[lag-10]; no absolute floor -
       the null's lag-10 envelope is well off zero (measurable-only
       range 0.014-0.096), so the envelope itself carries the load.
  6. Verdict per (fact, TEST day): PASS / FAIL / INCONCLUSIVE as defined
     in the DEFECT 2 fix. If the two TEST days disagree, both verdicts
     are published; regime disagreement is a finding, not a tiebreak.
  7. Stretch bar unchanged from the original (e): "reproduces real" =
     additionally within the real TEST value +/- 0.10 on ACF(1), +/- 0.20
     on slope. Defined for flow memory only; persistence has no stretch
     bar.
  8. The falsifiable sentence (f), amended only in its envelope wording:
     "The LM FAILS the headline comparison if, on the flow-sign memory
     fact, its per-symbol ACF(1) and log-log decay slope fall inside the
     CST null's [q10, q90] seed-quantile envelope - not on the real side
     of it - for at least half of the flow-measurable TEST symbols; that
     is, if it is not reliably distinguishable from a memoryless
     generator on a majority of the symbols where the fact can be
     measured. Reproducing the pooled cross-symbol median while failing
     this per-symbol bar is also a failure, not a partial pass."
  9. Sanity checks unchanged from (c): fat-tail kurtosis level,
     aggregational-Gaussianity decay shape, Hill tail index, ACF(r)
     bounce sign - never scoring criteria; gross basic-validity failures
     noted separately; passing earns nothing.

- 2026-07-30 (U fork DECIDED, user's call) EXPAND 'U' to Delete+Add in the
  ITCH-driving adapter. The tuple-structure argument settles it: a replace
  carries TWO locations (the original order and the new price) against the
  tuple's ONE PRICE_OFF slot, so TYPE_REPLACE would force a sixth field -
  a structural tuple change - for a distinction none of the scored facts
  need. Standing caveat carried over from FORMAT_RECONCILIATION.md, logged
  here so it is not rediscovered: the model must LEARN replace-atomicity
  (a DT_ZERO delete followed by a DT_ZERO same-side add) - the adapter
  CANNOT enforce it, because both halves decode to plain D/A with no
  marker distinguishing them from standalone events; each half is
  individually valid so the book stays correct, and the exposure is
  confined to cancel/replace timing, a sanity-check quantity. Post-hoc
  measurement REQUIRED: the REPLACE-ATOMICITY RATE - how often the LM
  emits the paired pattern versus reality. This measurement is hereby
  ADDED to the sanity-check list of the pre-registration as amended (the
  2026-07-30 AMENDMENT Decision above, point 9), explicitly NOT a scoring
  criterion: it can flag a basic-validity failure, it earns the LM
  nothing. Provenance correction, so nobody re-derives it: the "~12% U"
  figure quoted in an earlier task and provisionally attributed (in the
  2026-07-30 "U fork, quantified" Decision and FORMAT_RECONCILIATION.md)
  to the post-expansion pair-half share was actually the SYNTHETIC
  generator's message mix (U 12% in the BENCH.md 2026-07-27 synthetic
  row) - a fact about our generator's hardcoded ratios, never about real
  BX. The measured real-data figure stands: U = 5.9% of book messages
  pooled over 3 TRAIN days (4.6-8.7% by day). FORMAT_RECONCILIATION.md
  updated to mark the fork RESOLVED and carry the corrected provenance.

- 2026-07-30 (context-length gate, FINDING - measurement only, nothing
  changed) The planned LM architecture (tape repo config: 4 layers, vocab
  52, n_ctx=320 = 64 events at 5 tokens/event) CANNOT express the
  pre-registered scoring fact (flow-sign ACF to lag 100, 1ms collapse).
  Measured on 3 TRAIN days x panel from existing stylized summaries
  (RESULTS.md 2026-07-30 "Context-length gate"): median 188.5 book events
  per collapsed sign, so lag 100 needs a median 94,246 tokens of context -
  a 294.5x n_ctx growth factor (min 59.9x, max 8815.6x); 0 of 30
  symbol-days fit. The median tokens to span even lag 1 is 942, so a
  320-token context typically holds ZERO complete prior signs. Per the
  run's standing order: NO fix is proposed here - no new n_ctx, no
  tokenization change, no pre-registration amendment. The architecture
  decision is the user's (see Blocked on you). Work continued to the
  phases that are needed whatever context length is chosen.

- 2026-07-30 (tape reconciliation) DETERMINATION: ~/orderflow-lm and
  github.com/daltonoscar0/tape ARE the same codebase. Evidence: local root
  commit b2395b8 and remote fd8ac22 share author, message, and author
  timestamp (2026-07-30T18:14:40Z) with blob-identical trees except a
  CLAUDE.md->NOTES.md rename (same blob) and a dropped .claude/ rules file
  - i.e. a sanitized re-push; tape then subtree-merged that tree under
  pipeline/. They have DIVERGED since: the remote is 3 commits newer
  (adds the LM/probing suite, the SPY train+probe run, README - through
  2026-07-30T20:53Z) and is AUTHORITATIVE for the training/probing half,
  which exists NOWHERE locally (~/orderflow-lm has zero .py files, no
  train_spy, analysis/ empty; the local "miniGPT" repo is an unrelated
  text-corpus demo). The local repo is authoritative for exactly one
  thing: unique commit d8b15cc "SPEC: split-guard requirement for the
  future ITCH ingest", absent from the remote (remote SPEC.md has no
  dataset::enforce text). Flagged for the user: that split-guard SPEC
  commit should be pushed/merged into tape or it will be lost; the local
  repo has no git remote configured at all.
  HARVEST DECISION (recorded here; ~/orderflow-lm is not pruned, nothing
  deleted there this session):
  - TAKE: the 5-tuple factored tokenizer (52-id vocab, level-index
    PRICE_OFF), the fit/apply bin-freezing split (fit_core.hpp confined to
    fit_main; tokenize cannot fit), the OFTK v2 binary token format + the
    orderflow-factored-v2 manifest.json shape, and the minigpt training
    loop (train_spy) - the last from the REMOTE tape repo, the only place
    it exists.
  - LEAVE: the probing suite (remote tape; interpretability, orthogonal to
    generation - a legitimate Phase 4 question if the LM ever beats the
    null).
  - SUPERSEDE: tape's C++ book reconstruction. This repo's engine replays
    23.8M and 109.7M real ITCH messages with zero rejects and exact
    conservation; tape's is the one whose LOBSTER problems produced its
    censoring finding. One book implementation, and it is this one - the
    ITCH ingest drives THIS repo's Book.

- 2026-07-30 (Phase 4, ITCH ingest) Implementation decisions, all logged
  because they define what the token stream means:
  (a) The ingest lives in THIS repo (tools/itch_tokenize + _fit over
  src/oftk.hpp, src/oftk_fit.hpp, src/itch_tokenize.hpp), per the harvest
  decision: one book implementation, this one. The OFTK v2 binary format
  and orderflow-factored-v2 manifest are reimplemented byte-for-byte from
  tape (fixed key order/strings; write->load->write byte-identity pinned in
  tests) so tape's training half loads them unchanged. Fit/apply split
  kept: oftk_fit.hpp is included only by the fit tool and tests.
  (b) 'U' -> Delete+Add (the decided fork): delete half carries the orig
  order's remaining shares at its price and the real dt; add half carries
  the new price/size, same side, dt=0 (same timestamp). Verified
  state-identical to atomic Book::replace (remove+add produces the same
  ledger and the same seq numbering; layer-1 fingerprints agree on real
  data).
  (c) PRICE_OFF = tape's level_index rule against the book state BEFORE
  the event applies; the U add-half indexes against the post-delete book
  (its true pre-state). E/C/X/D take side+price from the standing order
  via find() pre-apply. F -> Add (MPID dropped), C -> ExecVisible (exec
  price detail dropped by the level-index scheme). P/Q/H remain skips.
  (d) Tokens cover 09:30-16:00 only (the auction-exclusion time filter,
  same as stylized); ALL messages are applied whatever their timestamp.
  dt is measured between consecutive emitted events; first emitted event
  gets dt=0 (tape's convention for row 0).
  (e) dataset::enforce() runs before any open in both tools; a TEST day is
  unreachable from this code path.
  (f) THE ROUND-TRIP TEST is two-layered because the 52-id vocab is lossy
  BY DESIGN (8 size buckets cannot encode 350k distinct sizes; refs are
  dropped), so "detokenize and reconstruct byte-identically" is
  information-theoretically impossible through the quantized tokens alone.
  The honest strong version, implemented: LAYER 1 (lossless) - the exact
  expanded event stream (what the tokens quantize, with exact
  price/size/ref) drives a fresh Book; its running per-event fingerprint
  (best bid/ask, ledger, open orders, top-12 levels/side, folded after
  every raw-message-equivalent) must equal the raw-ITCH-driven Book's,
  with PRICE_OFF independently recomputed from the fresh book at each
  event. LAYER 2 (quantized) - tokens must be the exact quantization of
  that stream and invert per tape's roundtrip contract (re-encode
  identity, decode agreement on lossless fields, roundtrip_ok). End-state
  compare alone would be vacuous (books drain to 0 at close), hence the
  running fingerprint.
  (g) Mutation-verified on BOTH synthetic and the real TRAIN day: adjacent
  -event swap -> layer-1 fingerprint mismatch (caught); PRICE_OFF
  off-by-one on the event side -> lvl_off recompute catches it at the
  exact event; off-by-one on the token side -> layer-2 token/stream
  mismatch at the exact event. The test can fail, and names where.
  RESULTS: full-day SPY on TRAIN 20190130: 74.18M msgs applied (zero
  rejects), 401,881 SPY msgs -> 425,841 events (23,960 U expanded, 5.96% -
  matches the measured 4.6-8.7% U range) -> 376,425 in-window -> 1,882,129
  tokens; round-trip layers 1+2 PASS; 3.88M msgs/sec (BENCH.md row).
  First real BX numbers, single day, SPY only (Phase 5 does the panel):
  PRICE_OFF inside(-1) 20.2%, tail(>+10) 2.0%, deeper than +30 0.007%,
  px UNK 0%; dt_zero 5.8%; SPY BX size edges [100,101,102,103,104,500,
  501] vs LOBSTER-SPY [100,200,201,387,500,501,1000] - the -1-only and
  window assumptions look survivable but the 20% inside-spread share is
  NEW vs LOBSTER (BX's wide spreads leave room inside); formal re-measure
  in Phase 5.

- 2026-07-31 ARCHITECTURE DECISION (user's, recorded verbatim in four
  parts before any further work; resolves Blocked-on-you item 1).
  (0a) OPTION (a) - grow n_ctx to span lag 100 - IS DEAD, and the binding
  constraint is DATA AVAILABILITY, not compute. At 94,246 tokens, one
  context window is ~5% of SPY's entire best day (1.88M tokens); across
  13 symbols x 6 TRAIN days that is order-1,500 non-overlapping windows -
  not a training set. A model whose context spans lag-100 flow memory
  would overfit long before learning flow structure. Nine days of one
  thin venue cannot support it. Recorded as a DATA-AVAILABILITY finding
  so it is not revisited later as though it were a budget question;
  compute makes it worse but is not what kills it.
  (0b) OPTION (c) IS CHOSEN: train at n_ctx=320. The reframing that makes
  this a real experiment rather than a concession: PRICE_OFF is a level
  index resolved against the LIVE BOOK, and the book persists far beyond
  64 events. So there is an information channel across the context gap
  that direct token memory does not have - if a large participant is
  working an order, the book's shape reflects it, and the model
  conditions on that shape through every price token it emits. The
  question this experiment now asks: CAN BOOK-STATE FEEDBACK CARRY
  ORDER-FLOW MEMORY WHEN DIRECT TOKEN MEMORY CANNOT? Recorded as a
  HYPOTHESIS TO BE TESTED, not an assumption that it works. Explicitly:
  this changes NOTHING about the pre-registered scoring facts,
  thresholds, or failure condition. Same bar, same protocol; the
  pre-registration (c06df11 + amendment) stands unamended.
  (0c) PRE-REGISTERED PREDICTION, written before any model exists so the
  outcome-dependence is on record the way the lag-10 prediction was:
  "Given the context gate, we expect flow-sign memory at short lags to
  be reachable via book-state conditioning and long lags (approaching
  100) to be weak or absent. If long-lag memory appears anyway, that is
  a finding about closed-loop conditioning beyond what the context
  arithmetic predicts and must be reported as such. If it does not
  appear, the pre-registered failure condition applies as written - a
  failing LM is a publishable result and the context gate is its
  explanation, not its excuse."
  (0d) THE SIZE-BUCKET CONFOUND, recorded now so it is reportable later
  and NOT usable as a post-hoc discount. BX SPY size edges came out
  [7,30,100,101,102,200,500]: eight buckets, three effectively identical
  (the enforce_edge_invariants +1 bumps on round-lot mass). The CST null
  draws sizes from the FULL empirical distribution; the LM can emit only
  eight representative values. Since the null column showed fat tails
  come from book mechanics plus empirical sizes, an LM that
  underperforms on the DISTRIBUTIONAL SANITY CHECKS may be losing to its
  tokenizer rather than its modeling. Explicit caveat, in the user's
  words: this is logged so the asymmetry can be REPORTED, not so a loss
  can be DISCOUNTED. It does not touch the two scored facts (flow
  memory, vol-clustering persistence), which do not depend on size
  resolution. If the LM loses on a SCORED fact, the size confound is not
  an explanation. Quantification (occupancy per bucket and the variance
  the 8-bucket quantization preserves) is in RESULTS.md 2026-07-31
  "Size-bucket confound quantified".

- 2026-07-31 (Step 1, COLD-START CONTROL - a FINDING that invalidates
  prior viability numbers) Every viability result recorded before today
  measured the HARNESS, not the model. The control: run the REAL BX token
  stream - by construction what a perfect model emits - through sim_health
  under the IDENTICAL two-order seeding all prior runs used. It is NOT
  VIABLE: book dead at tuple 3 (SPY 20190130), tuple 39 (SPY 20191230),
  zero collapsed signs, 98%+ UnknownReference (RESULTS.md Step 1 row).
  Mechanism: shim::resolve maps PRICE_OFF 0..+10 to "the price of that
  occupied same-side level, must exist"; two resting orders leave ONE
  occupied level per side, so only indices 0 and -1 can resolve, and Phase
  5 measured 20.9% of panel events pricing inside the spread - so roughly
  80% of a correctly-learned add distribution targets levels that are not
  there at t=0. Adds fail, deletes succeed, the book drains, and an EMPTY
  book is an ABSORBING state for this shim (every rule needs an existing
  reference). CST never had this problem: it places at absolute tick
  distances from the touch so it can build depth from nothing, and it got
  an 08:00-09:30 warm-up. CONSEQUENCE FOR THE RECORD: the 2026-07-31 Step
  3c row ("4/4 sampled streams NOT VIABLE" at the 2,000-step checkpoint)
  and the Phase 6 pilot's 96.2% UnknownReference are NOT evidence about
  those models. They are not retracted - they are re-labelled by this
  Decision as measurements of the cold start. Diagnostic added so this
  class of error is visible next time: shim::Why + sim_health --why name
  the RULE that refused, not just the Reject category.

- 2026-07-31 (Step 2, HARNESS CHANGE - explicitly not a model or scoring
  change) --warm-start: generation may begin from a REAL resting book.
  tools/warm_book replays a TRAIN day's pre-open flow through the SAME
  ingest (ingest::ingest_day + strict ingest::replay_events, so no second
  reconstruction exists) up to 09:30 and writes the resting book as a text
  snapshot; sim_health and shim_drive load it instead of seeding two
  orders. Framing, stated so it cannot be mistaken later: this makes the
  LM's starting conditions COMPARABLE to the CST null's 08:00-09:30
  warm-up, it does not advantage the LM - the null already had a warm
  book by construction and the LM did not. No scored fact, threshold,
  measurability rule, or failure condition is touched.
  Implementation calls, logged for veto: (a) the replay lives in a
  SEPARATE tool rather than inside sim_health, because the VAL sweep runs
  sim_health dozens of times and re-replaying a multi-GB day per run is
  untenable (the TRAIN days are gzipped and the machine has ~1GB of
  headroom); the snapshot is a small inspectable artifact and the tools
  just load it. (b) The snapshot stores every resting order in book order
  (levels best-first, FIFO within level), so reload reproduces level
  structure AND queue order exactly; only refs differ, and refs are
  dropped by the tokenizer and unobservable through the shim.
  (c) warm_book accepts NO override flag at all - a warm start has no
  business reading a sealed day, so the guard cannot be talked out of it.
  (d) The model is NOT primed with real tokens; only the book is warmed.
  tests/test_warm_book.cpp pins the round-trip, shim-resolution
  equivalence, refusal of malformed/crossed snapshots, and the cold-start
  failure itself, so a future "simplification" back to two-order seeding
  fails a test.

- 2026-07-31 (Step 2, THE DEEPER DEFECT - measured, NOT fixed) Warm-started,
  the real stream is STILL not viable (dead at tuple 8,265 vs 39 cold; 37
  signs vs 0). Per the pre-registered branch of the instruction, that means
  the shim itself is wrong, and it is: THE PRICE_OFF INVERSE IS NOT A
  STRUCTURAL INVERSE. ingest::detail::level_index records the COUNT of
  strictly-better occupied levels, so "join occupied level k" and "open a
  NEW level just better than occupied level k" encode to the SAME token,
  and shim::resolve always picks "join level k". Measured on real
  in-window adds (itch_tokenize now reports the classification): SPY
  56.46% at-level, 9.40% NEW-interior (inverse wrong), 34.14% inside the
  spread and collapsed to exactly one tick, 0.00% below the book bottom.
  Consequence is a DEPTH RATCHET: level destruction is unrestricted, level
  creation happens only at the touch (-1) or via PX_TAIL which itself
  REQUIRES >= 11 occupied levels, so a side that falls below 11 levels can
  never rebuild depth. Observed: ask side 11 levels -> 4 within 400 tuples
  while the order count still rises, then indices 8/9/10 and PX_TAIL - the
  adds that would rebuild depth - reject en masse (12.4% of tuples).
  Boring explanations checked and eliminated by measurement rather than
  argument: partial-cancel-as-full-delete (0.02% of events),
  add/delete imbalance (net +6,795 toward growth), adds below the bottom
  (0.00%), invariant/absurd rejects (0).
  WHY IT IS A CEILING: out/tokens/sample.py generates from the model's own
  token history alone and passes NO book state, so a trained model, like
  the real stream, requests level indices blind to the depth that exists.
  The real stream is a fair UPPER BOUND on what any model on this
  tokenization can reach through this shim, and it does not clear the bar.
  NOT FIXED THIS SESSION, deliberately: every candidate repair changes what
  the model emits or what the vocabulary means (see Blocked on you), and
  that is a scope decision, not an implementation detail.

- 2026-07-31 (Step 4, THE TRAINING KILLS ARE A RESOURCE PROBLEM, not a stop
  signal) The third 32,000-step attempt died at step 7,600 with no
  traceback and no nonzero exit in the log - a SIGKILL signature. It had
  been launched double-forked into its own session with caffeinate, so a
  harness teardown or idle sleep is ruled out. What the machine looked like
  at the kill: 16 GiB RAM, 3.55 of 5 GiB swap in use, and 379 MiB of FREE
  DISK - and the swap file lives on that volume, so macOS could not grow
  swap. Concurrently running: a warm_book / itch_tokenize ingest, each of
  which slurps an 863 MB day plus a full BookSet (multi-GB RSS). The boring
  explanation fits every observation: memory pressure with no room to page
  out, and jetsam takes the largest resident process, which is the trainer
  holding the 88.4M-token corpus. It also retro-fits the earlier two stops -
  both landed during heavy corpus-build ingests on a near-full disk. Acted
  on rather than argued: the ingest pass was cut short at 7 of 13 symbols
  and the decompressed day deleted the moment free disk hit 290 MiB.
  Implication for the re-run: the trainer needs the machine to ITSELF, and
  it needs disk headroom - not a different venue. Per the instruction, the
  run was NOT restarted a fourth time.

- 2026-07-31 (Step 7, SHIM REPAIR - Blocked-on-you item 5 decided, option
  (b), Claude's call under an explicit "finish it") THE
  NEAREST-ACHIEVABLE-INDEX RULE. An ADD whose PRICE_OFF names an occupied
  level the book does not have now OPENS a level one tick beyond the
  deepest one (index L, the nearest the book can offer) instead of
  rejecting; PX_TAIL anchors at level min(10, L-1), so behaviour is
  UNCHANGED whenever >= 11 levels exist; a PX_TAIL CANCEL takes the
  deepest occupied level. UNK cancels, -1 cancels and ordinary
  absent-level cancels still reject. Shim-only: no vocab change, no
  manifest bump, no bin refit, no corpus rebuild, no retrain.
  WHY (b) AND NOT (c): (c) is the correct fix for the residual interior
  ambiguity but costs a corpus rebuild, and rebuilding requires
  decompressing days on a volume with ~1 GiB free - the exact condition
  that has SIGKILLed the trainer three times. (b) removes the mechanism
  that was actually killing streams (the depth ratchet) at zero data cost.
  VALIDATED by the control that condemned the old harness: the real token
  stream, warm-started, is VIABLE on ALL SIX SPY TRAIN days (99.65-99.98%
  applied, 2,034-5,623 signs, V1+V2+V3 pass). It was dead at tuple 3 cold
  and tuple 8,265 warm before this.
  A WRONG TURN ON THE RECORD, because the measurement is the point: the
  first cancel rule was FULLY symmetric with the add rule, on the
  plausible argument that being generous to adds and strict to cancels
  must inflate depth - and it does (+4,304 orders/day, drift +4.84 per
  1k). Measured, full symmetry COLLAPSES the book (applied 99.27% ->
  11.74%, dead at tuple 108,540), because index-8..10 cancels then hammer
  the bottom levels. Scoping relocation to PX_TAIL alone keeps viability
  AND fixes the inflation (99.90% applied, book end 506 orders, drift
  +0.62). The boring explanation beat the elegant one, again.
  TWO TESTS WERE REWRITTEN, logged here as deliberate behaviour changes
  rather than edits to green (CLAUDE.md requires this): the old cases
  pinned "deep-index add rejects" and "any absent-level cancel relocates",
  which are precisely the behaviours this change alters. They now pin the
  new invariants, including an ANTI-RATCHET property test (deep-index adds
  must REBUILD depth) that the old shim would fail. The ctest gate caught
  the second rewrite before it shipped, which is what the gate is for.
  COSTS, stated: PRICE_OFF decoding changed, so every viability number
  from before this Decision is on the OLD decoder and is not comparable to
  numbers after it - the Step 1/2/5 rows stand as measurements of the
  harness as it then was, and are not retracted. The interior ambiguity
  (9.40% of real SPY adds open a level between two occupied ones) is NOT
  fixed and cannot be without a vocabulary change; item 5 therefore
  survives in reduced form as item 2's companion. Nothing in the
  pre-registration is touched - no scored fact, threshold, measurability
  rule, seed count or failure condition.

- 2026-07-31 (trainer resume) train_corpus.py --resume: the checkpoint now
  carries optimizer, scheduler and sampler-RNG state, so a kill costs one
  checkpoint interval instead of the whole run. Added because the run has
  been SIGKILLed three times and the diagnosis (memory pressure, not a
  stop signal) says a fourth is possible whatever we do. The supervisor
  that drives it caps attempts at 6, so it can never become an infinite
  restart loop, and each attempt logs free disk so a resource kill is
  visible in the log rather than inferred.

- 2026-08-02 PRICE_OFF RESOLUTION - KEEP THE 52-ID VOCAB (blocked-on-you
  items 2 and 5, decided TOGETHER by the user as recommended, since both
  are about PRICE_OFF resolution and one rebuild would have served both).
  Option (c) - the tokenizer change - is DECLINED. No vocab change, no
  manifest bump, no bin refit, no corpus rebuild, no retrain. The 88.4M
  corpus and the in-flight checkpoint stay valid.
  WHAT THIS BUYS: the shim repair (Step 7) already removed the mechanism
  that killed books, so what item 5 still carries is DISTORTION, not a
  survival bug.
  THE "does not touch either scored fact" CLAIM WAS CHALLENGED AND THEN
  UPHELD, by measurement both times. RESULTS.md Step 8 found the pipeline
  destroys volatility clustering (a scored fact) and I flagged the claim as
  wrong. The Step 9 ablation then isolated the vocabulary's own
  contribution: `pxadd` moves 72,102 add prices to the PRICE_OFF-decoded
  level with everything else exact, and BOTH scored facts survive it - tick
  ACF(|r|) 0.462 vs a 0.503 control, flow-sign slope EXACTLY unchanged at
  -0.881. What the vocabulary costs is a ~6x inflation of fat tails, which
  the null column already demoted to a sanity check. So the claim stands as
  originally written, now on evidence rather than on my framing, and the
  destruction of clustering belongs to something else (leading hypothesis:
  the shim's ref-free cancel resolution - untested).
  WHAT IT COSTS, and both of these are now PERMANENT LIMITATIONS of the
  claim rather than open questions: (1) 9.40% of real SPY adds open a new
  interior price level, the token cannot say so, and those adds join the
  level below instead; (2) 20.9% of BX panel events price inside the
  spread and all collapse into the single -1 bucket, one tick of a 7-tick
  BX spread. Both are MEASURED, both get stated in the writeup as named
  limitations with these numbers attached, and neither is retracted or
  discounted later. The honest claim shrinks accordingly: add PLACEMENT
  inside and just-inside the spread is coarser than BX reality, and no
  result here should be read as evidence about fine-grained placement.

- 2026-08-02 THE TAPE IMPORT PATH WAS A TIME BOMB, and it went off. Both
  train_corpus.py and sample.py imported the `tape` package from a
  PER-SESSION scratchpad path under /private/tmp/claude-501/<session-uuid>/.
  That directory is wiped between sessions, so the resume died instantly
  on ModuleNotFoundError - nothing to do with memory pressure, and it
  would have broken EVERY future training and sampling run. Repointed at
  ~/tape, a durable git checkout of the same package.
  VERIFIED IDENTICAL rather than assumed, because ~/tape's files predate
  the scratchpad copy by a day and could have drifted: (1) the 11,000-step
  checkpoint's state_dict loads STRICT into a MiniGPT built from ~/tape's
  ModelConfig - same architecture, no missing or unexpected keys; (2)
  load_bins reproduces the original run's corpus counts EXACTLY - 76 train
  bins / 88,020,554 tokens and 13 val bins / 9,624,097 tokens - with max
  token id 51 < 52. One red herring checked and dismissed: ~/tape's
  tdata.vocab_size("factored") returns 19, not 52, but that constant is
  never on this path - vocab comes from out/tokens/manifest.json, which is
  the BX-refit 52-id manifest.

- 2026-08-02 SCORED-RUN BUDGET = 90 MINUTES (the open half of
  blocked-on-you item 4, decided by the user). The scored model gets the
  SAME budget as the budget run - 32,000 steps, ~1.9 epochs of the 88.4M
  corpus - not the 200k-step option. Consequence to state in the writeup:
  the headline table is produced by a deliberately small model on a fixed
  90-minute budget, so a weak LM column is NOT evidence that the
  architecture cannot do better; it is evidence about what this budget
  buys. Undertraining is a named, chosen limitation, not a discovered one.

- 2026-08-02 THE SEALED TEST RUN IS AUTHORISED by the user, on
  {20181228, 20200130} - the pair in src/dataset.hpp. The user's message
  said "20191228", which is a SATURDAY and not a trading day; no such file
  exists in data/. Read as a typo for 20181228 and corrected here rather
  than silently. ORDERING, which the authorisation does not change: TEST
  is one-shot and unsealing it cannot be undone, so it runs LAST - after
  viability passes and after the LM stylized column is settled on VAL.
  Running it earlier would spend the seal on a model we cannot yet score.
  THE ONE CONDITION UNDER WHICH I STOP AND ASK ANYWAY: if the model's
  stream is NOT VIABLE at 32k steps, there is no LM column to compare and
  the TEST days would be unsealed for nothing - that is a different
  situation from the one authorised, so it goes back to the user.

- 2026-08-03 KV CACHE for sampling (pylm/kvcache.py), and the SEMANTIC
  CHANGE it forces - logged because it changes what the model conditions
  on, which is not an implementation detail.
  WHY: sampling, not training, is the remaining wall-clock cost. Measured
  on this machine (M4/MPS, budget32k_v2's 227k-param config, batch 8):
  full forward at n_ctx=320 = 16.65 ms, full step 18.96 ms, 52.3
  tok/s/stream. 319 of every 320 positions were recomputed and discarded.
  THE PROBLEM, which has no clean fix: MiniGPT uses LEARNED ABSOLUTE
  position embeddings and the old sampler slid its window by one token per
  step, so every token's position - and therefore every cached key and
  value - changed every step. An EXACTLY equivalent KV cache for this
  model DOES NOT EXIST. That is a property of absolute-PE + sliding
  window, not a shortcut anyone took.
  WHAT WAS BUILT INSTEAD - CHUNKED REFRESH: hold the window fixed while
  the cache fills, then advance it by refresh_stride and rebuild with one
  prefill. Consequence, stated plainly: the model now sees between
  (n_ctx - stride) and n_ctx tokens of history where it previously always
  saw n_ctx. At the default stride 64 that is 256..320, mean 288.
  WHY THAT IS ACCEPTABLE HERE, argued rather than asserted: the context
  gate (2026-07-30 Phase 1 finding) already established that n_ctx=320
  cannot express the scored fact at all - the median requirement is 294.5x
  the context, and lag 1 alone typically exceeds it. A 10% mean reduction
  in a window already two orders of magnitude too short cannot change a
  scored-fact verdict. If the architecture ever grows a context that CAN
  express the fact, this trade must be revisited, not inherited.
  THE VERIFICATION STANDARD WAS WEAKENED ON PURPOSE, and the weakening is
  the point to notice. The 2026-07-31 preallocation fix was checked
  BYTE-IDENTICAL against the old loop. This one cannot be: chunked refresh
  changes the conditioning context, so streams MUST diverge. Claiming
  byte-identity here would have been false. What replaced it:
  (1) EXACT where exactness is available - pylm/test_kvcache.py pins
  prefill == MiniGPT.forward (max |diff| 4.8e-7), teacher-forced cached
  step == sliding-window forward over all pre-slide steps (4.8e-7),
  prefill fully resets cache state, and post-refresh logits == an uncached
  forward over the window the cache actually holds. Cache arithmetic is
  therefore proven correct independently of what the sampler does with it.
  (2) The PREDICTED divergence point, which is the sharpest evidence the
  implementation is right: the window first moves at token 320, and on the
  real checkpoint all 8/8 streams are IDENTICAL to the uncached stream for
  the first 319 tokens and first diverge at index 321. A wrong cache
  diverges immediately; this one diverges exactly where theory says.
  (3) DISTRIBUTIONAL, since byte-identity is unavailable: token-histogram
  total-variation distance cached-vs-uncached is 0.047-0.059, BELOW the
  0.061 split-half noise floor of the uncached run against itself and
  below the 0.070 seed-to-seed distance. The cache is not distinguishable
  from a reseed at this sample size.
  (4) DOMAIN-LEVEL: sim_health warm-started on 4 streams per mode, 60k
  tokens each - identical verdict on all 8 (NOT VIABLE, V1 FAIL, V3 pass).
  Honest limit of this arm, stated rather than buried: the within-mode
  seed spread (applied 48-77% uncached, 51-99% cached) is WIDER than the
  between-mode difference, so with 4 streams it confirms no gross
  behavioural change and cannot certify more than that. The histogram test
  is the stronger evidence.
  MEASURED RESULT: 4.68x at batch 8 (52.3 -> 244.9 tok/s/stream), 2.95x at
  batch 4. Stride 128 is not faster than 64 (the incremental step, not the
  refresh, is the floor), so 64 is the default: same speed, more history.
  --no-kv-cache retains the original loop, which is how any pre-2026-08-03
  RESULTS row gets reproduced.
  WHERE THE CODE LIVES, and why it moved: pylm/ in THIS repo, not
  out/tokens/ alongside sample.py. out/ is gitignored, and the 2026-08-02
  scratchpad-path bomb survived four kills precisely because the
  generation scripts were untracked. New load-bearing logic does not go
  back into an untracked directory. sample.py still lives in out/ and is
  still untracked - moving the existing files is a separate call.
  NOT DONE, flagged not fixed: sample.py still writes only at the END of a
  run, so a kill still costs everything. That is now a ~1 h exposure
  rather than ~4.2 h, but it is the same defect that lost the 100k-token
  run on 2026-08-02.

- 2026-08-03 ALL 7 SEEDS IN ONE BATCH (--seeds), authorised by the user
  ("drop it to 1 hr"). At n_ctx=1 this model is dispatch-bound, so batch is
  nearly free (2.96 ms at B=8, 3.73 ms at B=56) while the uncached path
  scales linearly with it. 7 seeds x 8 streams therefore cost about what
  one seed cost before.
  HOW THE PRE-REGISTRATION IS KEPT LITERALLY TRUE, since "LM seeds =
  EXACTLY 7" is a fixed term of the amended pre-registration and batching
  could have quietly reinterpreted it as "56 streams from one seed": each
  seed gets its OWN torch.Generator, and its uniforms are drawn only from
  that generator, in an order independent of how many seeds share the
  batch. So the 7 groups are 7 genuinely distinct RNG streams that happen
  to share a forward pass, not one stream partitioned after the fact.
  Verified rather than asserted: seed 3 run ALONE at batch 8 reproduces
  seed 3's block inside the 56-row batch byte-for-byte on 7 of 8 streams
  over 20,000 tokens; the 8th is identical for 17,537 tokens and then
  diverges (fp tie-flip - batched matmul tiling differs at B=56 vs B=8, so
  one near-boundary categorical draw resolves the other way and the
  streams decorrelate after it). Exact in logic, subject to GPU fp
  non-determinism across batch shapes. Said plainly so nobody later reads
  "reproducible" as stronger than it is.
  SAMPLER RULE CHANGED, and why it had to: torch.multinomial takes ONE
  generator per call, so 7 distinct seeds would need 7 calls per step
  (~3 ms each at batch 8 = ~21 ms, worse than the problem being solved).
  Replaced with inverse-CDF sampling from bulk-drawn per-seed uniforms,
  which is both seed-separable and cheaper than multinomial (0.36 ms vs
  3.00 ms at batch 8). The sampling RULE is not pre-registered - only
  temperature/top-k selection and the seed count are - so this is an
  implementation call, logged because it changes streams. Correctness
  pinned by a chi-square-style gate (empirical vs target max error 0.0030
  over 20k draws) and a clamp gate (fp cumsum can land under 1.0, so a
  uniform above it would index past the vocabulary).
  A REAL BUG FOUND ON THE WAY, and it would have killed the TEST run: the
  first 56-row attempt died with EXIT 137 (SIGKILL). RSS was NOT the
  cause - it plateaued at ~230 MB. Every prefill creates ~112 MB of
  transient tensors at batch 56 and the MPS caching allocator never reused
  them: driver-allocated memory grew LINEARLY at ~1.8 MB/token (1.68 GB at
  500 tokens -> 13.78 GB at 7,500) while live-tensor memory stayed flat at
  41.8 MB. On Apple Silicon that is unified memory, so it is invisible in
  RSS and jetsam takes the process. Projected to ~900 GB over a 500k-token
  run - the 1-hour plan would have died every time. Fixed with
  torch.mps.empty_cache() every 8 refreshes: bounded at ~1.7 GB, no
  measurable throughput cost, and the 20k-row run that died now completes.
  NOTE FOR THE RECORD, because PLAN has blamed the machine four times: the
  machine WAS also in the documented bad state during this session (data
  volume 98% full / 5.2 GiB free, swap 4.16 of 5.12 GB used, and the
  user's python3 -m edge.harness.run at 112% CPU). That state is real and
  still costs throughput - but it was NOT the cause of this kill, and
  attributing this one to it would have hidden an unbounded allocator
  leak. Boring explanation first, as the working rules say; this time the
  boring explanation was our own bug.
  MEASURED: 20,000 tokens x 56 streams in 1.5 min = 228 tok/s/stream,
  12,794 tok/s total, WHILE contending with the edge job. Projected 500k x
  56 = ~37 min, against ~18.6 h for 7 sequential uncached batch-8 runs
  (~30x). The user's "1 hr" target is met with margin.

- 2026-08-03 THE ENVIRONMENT MOVED UNDER US, twice, and both moves matter
  more than they look.
  (1) ANACONDA WAS DELETED while freeing disk. /opt/anaconda3 is gone - it
  was ~37 GiB and it was the interpreter every script here named by
  ABSOLUTE PATH. Repointed to /usr/bin/python3 (3.9.6, torch 2.4.1, numpy
  1.26.4); out/tokens/sweep.sh was the only other file carrying the dead
  path. All 10 pylm gates re-verified green on the older torch, checkpoint
  loads, tape imports. Same class of failure as the 2026-08-02 scratchpad
  path: a hardcoded absolute interpreter path outside version control.
  (2) CPU IS 10.8x FASTER THAN MPS for this model - 703.7 vs 65.4
  tok/s/stream at batch 8. The model is 227k parameters, far too small to
  amortise GPU dispatch, and torch 2.4.1's MPS path is worse than 2.9.1's
  (~200 us/op vs ~58). Every measurement in this project, including the KV
  cache design above, was taken on MPS because that is what training used
  and what the sampler defaulted to. THE DEVICE CHOICE WAS WORTH MORE THAN
  THE CACHE (10.8x vs 4.7x) and it was never questioned. The cache still
  earns its place - it is a device-independent algorithmic win, and both
  compose - but the lesson is that the default was never measured.

- 2026-08-03 VIABILITY AT DAY SCALE: NOT VIABLE, and now CHARACTERISED
  rather than merely observed. First verdict at real-day stream length
  (~1.07M tokens = ~213k tuples) on the repaired decoder; V1 has never
  before been testable at any length ever sampled.
  THE FINDING - MONOTONIC DECAY WITH STREAM LENGTH. One stream (s3, the
  BEST of 8), everything else held fixed, truncated at increasing lengths:
    tokens   tuples   applied  two-sided  signs
    50,000   10,014    98.50%    100.0%      60
   100,000   20,022    83.11%     69.0%      98
   200,000   40,050    65.88%     34.5%     139
   300,002   60,068    60.11%     23.0%     189
  At 50k tokens this stream PASSES V2. By 300k it is at 23%. Sign
  production decelerates as the book thins, so V1 does not arrive either.
  THE CONTROL THAT MAKES IT TRUSTWORTHY, run because this project has been
  burned by exactly this before (the cold-start control proved an earlier
  headline was the harness, not the model): the REAL BX token stream, same
  warm start, same harness, same truncation -
    tokens   tuples   applied  two-sided  signs
    50,000   10,000    98.61%    100.0%      73
   200,000   40,000    99.12%    100.0%     258
   700,000  140,000    99.66%    100.0%     795
 1,068,774  213,754    99.77%    100.0%   1,844
  The real stream IMPROVES with length and never leaves 100% two-sided. So
  the decay is the MODEL's, the harness is exonerated, and - a second
  result that comes free - the repaired shim is validated at day scale for
  the first time: the real stream would pass all three criteria (1,844
  signs, 100% two-sided, alive). The bar is reachable; this model does not
  reach it.
  WHAT THIS RETROSPECTIVELY INVALIDATES: every viability verdict in this
  project's history was taken at short stream length, and short lengths
  systematically FLATTER the model - at 50k tokens the model produces 60
  signs against the real stream's 73, nearly indistinguishable. The
  "5 of 8 keep the book alive" row (2026-07-31) and this session's own
  60k-token comparison are both length artifacts. Viability must be
  measured at day scale or not claimed.

- 2026-08-03 THE ORDER-IDENTITY ABLATION IS INCONCLUSIVE, and is recorded
  as inconclusive rather than written up as the confirmation it superficially
  resembles. tools/ablate gained --mode noref (order identity dropped,
  everything else exact; deliberately NOT folded into --mode all, which
  prior RESULTS rows used). The control still reproduces the real column
  exactly (kurt 106.96, tick ACF 0.503, slope -0.881, 1,866 signs), so the
  harness is sound.
  BOTH ATTEMPTS ARE DEGENERATE BY THIS PROJECT'S OWN RULE. Exact-price
  resolution: n_tick 41,517 -> 2,031, vartop10 0.9966. Nearest-occupied-
  level fallback (closer to what the shim does): n_tick 10,262, vartop10
  0.8462. The measurability rule (2026-07-30 per-symbol restructure) calls
  tick ACF UNMEASURED at vartop10 >= 0.5, so neither run can carry the
  claim - the apparent collapse of clustering to ~0 is indistinguishable
  from estimator degeneracy, which is precisely what that rule exists to
  catch.
  WHY IT LOOKS INTRINSIC, not tunable: dropping identity makes the
  reconstructed book diverge from the real one, which makes later events
  unresolvable, which guts the mid series the statistic needs. Skip
  provenance at 38%: no-order 52,176, engine-reject 51,853, zero-qty 0 -
  so half is events finding no order on the side and half is the engine
  refusing an operation the diverged book cannot accept. You cannot drop
  identity and keep the event stream intact, which means a single-variable
  ablation may be the wrong instrument for this question.
  CONSEQUENCE: the SECOND SCORED FACT REMAINS OPEN, and it blocks the seal
  independently of the model. Settling it needs a different design, or a
  dated Decision demoting persistence to a pipeline ceiling. Not tonight.

- 2026-08-03 TEST WAS NOT RUN, and the authorisation was not exercised.
  The user authorised the sealed run (2026-08-02) and tonight asked to go
  all the way. It still did not run, because the PRIOR gate in that same
  Decision fired: "if the model's stream is NOT VIABLE at 32k steps, there
  is no LM column to compare and the TEST days would be unsealed for
  nothing." Two independent blockers, either sufficient: the model is not
  viable, and the second scored fact is unsettled. Spending a one-shot
  irreversible read to produce a column with no model behind it is the one
  thing the seal protocol exists to prevent. TEST remains sealed.

- 2026-08-03 SCORED FACT 2 IS DEMOTED to a documented limitation, and the
  project finishes on the NEGATIVE RESULT (option (a), chosen by the user).
  This Decision names the 2026-07-30 RATIFICATION Decision that made
  tick-time volatility-clustering persistence the second scored fact, and
  states what changed and why, per that Decision's own protocol.
  WHAT CHANGED, and note the direction: (i) the generation pipeline
  DESTROYS the statistic (tick ACF 0.503 -> -0.000, Step 8), and (ii) the
  ablation built specifically to find out why is INCONCLUSIVE - both
  variants are degenerate by this project's OWN measurability rule
  (vartop10 0.9966 and 0.8462 against a >= 0.5 UNMEASURED threshold), and
  the degeneracy looks intrinsic to the design rather than tunable. So the
  fact COULD NOT HAVE BEEN SCORED even if a viable model had existed.
  WHY THIS IS NOT TARGET-MOVING, which is the thing to check whenever a
  scored fact is dropped after results exist: demotion makes the claim
  NARROWER, not wider. It rescues no result - no LM was scored on
  ANYTHING, because none produced a viable stream. Dropping a fact the
  model failed would be target-moving; dropping a fact the APPARATUS
  cannot measure, while the model fails a different and still-standing
  bar, is a limitation being admitted. The ratification itself was sound
  on the evidence available on 2026-07-30; what was not known then is that
  the pipeline cannot carry the statistic.
  FLOW-SIGN MEMORY IS UNTOUCHED and remains the scoring fact of record. It
  was never exercised against an LM, because there was no viable LM.
  CONSEQUENCE: the seal is not needed and TEST is not spent. docs/CLAIM.md
  updated to the negative result.

- 2026-08-03 (SUPERSEDED SAME DAY - see the FRAMING WITHDRAWN Decision
  below; kept because the reasoning it contains is still the standard any
  future reframe has to meet)
  REFRAMED AS A BENCHMARK (docs/BENCHMARK.md, README.repo.md),
  decided by the user AFTER the negative result. Logged with that ordering
  stated explicitly, because reframing a failed modelling project as a
  benchmark is exactly how a negative result gets laundered into a
  deliverable, and the only defence is that the record says when the
  reframe happened and what it did or did not touch.
  WHAT THE REFRAME DOES NOT TOUCH - the list is the point: no threshold,
  bar, scored fact, measurability rule, seed count, envelope, verdict
  partition, or measured number changed. The failing entry
  (budget32k_v2, NOT VIABLE 8/8) is IN the standings table, named, with
  its decay curve. The sealed TEST days stay sealed. The
  pre-registration is cited, not rewritten.
  WHY IT IS DEFENSIBLE ANYWAY: the artifact genuinely is a benchmark and
  always was one - a correct engine validated on real data, a
  TRAIN-calibrated memoryless floor, a pre-registered scoring rule
  committed before any model existed, a viability bar that real data
  demonstrably passes, and two never-read held-out days. That apparatus
  is what usually takes months and is easy to get subtly wrong; it does
  not depend on the model that failed. The reframe changes which part of
  the work is the headline, not what any part of it says.
  WHAT IT ADDS THAT MATTERS TO A FUTURE ENTRANT, beyond re-labelling:
  the day-scale requirement is now stated as a RULE rather than a
  finding - viability measured below ~1.07M tokens is not a measurement,
  because short streams flatter models badly (the failing entry is
  indistinguishable from real data at 50k tokens and at 6.4% two-sided by
  day scale). Every viability number this project recorded before today
  violated that rule.

- 2026-08-03 BENCHMARK FRAMING WITHDRAWN, same day, at the user's
  direction: the docs now present the project as what it is - a failed
  attempt with a well-characterised failure and a reusable apparatus -
  rather than as a benchmark with one failing entry.
  WHY THIS IS THE BETTER CALL, and it is not merely cosmetic: the
  benchmark framing was defensible (the earlier Decision's argument still
  holds - nothing was renamed, no threshold moved, the failing entry
  stayed in the table) but it put the emphasis on the reusable apparatus
  and made the failure a row in a standings table. That is a real
  distortion of what happened, even with every number intact. The project
  set out to model order flow and did not. Leading with that is the
  accurate presentation; leading with the harness is the flattering one.
  WHAT CHANGED: docs/BENCHMARK.md removed (content preserved in git and
  its practical parts folded into README + POSTMORTEM); docs/POSTMORTEM.md
  written as the centrepiece - what was tried, what failed, SEVEN process
  mistakes with their lessons, what is worth keeping, what to try next;
  README.repo.md leads with "it didn't work"; docs/CLAIM.md rewritten to
  lead with the failure rather than the apparatus.
  WHAT DID NOT CHANGE, and deliberately: RESULTS.md and BENCH.md are
  untouched - they are append-only, and rewriting the measurement logs to
  match a new narrative is precisely the thing the append-only rule exists
  to prevent. The dated Decisions above are untouched for the same reason,
  including the superseded benchmark Decision: an audit trail that gets
  tidied is not an audit trail. Two mechanism claims retracted earlier
  today remain legible in RESULTS.md as corrections naming their originals.

## Blocked on you
Updated 2026-07-31. Each item states specifically what it needs from you.
1. ARCHITECTURE vs THE CONTEXT GATE - RESOLVED 2026-07-31 by the user:
   option (a) dead on data availability, option (c) chosen (train at
   n_ctx=320), book-state-feedback hypothesis + pre-registered lag
   prediction recorded, size confound quantified. Full text: the
   2026-07-31 ARCHITECTURE DECISION entry above.
1b. THE TRAINING RUN HAS NOW BEEN KILLED THREE TIMES - and the cause is
   DIAGNOSED, so this is no longer a question about your intent. The
   third attempt (launched double-forked into its own session under
   caffeinate, so teardown and sleep are ruled out) died at step 7,600
   of 32,000 with a SIGKILL signature. At the kill: 16 GiB RAM, 3.55 of
   5 GiB swap in use, 379 MiB of FREE DISK - and the swap file lives on
   that volume, so macOS could not grow swap while an ITCH ingest was
   holding multi-GB RSS. jetsam takes the largest resident process,
   which is the trainer holding the 88.4M-token corpus. The same
   conditions fit both earlier stops (each landed during heavy
   corpus-build ingests on a near-full disk). Per your instruction it
   was NOT restarted a fourth time then. UPDATED later the same day, and
   this is the actionable part: the pressure is NOT mainly this
   project's ingests. A process listing during the fourth attempt caught
   two UNRELATED heavy jobs of yours saturating the machine -
   ~/code/ticker scripts/score_novelty.py (98.7% CPU, 798 MB) and
   python -m edge.features.build (98.4% CPU, 1.64 GB and climbing).
   Measured effect: the identical trainer ran at 6.9 steps/s at 11:00
   and 1.2 steps/s at 15:18, so the 32,000-step budget run goes from ~77
   minutes to ~7.4 hours. NEEDS FROM YOU: pause those two while a
   training or sampling run is up, and free disk (the volume is at 199
   of 228 GiB). That is worth ~6x on every remaining run here. Resume
   support now exists (train_corpus.py --resume carries optimizer,
   scheduler and RNG state), and the supervisor caps restarts at 6, so a
   kill now costs one checkpoint interval rather than the whole run.
2. INSIDE-SPREAD BUCKET GRANULARITY - RESOLVED 2026-08-02 by the user,
   jointly with item 5: KEEP THE 52-ID VOCAB. The 20.9% inside-spread
   collapse into the single -1 bucket is accepted and becomes a stated
   limitation of the claim, not an open question. Full text: the
   2026-08-02 PRICE_OFF RESOLUTION Decision above.
3. TAPE REPO DIVERGENCE - RESOLVED 2026-07-31 by the user: local HEAD
   (d8b15cc, the split-guard SPEC commit) pushed to daltonoscar0/tape as
   branch `split-guard-spec`, origin now configured in ~/orderflow-lm.
   The data-loss risk is gone. Residual (optional): the branch is not
   merged into tape's default branch, and it carries the PRE-subtree
   history (local root b2395b8), so it will not merge cleanly - the
   split-guard SPEC text is simplest to cherry-pick/apply onto tape's
   pipeline/SPEC.md when convenient.
4. REAL TRAINING RUN BUDGET - FULLY RESOLVED. The remaining half (the
   SCORED run's budget) settled 2026-08-02 by the user: 90 minutes /
   32,000 steps, same as the budget run. Full text: the 2026-08-02
   SCORED-RUN BUDGET Decision above. Original text follows.
   PARTIALLY RESOLVED 2026-07-31: you asked
   me to establish it rather than guess; established (RESULTS.md Step 3a:
   MPS ~6.4 steps/s realized, 90 min = ~32k steps ~= 1.9 epochs of the
   88.4M-token corpus). What remains is item 1b: an uninterrupted window
   to actually run it, and - for the SCORED run later - your call on
   whether 90 minutes is also the budget for the real model or whether
   it gets longer (200k steps = ~5.6h bench-rate, ~8.7h realized-rate).

5. THE SHIM'S PRICE_OFF INVERSE - FULLY RESOLVED. Part one 2026-07-31 by
   Claude under an explicit "finish it": option (b) implemented, and the
   real stream is now VIABLE on all 6 TRAIN days (see the Step 7
   Decision). Part two - the surviving INTERIOR AMBIGUITY - RESOLVED
   2026-08-02 by the user jointly with item 2: option (c) declined, the
   52-id vocab stands, and the 9.40% mis-placement becomes a stated
   limitation. Full text: the 2026-08-02 PRICE_OFF RESOLUTION Decision.
   The original finding is kept below for the record.
   The INTERIOR AMBIGUITY, as originally written. The
   token cannot distinguish "join occupied level k" from "open a new level
   just better than level k", and 9.40% of real SPY adds are the latter,
   so they still join the level below. Fixing it needs a vocabulary change
   and therefore a refit + corpus rebuild + retrain - the same bill as
   item 2, which is why the two should be decided together. It no longer
   BLOCKS anything: it distorts add placement, not book survival, and it
   does not touch either scored fact. Original text of the finding kept
   below for the record.
   Measured, not suspected (RESULTS.md Step 2 + the Decision):
   level_index records the COUNT of strictly-better occupied levels, so
   "join occupied level k" and "open a NEW level just better than level
   k" are the SAME token, and the inverse always joins. 9.40% of real
   SPY adds open a new interior level (inverse wrong); 34.14% price
   inside the spread and are collapsed to exactly one tick of a 7-tick
   BX spread. Level destruction is unrestricted while creation happens
   only at the touch, so depth ratchets down and never rebuilds. The
   real token stream - a fair upper bound, since sample.py feeds the
   model NO book state - fails viability warm-started because of this.
   I did NOT fix it: every candidate repair changes what the model emits
   or what the vocabulary means, which is your call, not an
   implementation detail. The options as I see them, with what each
   costs:
   (a) DO NOTHING and report the ceiling. Cheapest, honest, and the
       publishable claim becomes narrower still: the pipeline is sound
       and the generation harness cannot sustain a book.
   (b) SHIM-ONLY REPAIR, no vocab change, no retrain, corpus untouched:
       make PX_TAIL work below the current bottom (drop its >= 11-level
       precondition) and let deep indices that fall past the book's
       bottom open a new level one tick beyond it instead of rejecting.
       This restores a depth-creation channel. It does NOT fix the
       interior ambiguity, and it changes the DECODE of tokens already
       in RESULTS rows, so prior viability rows would need re-labelling.
   (c) TOKENIZER CHANGE - make PRICE_OFF distinguish "at level k" from
       "new level before k" (one extra bit, or a signed sub-index).
       Fixes it properly; costs a vocab change, manifest version bump,
       bin refit, corpus rebuild and retrain. This is the same family as
       item 2 below (inside-spread granularity) and they should be
       decided together, since both are about PRICE_OFF resolution and
       one rebuild could serve both.
   My recommendation if you want one: decide 2 and 5 together, and if
   you are rebuilding the corpus anyway, take (c). If you are not, take
   (b) plus an explicit re-labelling note rather than (a), because (b)
   is cheap and the ceiling it removes is large.

Resolved earlier 2026-07-30 (kept for the record):
  - LOBSTER samples: superseded. Real NASDAQ BX ITCH day landed in data/
    and became the ground truth (see Decisions). LOBSTER remains optional
    if we ever want an external orderbook diff.
  - ITCH 5.0 PDF spec diff: closed by real-data replay evidence (Decisions).
  - Note: data/20190730.BX_ITCH_50.gz.md5sum is an HTML 404 page, not a
    checksum, so the download can't be verified against NASDAQ's md5 list;
    framing was hand-verified instead and 28.7M frames parse cleanly.

## RESULTS log -> RESULTS.md, benchmarks -> BENCH.md

## Session log
- 2026-07-30 Setup: extracted phase-1 archive, git init, tagged
  phase1-baseline. Gates green on Apple M4 (0 warnings / ctest pass / 1M fuzz
  pass). Local bench baseline appended to BENCH.md (6.27M msgs/sec,
  p50 83ns, p99 1000ns, p99.9 1583ns, max 13.75ms).
- 2026-07-30 Phase A: blocked, LOBSTER samples no longer freely fetchable
  (see Blocked). Skipped, no code written.
- 2026-07-30 Phase B: built bench_tail, attributed max latency to order-pool
  rehash (RESULTS.md), fixed with Book::reserve; max 13.75ms -> 22.4us,
  p50/p99/p99.9 unchanged. Gates green (0 warnings / ctest / 1M fuzz).
- 2026-07-30 Phase C: BookSet (src/bookset.hpp) + multi-symbol generator +
  routing fuzz at 1M msgs + bench single/set/interleaved (BENCH.md). Single
  path unregressed; interleaved ~5% slower. Gates green.
- 2026-07-30 Phase A unblocked with real BX ITCH data: FrameReader now
  skips unknown types (histogram) with malformed still fatal; 'R' directory
  parser + side table; src/itch_replay.hpp harness + replay_itch tool;
  full-day replay = 23.8M book msgs, ZERO rejects, audits clean, exact
  conservation, books drain to 0 at close - Phase 1 milestone met (revised
  criterion, see Decisions). Real-data bench row appended (5.27M msgs/sec,
  p50 125ns, p99 875ns). ctest gate: tests/test_itch_replay.cpp replays a
  200k-msg slice with per-message invariant checks, skips gracefully when
  data/ absent. Gates green (0 warnings / ctest / 1M fuzz).
- 2026-07-30 Step 1 (external validation): tools/field_sanity (A/F shares
  round-lot structure + price plausibility) and tools/bbo_trace (top-5
  BBO/spread/depth every 60s -> out/*.csv). Both pass: the reconstruction
  now has external validation, not just internal consistency. RESULTS.md
  row appended. Gates green.
- 2026-07-30 Step 2 (stylized facts): tools/stylized computes the Phase 3
  "real" column from the replayed day (top 20 symbols, event + 1s/10s/60s
  sampling, kurtosis/Hill/ACFs/flow-sign ACF) -> RESULTS.md per-fact table
  + out/stylized_*.csv raw series. Methodology traps handled explicitly in
  Decisions. Gates green.
- 2026-07-30 Phase D (prose cleanup) KILLED deliberately: started in an
  earlier session and orphaned; no partial rewrite survived on disk
  (working tree clean, nothing in stash/reflog), so nothing to revert.
  Deferred until the code is structurally stable (post-match(), post-
  Phase 2 adapter). Do not resurrect it before then. CLAUDE.md created at
  repo root so the working rules are version-controlled, not session memory.
- 2026-07-30 Step 2 corrections: tick-time |r| ACF (2a: volatility
  clustering partially survives - real on ~half the panel, absent on 6/20;
  part of the 1s-grid decay was activity clustering), Hill alpha<1
  reclassified as estimator failures (2b: median 2.59 -> 2.95), windowed
  sign collapse (2c: lag-1 0.42 -> 0.27, inside Lillo-Farmer 0.2-0.3).
  RESULTS.md addendum row + section; original table untouched. Gates green.
- 2026-07-30 Step 3: match() path landed - Book::match (price-time walk,
  resting-price fills, limit remainder rests / market remainder canceled)
  + src/match.hpp emission (E per fill + A for remainder, same wire format
  the LM emits). tests/test_match.cpp: 8 property tests with an
  independent-oracle fill check, plus the strong test: 1M generated
  matches, full emitted stream (A/E/X/D/U; C never emitted by design)
  replayed through the validated reconstruction path into a fresh Book,
  end state byte-for-byte identical (fingerprint compare); zero rejects,
  mutation-verified (see Decisions). bench_match: 4.72M matches/sec,
  crossing p50 167ns / p99 792ns / p99.9 2000ns (BENCH.md row; not
  comparable to replay rows). Gates green incl. the new fuzz at 1M.
- 2026-07-30 Step 1: 8 more BX days fetched (every remaining downloadable
  day on emi.nasdaq.com), all gzip-verified; orderflow-lm-trains-on-BX
  decision + claim scope logged in Decisions. Generalization check: full
  replay of 20181228 (different year, 4.6x volume: 109.7M book msgs) =
  zero rejects, exact conservation, drains to 0 (RESULTS.md row). The
  plain sequential curl stalled at ~10KB/s; -C - resume + --speed-time
  stall-abort + 3-way parallel fetched all 8 (~6.2GB) in minutes.
- 2026-07-30 Step 0 (state check after interruption): everything the
  previous session logged actually completed - all 9 data files pass
  gzip -t, build is zero-warning, ctest + 1M fuzz green, stylized has
  2a/2b/2c, test_match.cpp exists and passes. Nothing to finish.
- 2026-07-30 Step 2 follow-up: tools/gap_hist.cpp rescued from scratchpad
  into the repo (see Decisions), builds clean, reproduces the bimodal
  same-sign inter-fill gap histogram (448k pairs, machine mode ~32us,
  valley 10-32ms, decision mass >= 100ms). Gates green.
- 2026-07-30 Step 1 follow-up: collapse_ns sensitivity sweep, 7 windows
  0 -> 100ms on 20190730 -> out/collapse_sensitivity.csv + RESULTS.md row.
  No plateau: ACF(1) median 0.414 -> 0.168 slides smoothly; verdict (per
  the pre-registered rule) = the lag-1 level is a parameter choice; slope
  ~ -0.6 is the robust quantity (see Decisions). Gates green.
- 2026-07-30 (dataset split) The 9 BX days were BOTH the intended
  orderflow-lm training set AND the source of the Phase 3 "real" column - a
  leakage hazard: train on all 9, baseline on all 9, and Phase 3 asks
  whether the model reproduces statistics of data it memorised. Split fixed
  NOW, mechanically, in src/dataset.hpp (not a convention someone
  remembers): TEST = {20181228, 20200130}, VAL = {20190730}, TRAIN =
  {20190130, 20190327, 20190530, 20190830, 20191030, 20191230}.
  Reasoning: TEST needs >= 2 days including 20181228 (Q4-2018 selloff,
  4.6x volume - a held-out set of only calm days tests nothing about
  regime transfer); 20200130 joins it as the most recent day, so TEST spans
  both a stress regime and forward-in-time transfer on a calm one.
  VAL = 20190730 because it is the methodology-development day: every tool
  was debugged on it and every published number so far comes from it, so it
  is the one day that can never honestly be TEST; making it VAL formalizes
  its role as the tuning day (sampling temperature, hyperparameters). Costs
  one TRAIN day (6 instead of 7) - accepted.
  TEST IS NOT TO BE LOOKED AT - not for baseline statistics, not for
  debugging, not for "just checking" - until the headline real-vs-sim
  comparison runs once. Enforced, not advisory: dataset::enforce() in every
  day-reading binary (tools/stylized, tools/bbo_trace, tools/field_sanity,
  tools/gap_hist, bench/replay_itch) refuses TEST days without
  --i-am-running-the-final-comparison and refuses unassigned days outright
  (the override does not bypass that); tests/test_dataset.cpp asserts the
  guard fires and pins the allocation constraints.
  Prior TEST exposure, written down rather than hidden: 20181228 was
  replayed once before the split existed (2026-07-30 generalization check -
  rejects/conservation/drain and message counts only, RESULTS.md row). That
  is engine validation, not distributional peeking; accepted. 20200130 has
  never been read beyond gzip -t. Both are sealed from here on.
  Consequence for the published stylized-facts table: 20190730 landed in
  VAL, i.e. inside TRAIN+VAL, so the existing table is NOT contaminated by
  TEST data; it is one draw, and the TRAIN+VAL multi-day baseline
  (RESULTS.md 2026-07-30) supersedes it as the Phase 3 "real" column.
- 2026-07-30 match() mutation re-check: introduced an off-by-one in FIFO
  fill ordering (match takes head->next instead of head). The FIFO
  property test fails (4 assertions) AND the strong emit->reconstruct
  fuzz test aborts - the strong test catches fill-ordering bugs
  independently of the property tests. Mutation reverted, book.cpp
  byte-identical to HEAD, gates re-run green.
- 2026-07-30 Dataset split (leakage closure): src/dataset.hpp assigns all 9
  days mechanically (TEST 20181228+20200130, VAL 20190730, TRAIN rest);
  dataset::enforce() guard in stylized/bbo_trace/field_sanity/gap_hist/
  replay_itch refuses TEST without --i-am-running-the-final-comparison and
  refuses unassigned days outright; tests/test_dataset.cpp (6 cases) pins
  the constraints and the refusal, guard also verified live (exit 3 on the
  .gz TEST day). Multi-day "real" column recomputed on the 7 TRAIN+VAL days
  (out/multiday/, RESULTS.md v2 section): the published one-day table was
  computed on VAL, so uncontaminated but superseded as one draw. Per-day
  spread is a finding: 1s kurtosis day-medians span 18.7-1655 (FOMC jump +
  venue flaps dominate the level), flow-ACF slope -0.44..-0.63 across days
  (robust to the estimator, not the draw); Hill-excl-failures and tick-time
  vol clustering are the stable targets. Gates green (0 warnings / ctest /
  1M fuzz).
- 2026-07-30 CST null column: implemented src/cst.hpp + tools/cst_calibrate
  + tools/cst_sim (design choices + pre-registered interpretation rule in
  Decisions), calibrated on the 6 TRAIN days (13-symbol panel), generated 7
  seeded days (3.65M msgs each, self-verified: 0 rejects, exact
  conservation), ran them through the unmodified stylized pipeline, and
  compared against the real column restricted to the same panel.
  DECISIVE: the memoryless null's flow-sign slope is ~0 (seed spread
  [-0.23 .. 0.00], pure estimator noise), disjoint from the real
  [-0.79 .. -0.43] - the Lillo-Farmer slope survives its kill test and is a
  legitimate scoring target with the null floor recorded. Vol clustering
  also discriminates (disjoint ranges). The null REPRODUCES fat-tail
  levels, the kurtosis-decay shape, Hill (failures included), and the
  bounce sign - all demoted to sanity checks (RESULTS.md). Aggregation was
  plain medians over the tools' summary CSVs (scratchpad script, not
  committed - no repo-worthy logic beyond what RESULTS.md states; the
  evidence CSVs live in out/cstnull/). tests/test_cst.cpp pins the
  generator mechanics. Gates green (0 warnings / ctest / 1M fuzz).
- 2026-07-30 Per-symbol restructure (v3): characterised the 2a tick-time
  clustering split across all 140 TRAIN+VAL symbol-days. Thinness
  hypothesis REJECTED (no n threshold: zeros at n=250k, detection at
  n=11.8k); the split is variance concentration - zero days have vartop10
  median 0.98 vs 0.26 for significant days, and the VAL-day six share
  vartop10 0.93-1.00 at 2-5 tick spreads while being among the most active
  symbols. Three-way classification (rule in Decisions): 87 PRESENT / 4
  ABSENT / 49 UNMEASURED; measured absence is rare and boring (OILX with
  zero BX executions, IWB, MDY once each). stylized extended with covariate
  columns, 7 days rerun (leading columns unchanged), RESULTS.md v3
  per-symbol section supersedes the pooled-median structure, per-fact
  uniform-vs-symbol-dependent classification recorded. Evidence:
  out/multiday/persym_analysis.txt. Gates green (0 warnings / ctest / 1M
  fuzz).
- 2026-07-30 CST verdict surfaced + Steps 1-3. Step 0: printed the null
  section; CST flow-sign slope -0.018 (near zero) vs real -0.592 - the
  pre-registered rule fires on the "materially flatter" branch, so the slope
  DISCRIMINATES and stays a legitimate scoring target; work continues. Step 1
  (split guard, trainer side): audited orderflow-lm; no ITCH loader exists,
  all file selection is explicit-argv/manifest LOBSTER paths, so a BX TEST
  day is unreachable - requirement recorded in PLAN Decisions + SPEC, one
  source of truth = src/dataset.hpp. Step 2 (format reconciliation):
  docs/FORMAT_RECONCILIATION.md - tokenizer SURVIVES with no vocab redesign
  (PRICE_OFF already level-indexed, refs already dropped); bounded scope =
  ITCH-driving adapter + SIZE/DT bin refit + the 'U' fork; audit only.
  Step 3 (Phase 2 adapter): src/adapter.{hpp,cpp} + bench + test_adapter;
  milestone met (50k-step loop, zero invariant violations; all four reject
  categories fire; rejection bit-identical-total). Gates green (0 warnings /
  ctest / 1M fuzz). Each step committed separately.
- 2026-07-30 Methodology hardening, 4 steps, each committed separately,
  gates green throughout. Step 1: vartop10 n-confound test - NOT
  confounded (null n_tick SMALLER than real, 0.71 pooled; rho -0.08, R^2
  0.02 across 140 symbol-days; 1c n-matching not triggered; evidence
  out/multiday/n_confound_analysis.txt, RESULTS.md row). Step 2:
  user RATIFIED tick-time vol-clustering persistence as the second scored
  fact (both lags 10+100 scored, lag-10 no-separation prediction on
  record, lag-100 floor 0.015 - see Decision). Step 3: pre-registration
  AMENDED (names c06df11): [q10,q90] quantile envelope replaces min-max,
  LM seeds fixed at exactly 7, three-way PASS/FAIL/INCONCLUSIVE partition
  closes the verdict gap; operative rule restated in full in the
  amendment Decision. Step 4: U fork RESOLVED (user): EXPAND to
  Delete+Add; replace-atomicity rate added to the sanity-check list;
  FORMAT_RECONCILIATION.md marked resolved; ~12% provenance corrected
  (source was the synthetic generator's U mix in BENCH.md 2026-07-27, not
  real data; real figure 5.9%).
- 2026-07-30 Phase 1 (context-length gate): computed events-per-sign and
  lag-100 token requirements for the 13-symbol panel on 3 TRAIN days from
  existing out/multiday summaries (no raw data touched, TEST sealed);
  RESULTS.md row + Decisions finding: n_ctx=320 cannot express the scored
  fact (median factor 294.5x; lag 1 alone typically exceeds the context).
  Architecture decision left to the user. Gates green (0 warnings / ctest /
  1M fuzz).
- 2026-07-30 Phase 2: docs/CLAIM.md written - the narrow defensible claim
  with every qualifier cited to its RESULTS.md row / PLAN Decision (venue,
  9-day split, per-symbol measurability, 2 scored facts + 4 demotions +
  null floor, done/not-done with external-vs-internal validation named).
  No LM result claimed; LM column explicitly nonexistent. Gates green.
- 2026-07-30 Phase 3: tape reconciliation - determined ~/orderflow-lm ==
  daltonoscar0/tape (sanitized re-push, subtree-merged under pipeline/,
  diverged since; remote authoritative for the LM half, local holds one
  unique SPEC split-guard commit d8b15cc the remote lacks). TAKE/LEAVE/
  SUPERSEDE harvest decision recorded in Decisions; nothing deleted in
  ~/orderflow-lm. Gates green.
- 2026-07-30 Phase 4: ITCH ingest landed - src/oftk.hpp (tape's OFTK v2 +
  manifest contract, byte-for-byte), src/oftk_fit.hpp (fit split kept),
  src/itch_tokenize.hpp (ingest core: U->Delete+Add, level-index PRICE_OFF
  off the pre-event reconstructed book, dataset::enforce in both tools),
  tools/itch_tokenize{,_fit}, tests/test_itch_tokenize.cpp (7 cases incl.
  synthetic + real-slice round-trips and 3 mutation checks). Full-day
  round-trip on TRAIN 20190130 SPY: layers 1+2 PASS (401,881 state folds
  identical; 376,425 events token-verified); mutations swap/pxoff all
  caught on real data. 3.88M msgs/sec (BENCH.md). Gates green (0 warnings /
  ctest incl. new tests / 1M fuzz).
- 2026-07-30 Phase 7 (committed BEFORE Phase 6, deliberately: the pilot's
  adapter-rejection breakdown needs the shim to exist; order swap changes
  nothing else): src/token_shim.hpp - OFTK 5-tuples -> EmittedActions above
  the adapter. Resolution rules logged in the header + Decisions: ADD
  resolves PRICE_OFF against the current book (0..+10 = that occupied
  level, -1 = one tick inside, TAIL = one tick beyond level 10, UNK = one
  tick off the opposite best), EXEC = Market from the OPPOSITE side,
  CANCEL/DELETE = adapter Cancel at the resolved level (partial/full and
  SIZE dropped - cancel sizing is sanity-check territory), EXEC_HIDDEN/
  CROSS = Unparseable, DT ignored (no clock in the loop), stream driver
  resyncs one token on misalignment. tests/test_token_shim.cpp (5 cases):
  token sequence routes to the SAME book as hand-derived direct actions
  (incl. a marketable inside-spread add through match()), PartialCancel ==
  Delete, PX_TAIL, malformed/unresolvable tuples land in the right
  category, driver skips specials + resyncs. Gates green.
- 2026-07-30 Phase 4 follow-up (adversarial review, 19-agent workflow, 14
  confirmed findings / 1 refuted - all fixed, none waived): the round-trip
  test had three REAL blind spots, each demonstrated by compiled repro
  before fixing: (1) Delete-event SIZE was validated by neither layer
  (b.remove ignores it; layer 2 was circular) - strict replay now checks
  recorded side/price/size of every E/C/X/D event against the FRESH book's
  standing order; (2) dt was never independently checked - now recomputed
  from event timestamps in strict replay, with timestamps themselves
  folded into both fingerprints so they cannot drift from raw; (3) the
  fingerprint folded only the top-12 levels - now folds EVERY level.
  Mutation suite extended to five (swap, pxoff event, pxoff token, size,
  dt); all caught on synthetic AND the real TRAIN day; hardened
  round-trip still PASSES full-day SPY 20190130. Tool hardening: -o /
  --pxhist / --manifest paths that name a dataset day are refused up
  front (they truncate their target and enforce() only guarded the day
  argument - verified refusal against the sealed TEST .gz, file intact);
  a capped run no longer swallows a desync that precedes the cap;
  manifest rewrite is write-temp+rename so a failed write cannot destroy
  other tickers' frozen bins; token/pxhist writes are flush-checked; the
  unimplemented --max-frames flag doc removed; HALT-as-DT-gap divergence
  from tape documented in the header. Gates green.
- 2026-07-30 Phase 5: SIZE/DT bins refit on BX TRAIN (13-symbol panel,
  3 pooled TRAIN days; VXX 2 days - old series matured pre-20190130) and
  PRICE_OFF window re-measured on 8.03M panel events (RESULTS.md row).
  LOBSTER bins do not transfer (BX SPY carries sub-100 odd-lot mass;
  DT p99.9 gaps are seconds, not 0.26s). Window bounds survive (99.63%
  in [-1,+10], UNK 0%); the single -1 bucket does NOT match BX reality -
  20.9% of events price inside the (wide) spread. Two findings reported,
  nothing changed silently: degenerate 1-share size buckets on a
  round-lot venue; -1 bucket granularity (vocab change = user's call).
  Frozen bins in out/tokens/manifest.json. Gates green.
- 2026-07-30 Phase 6 (pilot, PIPELINE TEST - not a model): tape's minigpt
  + train_spy consumed this repo's BX OFTK tokens UNCHANGED (SPY 20190130,
  within-day 80/20 pilot split via --train-frac; panel bins untouched).
  1000 CPU steps, held-out loss 1.196 vs train 1.157, 22.7k tokens/sec;
  15k sampled tokens -> shim -> adapter: 1.35% applied, 96.2%
  UnknownReference (short-run model drains the book then references
  nothing), 2.4% Unparseable, 0 invariant violations, audit clean.
  Expected garbage; pipeline proven end to end. tools/shim_drive is the
  measurement harness (CMake entry lands with this commit). RESULTS.md
  row labeled PIPELINE TEST; no stylized facts computed on its output.
  Gates green.
- 2026-07-30 Phase 8: cold-resume state written - Status now describes the
  built-vs-missing split for a memoryless reader, Next 3 tasks reordered
  around the architecture gate, Blocked-on-you lists 4 items each naming
  specifically what it needs (architecture vs context gate; -1 bucket
  granularity; the unpushed local tape SPEC commit; training-run budget).
  Session ran phases 1-8 with 7 phase commits + 1 review-fix commit; the
  adversarial review of the ingest (19 agents) confirmed 14 findings, all
  fixed and re-verified on real data. TEST never read. Gates green.
- 2026-07-31 Step 2 (committed before Step 1's corpus commit - the
  corpus build is still running in the background; diagnostic first per
  "build BEFORE training"): tools/sim_health + a per-tuple hook on
  shim::drive. Viability bar recorded in RESULTS.md BEFORE the tool
  touched any model output (V1 >=500 collapsed signs on the pseudo-clock
  of decoded DT reps; V2 two-sided >=90% of checkpoints; V3 book alive
  through the 500th sign; drift reported, not part of the bar).
  Known-bad check passes: the pilot stream FAILS viability (book dead
  at tuple 29, 0 signs). Also this morning: the review-added aux-path
  guard was refined (Step 0 commit) - it refused ANY 8-digit day token,
  which blocked legitimate day-stamped artifact names and failed the
  entire first corpus-build pass; the hazard is *.BX_ITCH_50 targets
  and sealed-TEST tokens, which is now exactly what it refuses (both
  refusals re-verified live, TRAIN/VAL-stamped artifacts allowed).
  Gates green.
- 2026-07-31 Step 1: TRAIN corpus built - 76 TRAIN bins + 13 VAL bins,
  88.4M TRAIN tokens / 17.7M events (RESULTS.md row with per-symbol
  table + ev/sign next to the context gate). VXX absent 20190130 +
  20190327 (old series matured), noted and continued. One decompressed
  day at a time, all deleted after; 188MB of bins in gitignored
  out/tokens/corpus/, deterministic rebuild via corpus_build.sh. First
  build attempt failed wholesale on the over-broad aux-path guard (see
  Step 2 log entry); guard refined in the Step 0 commit, rebuild clean.
  Gates green.
- 2026-07-31 Step 3: throughput table (MPS ~51k tok/s bench, ~6.4
  steps/s realized; 90 min = ~32k steps); budget run INTERRUPTED - two
  external stops (19.9k steps lost pre-checkpointing; 2.3k steps with
  the 2,000-step periodic checkpoint surviving, VAL loss 1.10). Not
  restarted against the stop signal; user asked, no answer in time.
  Viability at the 2,000-step scale: 4/4 sampled streams NOT VIABLE
  (zero signs, ask side dies immediately, book dead by tuple <=916).
  The intended 90-minute-scale question is UNANSWERED. RESULTS.md row.
  Gates green.
- 2026-07-31 Step 4: BLOCKED per its own gate ("only if Step 3c produced
  a viable stream") - 3c produced none. The sweep infrastructure exists
  (out/tokens/sweep.sh + sample.py accept temperature/top-k) and the
  leakage rule stands on record for when it runs: sampling params are
  tuned on VIABILITY ONLY, never toward a stylized fact - tuning toward
  a scored fact on VAL and then scoring on TEST is exactly the leakage
  the split exists to prevent.
- 2026-07-31 (second session) Steps 1-6. Step 1: THE COLD-START CONTROL -
  the REAL BX token stream is NOT VIABLE under the two-order seeding
  (dead at tuple 3), so every viability number recorded before today
  measured the harness; shim::Why + sim_health --why added so the RULE
  that refused is visible. Step 2: --warm-start landed (tools/warm_book,
  src/warm_book.hpp, tests/test_warm_book.cpp) - a HARNESS change that
  makes the LM's start comparable to CST's warm-up; the real stream is
  still not viable warm-started, and the reason is now named and
  measured: the PRICE_OFF inverse is not a structural inverse, so book
  depth ratchets down (Decision + RESULTS.md). Boring explanations
  (partial cancels, add/delete imbalance, adds below the bottom) were
  each checked and eliminated by measurement. Step 3: dead-bucket audit -
  IWO/SOXL/IWN carry literally dead SIZE buckets, 12 of 13 symbols carry
  near-dead ones, the DT quantizer is healthy; reported, not fixed. Step
  4: the 32,000-step run was killed a THIRD time at step 7,600, and the
  cause is diagnosed as memory pressure at 379 MiB of free disk, not a
  stop signal - not restarted, per instruction. Step 5: viability of the
  surviving 7,000-step checkpoint, warm-started - NOT VIABLE 8/8, but V3
  passes on 5/8 with positive drift and 75.6% applied at best; V2 is the
  real model failure and V1 was never testable at any stream length ever
  sampled. No VAL sweep (gated on viability); sweep.sh now refuses to run
  cold. Step 6: this state. TEST never read; the headline comparison was
  not run and was not approached.
- 2026-07-31 Step 5: cold-resume rewritten - Status leads with the one
  headline fact (no viable stream yet; the 90-minute-scale question is
  UNANSWERED because the budget run was stopped twice; the sealed run
  must not be scheduled until viability passes and the sweep has run);
  Blocked-on-you updated (architecture resolved, training-window item
  1b added, budget item partially resolved). Session: Steps 0/1/2/3
  committed, Step 4 blocked by its own gate. TEST never read.
