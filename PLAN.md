# PLAN - Exchange

## Phase 1: Matching engine core (weeks 1-3)
- [x] Order book data structure (price levels, FIFO queues); add/cancel/replace/execute
- [x] ITCH-style binary message codec + fuzz tests (A, F, E, C, X, D, U)
- [x] Property tests: no crossed book, FIFO within level, share conservation,
      unknown/duplicate-id rejection; randomized fuzzer checks all four after
      every message (1M-message runs clean, `FUZZ_N=1000000 ./tests "fuzz*"`)
- [~] BENCH.md: msgs/sec + p50/p99 - first row recorded on a 5M-message
      SYNTHETIC stream (LOBSTER sample not available in this environment;
      synth.hpp generates realistic type ratios as a stand-in). Re-run on the
      real LOBSTER file to close this box.
Milestone: replay a full LOBSTER day byte-identically - BLOCKED on data (below).

## Phase 2: Generative agent loop (weeks 4-6)
- [ ] Adapter: orderflow-lm emits messages -> engine executes -> state feeds back
- [ ] Sampling controls (temperature, top-k) + rejection of malformed messages
Milestone: closed-loop simulation runs N steps without invariant violations.

## Phase 3: Stylized-fact validation (weeks 7-9)
- [ ] Fat tails (return kurtosis), volatility clustering (ACF of |r|),
      order-flow autocorrelation, square-root impact fit
- [ ] Compare vs Cont-Stoikov-Talreja null model
Milestone: table of stylized facts, real vs LM-sim vs null - the headline result.

## Phase 4 (stretch): Execution agent
- [ ] Almgren-Chriss baseline; RL or policy-gradient agent inside the sim

## Status
- Current phase: 1 (core + codec + tests done; commit 58cf5ce)
- Next 3 tasks:
  1. LOBSTER ingestion: parser from LOBSTER message CSV -> itch::Message
     stream; replay a real day, verify book state against the LOBSTER
     orderbook file at every row (the "byte-identical" milestone), re-bench.
  2. [x] Latency tail: DONE 2026-07-30 - max was order-pool rehash, fixed
     with Book::reserve; slab allocator not justified by measurement
     (see Decisions + RESULTS.md).
  3. [x] Multi-instrument dispatch: DONE 2026-07-30 - BookSet routes on
     stock_locate, fuzzer covers 8 interleaved symbols, single-symbol path
     unregressed (BENCH.md phase C rows).

## Decisions
- 2026-07-27 Catch2 v2.13.10 vendored as the single allowed dependency
  (third_party/catch.hpp): property-style SECTIONs + test discovery for the
  cost of one header, zero build-system impact. Fuzz hot loop uses raw
  aborts/FAIL, not per-message CHECKs (Catch2 macro overhead would dominate).
- 2026-07-27 ITCH message layouts taken from the public NASDAQ TotalView-ITCH
  5.0 spec from memory: the PDF excerpt named in README.md is NOT in project
  knowledge. A/F/E/C/X/D/U layouts (36/40/31/36/23/19/35 bytes, big-endian,
  48-bit ns timestamps) are unambiguous in the public spec, so this did not
  meet the stop-and-ask bar - but upload the excerpt and diff before
  Phase 1 sign-off.
- 2026-07-27 File framing: 2-byte big-endian length prefix per message
  (BinaryFILE convention), not SoupBinTCP session packets. Right for replay
  files; revisit if we ever speak the wire live.
- 2026-07-27 Book semantics are RECONSTRUCTION, not self-matching: the book
  applies an event stream where executions arrive as explicit E/C messages
  (matching happened upstream at the "exchange"). "Book never crosses" is
  enforced by REJECTING adds/replaces priced through the opposite side -
  correct for ITCH replay, where crossing flow executes and never rests.
  Phase 2's generative loop needs an actual matcher (agent sends a crossing
  order -> engine produces E/C messages); that is a planned new code path
  (`match()`), not a change to this one.
- 2026-07-27 Data structures: levels in std::map (bids descending, asks
  ascending; begin() = best), orders in unordered_map<ref, Order>, orders
  chained per-level in an intrusive doubly-linked FIFO. Both containers are
  pointer-stable, giving O(1) cancel/delete/execute-by-id and O(log L) adds.
  Boring and correct first; measured 5.48M msgs/sec, so no exotic structure
  is justified yet.
- 2026-07-27 Conservation ledger in the book itself (added = resting +
  executed + canceled, O(1) check) + a deep O(n) audit() that re-walks every
  level/FIFO chain. Fuzzer runs the O(1) check every message, audit every 4096.
- 2026-07-27 Replace ('U') = cancel remainder + add at back of queue (loses
  time priority), remainder counted as canceled - matches ITCH semantics.
- 2026-07-27 stock_locate ignored for now: one Book per process. See next
  task 3.
- 2026-07-27 Synthetic generator (src/synth.hpp) owns a shadow Book so valid
  messages are valid by construction; fuzzer replays the same stream into an
  independent Book through the full encode->decode->apply pipeline and
  cross-checks final state against the shadow.
- 2026-07-30 Latency-tail attribution done with a dedicated tool
  (bench/tail.cpp) rather than guessing: per-message timing + order-pool
  bucket-count tracking, spikes annotated with type/pool-size/rehash flag and
  bucketed per 500k-message window to separate size-correlated causes from
  uniform preemption. Tool kept in-repo so the measurement is reproducible.
- 2026-07-30 Tail fix = Book::reserve(n) (pre-size the order pool's hash
  buckets), called by the bench with 1<<20. NOT a reserve in Book's
  constructor: Phase C runs many books per process and a megabucket table per
  instrument would be the wrong default. Callers that know their depth opt in.
- 2026-07-30 Slab/freelist allocator for Order/Level NOT built: measurement
  (RESULTS.md 2026-07-30) shows it could only chase the residual ~100us
  worst-case allocator noise (~10 messages in 5M) and cannot move p99.9,
  which is ordinary deep-book map work plus clock overhead.
- 2026-07-30 BookSet routing = vector<unique_ptr<Book>> indexed directly by
  stock_locate (16-bit key, worst case 512KB of pointers): O(1), no hash, no
  iterator invalidation. Books created on first touch. Single-book path
  (feed.hpp apply on Book&) untouched; BookSet layers on top of it.
- 2026-07-30 Multi-symbol synth: per-symbol Generators interleaved by a
  separate pick-rng, one locate each, order refs globally unique via
  per-symbol ref_base ((i+1)<<40, clear of the 0xDEAD... unknown-id range).
  Unique refs are what make routing bugs fuzzable: a message applied to the
  wrong book hits UnknownId/DuplicateId instead of silently succeeding.
  Config defaults chosen so single-symbol output stays byte-identical to
  pre-change streams (verified by md5 on the seed-42 5M file).
- 2026-07-30 Merged multi-symbol stream is not globally timestamp-monotonic
  (per-symbol clocks advance independently). Accepted: no consumer reads
  cross-symbol time order, and the generator is a stand-in, not a market.

## Blocked on you
- LOBSTER sample files (lobsterdata.com download needs a browser; container
  network is locked to package registries). Drop the message + orderbook CSVs
  into project knowledge or the repo.
- ITCH 5.0 PDF excerpt for spec diff (see Decisions).
- 2026-07-30 Phase A blocked: LOBSTER sample needs manual download.
  lobsterdata.com is now a JS app; the old direct sample zip URLs
  (/info/sample/LOBSTER_SampleFile_*.zip) return the app shell, and the app's
  own bundle shows sample downloads are gated behind an email + purchase-proof
  request flow ("We verify purchase proof and send a time-limited download
  link"). Not freely fetchable; nothing on local disk either. Drop
  ..._message_N.csv + ..._orderbook_N.csv into data/ and Phase A can proceed.

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
