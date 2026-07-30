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
