# exchange

[![tests](https://github.com/daltonoscar0/A-Matching-Engine-and-Market-Simulator/actions/workflows/tests.yml/badge.svg)](https://github.com/daltonoscar0/A-Matching-Engine-and-Market-Simulator/actions/workflows/tests.yml)

A limit-order-book exchange engine in C++17 — price-time priority, matching,
and an ITCH 5.0 reconstruction path — validated by replaying two full days of
real NASDAQ BX data. Zero external dependencies beyond a vendored Catch2.

It was built as the measurement harness for an attempt to train a language
model to generate realistic order flow. **That attempt failed**, and the
failure is documented rather than buried: see
[the order-flow simulator](#the-order-flow-simulator-a-negative-result) below
and [`docs/POSTMORTEM.md`](docs/POSTMORTEM.md). The engine itself works.

## The engine, on real exchange data

Two full NASDAQ BX ITCH 5.0 days replayed through the book, end to end:

| Day | Book messages | Rejects | Invariant violations | Books audited | Share conservation | End-of-day |
|---|--:|--:|--:|--:|---|---|
| 2019-07-30 | 23,821,586 | **0** | **0** | 7,497 clean | exact | every book drains to 0 |
| 2018-12-28 | 109,732,869 | **0** | **0** | 7,220 clean | exact | every book drains to 0 |

"Exact conservation" is checked share for share, not approximately:

- **2019-07-30** — 8.43B shares added = 79.0M executed + 8.35B canceled + **0 resting**
- **2018-12-28** — 21.90B shares added = 140.1M executed + 21.76B canceled + **0 resting**

Every add is accounted for by execute/cancel/delete/replace flow, and no book
holds a single share at the close. The second day is a different year, a
different volatility regime (the tail of the Q4-2018 selloff) and 4.6× the
message volume, which is what makes it a generalisation check rather than a
re-run.

### Fuzzing

`tests/test_fuzz.cpp` drives randomized message sequences — including
deliberately invalid ones — against the book and compares it to an
independently maintained shadow book, single- and multi-symbol. The default is
200k messages so `ctest` stays quick; the full gate is 1M:

```bash
FUZZ_N=1000000 ./build/tests "fuzz*"
```

### Throughput and latency

From [`BENCH.md`](BENCH.md). **Apple M4, 10 cores, single-threaded.** Latency is
`steady_clock` around decode+apply per message, with clock-read overhead
included in every sample; throughput comes from a separate untimed pass, best
of 3–5.

| Path | Stream | Throughput | p50 | p99 | p99.9 |
|---|---|--:|--:|--:|--:|
| Replay (real) | BX 2019-07-30, 23.82M msgs, 7,497 symbols | 5.27M msgs/s | 125ns | 875ns | 1,583ns |
| Replay (synthetic) | 5M msgs, single symbol, deep book | 6.27M msgs/s | 83ns | 1,000ns | 1,583ns |
| Matching | 5M ops, 29% crossing, 2.64 fills/crossing order | 4.72M matches/s | 167ns † | 792ns † | 2,000ns † |
| Adapter loop | 5M actions incl. validation + BookView feedback | 3.38M steps/s | — | — | — |
| ITCH ingest | BX 2019-01-30, 74.18M msgs + tokenization | 3.88M msgs/s | — | — | — |

† The matching row is quoted for **crossing** orders, the ones that do the
work. Counting every `match_submit` call, including resting limits that never
cross, it is p50 83ns / p99 541ns / p99.9 1,375ns.

Read these with the caveats `BENCH.md` attaches to them, which are the point of
keeping that file:

- **The rows are not comparable to each other.** Matching does real work per
  order that reconstruction does not; the adapter row includes its own
  generator. `BENCH.md` marks each new path explicitly.
- **Nor to the container rows in `BENCH.md`** (1-vCPU Xeon, 5.48M msgs/s,
  p50 193ns). Different hardware, kept for the record, not for comparison.
- `steady_clock` on the M4 ticks at ~41.7ns, so sub-100ns percentiles are
  quantized — a p50 of 83ns is two ticks.
- Real BX is a thin venue (peak 270 open orders per book). The synthetic rows
  stress map depth (~347k live orders); the real rows stress book count and
  locality. Neither alone covers both.
- Maxima are worse than p99.9 and `BENCH.md` says so: a 397µs outlier on the
  real replay, in the allocator/OS-noise class, after pre-reserving the order
  pool took the synthetic max from 13.75ms to 22.4µs.

### Also validated, beyond internal consistency

Conservation alone cannot catch a wrong decode offset, since the same wrong
field is both added and removed. So `tools/field_sanity` and `tools/bbo_trace`
check the reconstruction against what real equity flow should look like: the
shares field spikes hard at 100 (48.5% exactly 100, 91.5% round multiples of
100), prices are 99.4% whole-penny with no zeros, and the top five books by
volume reproduce a two-sided market at 100% of 391 one-minute samples, with
median spreads of $0.04 on SPY and $0.03 on IWM.

## Build and test

```bash
cmake -B build && cmake --build build -j && ctest --test-dir build
FUZZ_N=1000000 ./build/tests "fuzz*"     # full 1M-message fuzz gate
```

Standard library and POSIX only. The single exception is Catch2 v2.13.10,
vendored at `third_party/catch.hpp`. CI builds the library, the tests and the
tools, and runs `ctest`; the benchmarks build but are not timed there, because
a shared runner cannot produce a number worth recording.

The sampler gates are a separate matter:

```bash
/usr/bin/python3 pylm/test_kvcache.py    # needs torch and a local tape checkout
```

TODO(oscar): `pylm/test_kvcache.py` does `sys.path.insert(0, "/Users/oscardalton/tape")`,
a hardcoded absolute path, so it only runs on your machine and is not in CI. The
module it wants is `tape/model_loader.py` in
[Interpretability-of-Market-Language-Models](https://github.com/daltonoscar0/Interpretability-of-Market-Language-Models).

## The order-flow simulator: a negative result

The engine above exists to measure a generative model of order flow. The model
**did not work**, and this is the whole result:

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

Viability has to be measured at day scale (~1.07M tokens). Shorter streams
flatter a bad model badly: at 50k tokens this model is nearly indistinguishable
from real data.

**[`docs/POSTMORTEM.md`](docs/POSTMORTEM.md) is the honest account** — what was
tried, what failed, what was got wrong, and what to try next.

A useful by-product: a memoryless Cont-Stoikov-Talreja null, calibrated on the
training days, already reproduces fat tails, the aggregational-Gaussianity
shape, the Hill index and the sign of the bounce. So none of those facts can
distinguish a trained model from noise. What the null does *not* reproduce is
flow-sign memory (ACF(1) ≈ 0 against 0.277 real; decay slope ≈ 0 against
−0.59) or volatility clustering. That rule was written down before the run, and
it is why those two are the only legitimate scoring targets.

### Reproducing it

```bash
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
```

## What is here and reusable

- **Exchange engine** — price-time priority book plus matching, with an
  invariant ledger and per-book audit.
- **Measurement pipeline** — stylized facts (fat tails, volatility clustering,
  flow-sign memory, Hill) per symbol, with a rule that refuses degenerate
  estimates.
- **Memoryless null** — Cont-Stoikov-Talreja calibrated on real data, the floor
  any real model has to beat.
- **Tokenizer round-trip** — real ITCH to factored tokens and back,
  mutation-tested.
- **Two sealed held-out days**, never read. The split is mechanical and
  enforced in code, not advised: `stylized` on a TEST day exits 3 and names the
  override required.

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
    tools/field_sanity      decode-offset sanity against real-flow structure
    tools/bbo_trace         market-reproduction check (spreads, two-sidedness)

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

## License

MIT. See [LICENSE](LICENSE).
