# BENCH - Exchange

Hardware: 1-vCPU Intel Xeon @ 2.10GHz (container). Latency = decode+apply per
message via steady_clock (adds ~2x20ns clock overhead per sample). Throughput
measured separately in an untimed-per-message pass, best of 3.

| date | commit | msgs/sec | p50 | p99 | notes |
|---|---|---|---|---|---|
| 2026-07-27 | 58cf5ce | 5.48M | 193ns | 598ns | Synthetic 5M-msg stream (LOBSTER stand-in), seed 42, single instrument. Mix: A 36% F 4% D 33% U 12% E 5.6% C 1.4% X 8%. Caveats: (1) stream is net-adding, book drifts to ~347k live orders, so this includes deep-book stress but also unrealistic map depth; (2) p99.9=3.1us, max=9.2ms - untracked spike, likely alloc burst/preemption on shared vCPU, investigate with reserve()/pool next; (3) single hot instrument overstates cache locality vs a real feed. |
