# Exchange — working rules

Matching engine + generative order-flow project. PLAN.md is the source of
truth for phases, decisions, and the session log. Read it first.

## Priorities
1. Momentum: ship the next structural piece; don't gold-plate.
2. Rigor: but never at the cost of a weakened invariant or a skipped gate.
When a result looks surprising, write down the boring explanation (bug,
estimator artifact, thin venue) before the exciting one (new phenomenon).

## Dependencies
Zero external dependencies. The single exception is Catch2 v2.13.10,
vendored at third_party/catch.hpp. Do not add libraries, package managers,
or FetchContent. Standard library + POSIX only.

## Gates — all three green before EVERY commit
1. Zero-warning build: `cmake --build build` clean under -Wall -Wextra.
2. `ctest --test-dir build` passes.
3. Fuzz at full depth: `FUZZ_N=1000000 ./build/tests "fuzz*"` passes.
Never weaken, skip, or reduce the depth of a test to get to green. If a
test is wrong, fixing it is a Decision to log in PLAN.md, not an edit to
slip by.

## Tests and results
- Tests live alongside code (tests/), written with the feature, not after.
- Property/invariant style preferred: conservation, no crossed book, FIFO,
  reject-unknown. The fuzzer cross-checks against a shadow book.
- RESULTS.md and BENCH.md are append-only logs: add rows, never edit or
  delete existing ones. A corrected number gets a new row that names the
  old one.
- Real-data bar is absolute: replaying genuine ITCH must produce ZERO
  rejects and exact share conservation. Any reject on real data is our bug.

## Decision authority
Implementation details are decided autonomously; non-obvious calls are
logged in PLAN.md Decisions (dated, with rationale). Stop and ask only
for: scope changes, destructive/irreversible actions, or anything that
would change what claim the project can honestly make.

## Data
data/ holds real NASDAQ BX ITCH days (~400MB-1.6GB each), gitignored —
keep it that way. Verify downloads with `gzip -t` (the .md5sum sidecars
404). Always `curl -f` so HTTP errors don't get saved as files.
