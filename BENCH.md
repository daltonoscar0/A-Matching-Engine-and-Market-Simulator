# BENCH - Exchange

Hardware: 1-vCPU Intel Xeon @ 2.10GHz (container). Latency = decode+apply per
message via steady_clock (adds ~2x20ns clock overhead per sample). Throughput
measured separately in an untimed-per-message pass, best of 3.

| date | commit | msgs/sec | p50 | p99 | notes |
|---|---|---|---|---|---|
| 2026-07-27 | 58cf5ce | 5.48M | 193ns | 598ns | Synthetic 5M-msg stream (LOBSTER stand-in), seed 42, single instrument. Mix: A 36% F 4% D 33% U 12% E 5.6% C 1.4% X 8%. Caveats: (1) stream is net-adding, book drifts to ~347k live orders, so this includes deep-book stress but also unrealistic map depth; (2) p99.9=3.1us, max=9.2ms - untracked spike, likely alloc burst/preemption on shared vCPU, investigate with reserve()/pool next; (3) single hot instrument overstates cache locality vs a real feed. |

## Apple M4 (local) - not comparable to the container rows above

Hardware: Apple M4, 10 cores, macOS 25.3.0. Same methodology: latency =
decode+apply per message via steady_clock (clock read overhead included in
each sample), throughput from a separate untimed-per-message pass, best of 3.

| date | commit | stream | msgs/sec | p50 | p99 | p99.9 | max | notes |
|---|---|---|---|---|---|---|---|---|
| 2026-07-30 | phase1-baseline | synth 5M seed 42 | 6.27M | 83ns | 1000ns | 1583ns | 13.75ms | Local baseline before any changes. steady_clock on this machine ticks at ~41.7ns, so sub-100ns percentiles are quantized (83ns = 2 ticks). max spike is a one-off; cause investigated in Phase B. |
| 2026-07-30 | phase B | synth 5M seed 42 | 6.06M | 83ns | 1000ns | 1541ns | 22.4us | Order pool pre-reserved (1M buckets) after bench_tail attributed max to unordered_map rehash (RESULTS.md 2026-07-30). max 13.75ms -> 22.4us; p50/p99/p99.9 unchanged - rehash never touched them. Throughput 6.06M vs 6.27M baseline is within run-to-run spread (passes ranged 5.61-6.27M), not a regression claim. Residual max is rare allocator/OS noise, ~top-10-in-5M territory. |
| 2026-07-30 | phase C | synth 5M seed 42, single symbol, Book path | 6.19M | 83ns | 1083ns | 1625ns | 22.9us | Regression check after BookSet landed: single-symbol path untouched, numbers match phase B within run spread (best of 5; passes ranged 5.42-6.19M). |
| 2026-07-30 | phase C | synth 5M seed 42, single symbol, via BookSet | 6.12M | 83ns | 1042ns | 1667ns | 505us | Routing overhead of BookSet (locate -> direct-indexed vector) on a single-symbol stream: nil within noise. max is one 505us outlier in 5M (allocator/OS noise class per RESULTS.md 2026-07-30); the other 4 passes audited clean too. |
| 2026-07-30 | phase C | synth 5M seed 42, 8 interleaved symbols, via BookSet | 5.91M | 83ns | 1167ns | 1792ns | 128.6us | Interleaved is ~5% slower than single-symbol at best-of-5 with a fatter mid-tail (p90 625 vs 541ns) - the expected cache cost of touching 8 books. Modest here because total open orders (~347k) match the single-book case and per-book level maps are shallower; a real feed with hundreds of symbols would look worse. Reserve 2^18 buckets/book (peak per-book open ~43k). |
