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
  2. Latency tail: p99.9=3.1us / max=9.2ms spikes - pre-reserve
     unordered_map buckets, then try a slab allocator for Order/Level;
     re-bench and add p99.9 column to BENCH.md.
  3. Multi-instrument dispatch: route on stock_locate to per-instrument
     books (real ITCH interleaves symbols; current bench is single-book).

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

## Blocked on you
- LOBSTER sample files (lobsterdata.com download needs a browser; container
  network is locked to package registries). Drop the message + orderbook CSVs
  into project knowledge or the repo.
- ITCH 5.0 PDF excerpt for spec diff (see Decisions).

## RESULTS log -> RESULTS.md, benchmarks -> BENCH.md
