# exchange — a benchmark for generative order-flow models

Real NASDAQ BX ITCH 5.0 data, a limit-order-book engine that is provably
correct on it, a calibrated memoryless floor, a pre-registered scoring rule,
and two sealed held-out days.

**Start here: [`docs/BENCHMARK.md`](docs/BENCHMARK.md)** — the task, the bars,
the standings, and how to run an entry.

## The short version

Generate order flow for one symbol, from a real book warm-started at 09:30.
Two stages:

1. **Viability** — does your stream sustain a live, two-sided book for a full
   day? Measured at day scale (~1.07M tokens). Real BX flow passes; the one
   model entered so far does not.
2. **Scoring** — does it reproduce flow-sign memory better than a memoryless
   Cont-Stoikov-Talreja null, per symbol, against a pre-registered
   PASS/FAIL/INCONCLUSIVE rule?

Current standings: **no entry has passed viability.** Real data passes it, so
the bar is reachable. The sealed TEST days have never been read.

## Build & test

    cmake -B build && cmake --build build -j && ctest --test-dir build
    FUZZ_N=1000000 ./build/tests "fuzz*"     # full 1M-message fuzz gate
    /usr/bin/python3 pylm/test_kvcache.py    # sampler gates

Zero external dependencies. The single exception is Catch2 v2.13.10, vendored
at `third_party/catch.hpp`. Standard library + POSIX only.

## Layout

    src/book.{hpp,cpp}      price-time priority book, invariant ledger + audit
    src/itch.{hpp,cpp}      ITCH 5.0 subset (A F E C X D U) + framed codec
    src/match.hpp           matching path (match_submit) - decides and reports fills
    src/feed.hpp            ITCH message -> book operation
    src/bookset.hpp         multi-symbol routing by stock_locate
    src/dataset.hpp         THE split. TRAIN/VAL/TEST, enforced not advised
    src/cst.hpp             Cont-Stoikov-Talreja memoryless null
    src/oftk.hpp            factored token format (5-tuple, 52 ids)
    src/itch_tokenize.hpp   real ITCH -> tokens
    src/token_shim.hpp      tokens -> order actions
    src/adapter.hpp         action -> engine, with book-state feedback
    src/warm_book.hpp       real book snapshot for warm starts

    tools/stylized          the measurement apparatus (stylized facts)
    tools/sim_health        viability verdict (V1/V2/V3)
    tools/lm_sim            generated tokens -> ITCH, for scoring
    tools/cst_calibrate     fit the null on TRAIN
    tools/cst_sim           generate null days
    tools/ablate            isolate which pipeline stage costs which fact
    tools/warm_book         make a 09:30 book snapshot

    pylm/kvcache.py         KV-cached sampler for the reference model
    tests/                  Catch2 property tests + 1M-message fuzzer
    bench/                  throughput and tail-latency benchmarks

## The record

- [`docs/BENCHMARK.md`](docs/BENCHMARK.md) — the benchmark itself
- [`docs/CLAIM.md`](docs/CLAIM.md) — exactly what is and is not claimed
- `PLAN.md` — phases, dated Decisions, session log. Source of truth.
- `RESULTS.md`, `BENCH.md` — append-only measurement logs. Corrections get a
  new row naming the old one; nothing is edited away.

`data/` holds the real BX days and is gitignored. `out/` holds generated
artifacts and is gitignored.
