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
- [ ] Adapter: orderflow-lm emits messages -> engine executes -> state feeds back
- [ ] Sampling controls (temperature, top-k) + rejection of malformed messages
Milestone: closed-loop simulation runs N steps without invariant violations.

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
- [ ] Fat tails (return kurtosis), volatility clustering (ACF of |r|),
      order-flow autocorrelation, square-root impact fit - for the LM sim
      and null columns once Phase 2 exists
- [ ] Compare vs Cont-Stoikov-Talreja null model
Milestone: table of stylized facts, real vs LM-sim vs null - the headline result.

## Phase 4 (stretch): Execution agent
- [ ] Almgren-Chriss baseline; RL or policy-gradient agent inside the sim

## Status
- Current phase: 2 (engine side ready: match() landed 2026-07-30; the
  adapter to orderflow-lm is the remaining Phase 2 structural work).
- Next 3 tasks:
  1. Phase 2 adapter: orderflow-lm emits messages -> match_submit executes
     -> emitted stream feeds back. The engine side exists (src/match.hpp);
     what remains is the LM-facing loop and message validation.
  2. Sampling controls (temperature, top-k) + rejection of malformed
     messages at the adapter boundary.
  3. Multi-day stylized-facts baseline: process the 8 additional BX days in
     data/ through tools/stylized, report cross-day medians so the "real"
     column is no longer one draw.

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

## Blocked on you
- (nothing) - resolved 2026-07-30:
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
- 2026-07-30 match() mutation re-check: introduced an off-by-one in FIFO
  fill ordering (match takes head->next instead of head). The FIFO
  property test fails (4 assertions) AND the strong emit->reconstruct
  fuzz test aborts - the strong test catches fill-ordering bugs
  independently of the property tests. Mutation reverted, book.cpp
  byte-identical to HEAD, gates re-run green.
