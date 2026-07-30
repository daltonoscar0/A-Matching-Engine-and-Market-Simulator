# RESULTS - Exchange

| date | config | metric | takeaway |
|---|---|---|---|
| 2026-07-27 | commit 58cf5ce, fuzz seeds 1/2 + 10-17, FUZZ_N=1M, invalid_permille 0/50/100 | 0 invariant violations, 0 wrong accepts/rejects across ~2.16M messages; fuzz book == shadow book at end | Book core + codec hold under randomized valid+hostile streams; correctness gate green. Caveat: generator and book share one dev's assumptions - real LOBSTER replay is the honest test. |
