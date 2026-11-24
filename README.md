# exchange

An attempt to train a language model to generate realistic limit-order-book
flow, validated against real NASDAQ BX data.

**It didn't work.** The model does not sustain a live, two-sided book. There is
no headline result. What's here is a working exchange engine, a validated
measurement pipeline, a memoryless baseline, and a well-documented failure.

**Start here: [`docs/POSTMORTEM.md`](docs/POSTMORTEM.md)**, what was tried,
what failed, what I got wrong, and what I'd try next.

## The result, in one table

Viability at real-day stream length, warm-started from a real book:

| stream source | applied | two-sided | signs | verdict |
|---|---|---|---|---|
| Real BX flow (validation day) | 99.77% | 100.0% | 1,844 | passes |
| Real BX flow (training day) | 99.90% | 100.0% | 4,137 | passes |
| The model (32k steps, 227k params) | 50.77% | 6.4% | 536 | **fails, 8/8** |

Real data passes the same bar through the same pipeline, so the target is
reachable and the harness isn't the problem. The model generates enough
activity but cannot keep both sides of the book alive, and it gets worse the
longer it runs.

## What works and is reusable

- **Exchange engine**, price-time priority book + matching. Two full days of
  real ITCH replayed with zero rejects, exact share conservation, books
  draining to zero at the close (23.8M and 109.7M messages). Fuzz-tested at 1M
  messages against a shadow book.
- **Measurement pipeline**, stylized facts (fat tails, volatility clustering,
  flow-sign memory, Hill) per symbol, with a rule that refuses degenerate
  estimates.
- **Memoryless null**, Cont-Stoikov-Talreja calibrated on real data, the floor
  a real model has to beat.
- **Tokenizer round-trip**, real ITCH to factored tokens and back,
  mutation-tested.
- **Two sealed held-out days**, never read.

## Build & test

    cmake -B build && cmake --build build -j && ctest --test-dir build
    FUZZ_N=1000000 ./build/tests "fuzz*"     # full 1M-message fuzz gate
    /usr/bin/python3 pylm/test_kvcache.py    # sampler gates

Zero external dependencies. The single exception is Catch2 v2.13.10, vendored
at `third_party/catch.hpp`. Standard library + POSIX only.

## Reproducing the result

    # snapshot a real book at 09:30
    ./build/warm_book data/20190730.BX_ITCH_50 --ticker SPY -o SPY_0930.book

    # generate a stream (CPU - this model is far too small for a GPU to help)
    /usr/bin/python3 out/tokens/sample.py --ckpt out/tokens/run32k/budget32k_v2.pt \
        --out-prefix out/tokens/sample32k/VAL --n-tokens 1070000 --batch 8 \
        --device cpu --ticker SPY

    # viability verdict (--why explains each rejected event)
    ./build/sim_health out/tokens/sample32k/VAL_s0.tokens.bin \
        --manifest out/tokens/manifest.json --ticker SPY \
        --warm-start SPY_0930.book --why

Viability must be measured at day scale (~1.07M tokens). Shorter streams
flatter a bad model badly, at 50k tokens this model is nearly
indistinguishable from real data.

## Layout

    src/book.{hpp,cpp}      price-time priority book, invariant ledger + audit
    src/itch.{hpp,cpp}      ITCH 5.0 subset (A F E C X D U) + framed codec
    src/match.hpp           matching path - decides and reports fills
    src/feed.hpp            ITCH message -> book operation
    src/bookset.hpp         multi-symbol routing by stock_locate
    src/dataset.hpp         the TRAIN/VAL/TEST split, enforced not advised
    src/cst.hpp             Cont-Stoikov-Talreja memoryless null
    src/oftk.hpp            factored token format (5-tuple, 52 ids)
    src/itch_tokenize.hpp   real ITCH -> tokens
    src/token_shim.hpp      tokens -> order actions
    src/adapter.hpp         action -> engine, with book-state feedback
    src/warm_book.hpp       real book snapshot for warm starts

    tools/stylized          stylized-fact measurement
    tools/sim_health        viability verdict
    tools/lm_sim            generated tokens -> ITCH, for scoring
    tools/cst_calibrate     fit the null on TRAIN
    tools/cst_sim           generate null days
    tools/ablate            isolate which pipeline stage costs which fact
    tools/warm_book         make a 09:30 book snapshot

    pylm/kvcache.py         KV-cached sampler
    tests/                  Catch2 property tests + 1M-message fuzzer
    bench/                  throughput and tail-latency benchmarks

## The record

- [`docs/POSTMORTEM.md`](docs/POSTMORTEM.md), what failed and why
- [`docs/CLAIM.md`](docs/CLAIM.md), exactly what is and is not claimed
- `PLAN.md`, phases, dated Decisions, session log. Source of truth.
- `RESULTS.md`, `BENCH.md`, append-only measurement logs. Corrections get a
  new row naming the old one; nothing is edited away, including two mechanism
  claims I asserted and then retracted.

`data/` and `out/` are gitignored.
