# Postmortem — what I tried, what failed, what I'd do differently

Written 2026-08-03, at the end of the project.

## What I set out to do

Train a language model on real limit-order-book flow and show it could generate
synthetic market data that reproduced real statistical signatures — fat tails,
volatility clustering, order-flow memory — better than a memoryless baseline.
The plan was four phases: build an exchange, build a generative loop, validate
against stylized facts, and (stretch) put an execution agent inside the sim.

The headline was supposed to be a three-column table: real / model / null.

## What actually happened

**The model does not work.** At real-day stream length it fails viability on
8 of 8 sampled streams. It generates plenty of *activity* — it clears the
activity bar for the first time in the project's history — but it cannot keep
a two-sided book alive. Its best stream ends the day two-sided 6.4% of the
time, against a 90% bar.

Worse, the failure gets *worse* the longer it runs: 100% → 69% → 34.5% → 23% →
17.2% → 6.4% two-sided across five and a half doublings of stream length. There
is no length at which it passes, and more sampling actively hurts.

There is no three-column table. There never will be from this model.

## Was it the model or the harness?

The model. This is the one thing I checked hardest, because the project had
already been fooled once by its own harness.

Real BX order flow, pushed through the *identical* pipeline with the same warm
start and the same criteria, **improves** with length: 99.77% applied, 100%
two-sided, 1,844 signs on a validation day; 99.90% / 100% / 4,137 on a training
day. The bar is reachable. The pipeline is not the problem.

## Why did it fail?

Partly known, partly not — and the honest answer matters more than a tidy one.

**Ruled out by measurement:**

- Not the harness (above).
- Not accumulated book drift alone — replaying 50k-token slices from a *fresh*
  real book does not rescue most of them.
- Not the action mix — the model's add/cancel/delete proportions match real
  data, and its best and worst slices are indistinguishable on it.
- Not the price-placement distribution — the model is *closer* to its training
  pool on this statistic than the median real day is, and the most extreme real
  day in the corpus is perfectly viable.

**What's left, unmeasured:** the *conditional* structure — which action, at
which level, **given the current book**. The model has the right marginals and
the wrong dependencies. That's a narrow target, and it's where I'd start.

## What I got wrong along the way

More useful than the result, honestly.

**1. Every viability measurement for weeks was too short to mean anything.**
Short streams flatter a bad model badly. At 50k tokens this model produces 60
aggressor signs against real data's 73 — nearly indistinguishable — and its
book is 100% two-sided. The failure only appears with length. Every viability
number recorded in this project before the final day was measuring nothing, and
one of them ("5 of 8 keep the book alive") had been treated as encouraging for
days.

*Lesson: if a metric can be computed at any scale, establish the scale at which
it discriminates before you trust a single reading of it.*

**2. I never measured the device default.** All the model work ran on the GPU
because that's what training used. The model is 227k parameters — far too small
to amortise GPU dispatch. **CPU is 10.8× faster.** I spent real effort building
a KV cache (a genuine 4.7× win) while a larger factor sat unexamined in a
default. The optimisation I *chose* was worth less than the one I never
questioned.

*Lesson: measure the defaults before optimising what sits on top of them.*

**3. I asserted mechanisms I hadn't tested, twice in one night.** Both times I
proposed the explanation confidently, ran the cheap check I'd recommended, and
watched it die within minutes. Both retractions are in RESULTS.md as new rows
naming the old ones. The work survived only because the checks were cheap.

*Lesson: the confident-sounding mechanism is the one to test first, not the one
to write down first.*

**4. Hardcoded absolute paths broke the project twice.** Once when a scratchpad
directory was wiped between sessions (costing a training run), once when an
interpreter was deleted while freeing disk. Both times the failure was silent
until something important died.

**5. The sampler wrote output only at the end of a run.** A killed run cost
everything, repeatedly, including a 100k-token run. This was known and not
fixed for days because each individual instance felt survivable.

**6. Training was killed four or five times** by memory pressure on a
near-full disk, with other jobs competing. Diagnosed correctly eventually, but
late — and one kill that *looked* like the same cause turned out to be an
unbounded memory leak in my own code. Blaming the environment is comfortable
and was wrong at least once.

**7. I ratified a scoring criterion before knowing the pipeline could carry
it.** The second scored fact — volatility-clustering persistence — turned out
to be destroyed by the generation path itself, and the experiment built to find
out why was degenerate by the project's own measurability rule. It had to be
demoted. It was never a fact the model failed; it was a fact the apparatus
couldn't measure.

*Lesson: before ratifying a metric, verify the pipeline can reproduce it on
data you already trust.*

## What's worth keeping

The apparatus, which doesn't depend on the model:

- **A correct exchange.** Two full days of real NASDAQ BX ITCH replay with zero
  rejects, exact share conservation, books draining to zero at the close.
  Fuzz-tested at a million messages against a shadow book.
- **A measurement pipeline** for order-flow statistics, with a rule that
  refuses degenerate estimates — it caught two of my own false positives.
- **A memoryless null model**, calibrated on real data, that a real model has
  to beat.
- **A tokenizer round-trip** from real ITCH and back, mutation-tested.
- **Two sealed held-out days** that have never been read. They're still worth
  something precisely because they weren't spent on a model that couldn't be
  scored.
- **A viability bar that real data demonstrably passes**, so a future attempt
  knows the target is real.

## What I'd try next

Cheapest first, and none of these needs a training run:

1. **Close the generation loop.** Sampling is currently blind to the book — the
   model emits level indices without seeing the levels. The adapter already
   exposes book-state feedback; the sampler doesn't use it. This is the most
   obvious suspect for the conditional-structure failure.
2. **Test whether the depth ratchet is repairable.** Right now depth can be
   destroyed anywhere but created only at the touch, which makes book damage
   permanent. That's why local mistakes compound into a dead book.
3. **Settle whether the day split is sound.** The days aren't exchangeable on
   price placement — leave-one-out distance runs up to 0.288, and the
   validation day sits outside the range of every training day. That needs
   resolving before the sealed days are ever spent.

Only after those would I spend another training run. Retraining without knowing
which of undertraining, blind sampling, or the ratchet dominates is picking a
change by guess.

## Rules of engagement, if this gets picked up

- Viability is measured at day scale (~1.07M tokens). Anything shorter is not a
  measurement.
- The sealed days stay sealed until there's a model worth spending them on.
- RESULTS.md and BENCH.md are append-only. Corrections get a new row naming the
  old one — that's why the two retractions above are still legible.
