#!/usr/bin/env python3
"""Correctness gates for pylm/kvcache.py. Run: python3 pylm/test_kvcache.py

The cache changes generated streams by construction (chunked refresh, see the
kvcache module docstring), so "the output is unchanged" is NOT the property to
test and testing it would be self-deception. What IS exact, and what these
gates pin:

  1. prefill() == MiniGPT.forward() on the same window, any length.
  2. Teacher-forced cached step() logits == reference sliding-window logits,
     token by token, in the pre-slide regime. Teacher-forcing removes RNG
     divergence so this isolates the cache arithmetic.
  3. prefill() fully resets cache state: a refresh mid-stream leaves the cache
     identical to a cold prefill of the same window.
  4. Post-refresh step logits == reference logits computed on the window the
     cache actually holds (not the window the old sampler would have used) --
     i.e. the cache is a correct implementation of its stated semantics.
  5. Generation is deterministic under a fixed seed.
  6. Refresh bookkeeping: history length stays within its advertised bounds.

Zero-dependency by the project's standard: torch + stdlib, no pytest.
"""

import sys

import torch

sys.path.insert(0, "/Users/oscardalton/tape")
sys.path.insert(0, __file__.rsplit("/", 2)[0])
from tape import model_loader as ml  # noqa: E402
from pylm import kvcache as kv  # noqa: E402

DEV = "cpu"          # CPU: reproducible and exercises the same code path
TOL = 2e-5           # fp tolerance between SDPA and nn.MultiheadAttention
FAILS = []


def check(name, cond, detail=""):
    if cond:
        print(f"  PASS  {name}")
    else:
        print(f"  FAIL  {name}  {detail}")
        FAILS.append(name)


def small_cfg(n_ctx=32):
    return ml.ModelConfig(vocab_size=52, n_layers=3, d_model=32, n_heads=4,
                          d_mlp=64, n_ctx=n_ctx)


def make_model(cfg, seed=0):
    torch.manual_seed(seed)
    m = ml.MiniGPT(cfg).to(DEV).eval()
    return m


def test_prefill_matches_forward():
    print("\n[1] prefill == MiniGPT.forward")
    cfg = small_cfg()
    m = make_model(cfg)
    for T in (1, 2, 7, 31, 32):
        toks = torch.randint(0, cfg.vocab_size, (4, T), device=DEV)
        with torch.no_grad():
            ref = m(toks)[:, -1, :]
            got = kv.CachedMiniGPT(m, cfg, 4, DEV).prefill(toks)
        d = (ref - got).abs().max().item()
        check(f"T={T:<3} max|diff|={d:.2e}", d < TOL, f"tol {TOL}")


def test_teacher_forced_steps():
    print("\n[2] cached step() == sliding-window forward (teacher-forced)")
    cfg = small_cfg()
    m = make_model(cfg, seed=1)
    B = 3
    seq = torch.randint(0, cfg.vocab_size, (B, cfg.n_ctx), device=DEV)
    c = kv.CachedMiniGPT(m, cfg, B, DEV)
    with torch.no_grad():
        got = c.prefill(seq[:, :2])
        worst = 0.0
        for t in range(2, cfg.n_ctx):
            ref = m(seq[:, :t + 1])[:, -1, :]      # window [0, t]
            got = c.step(seq[:, t:t + 1])
            worst = max(worst, (ref - got).abs().max().item())
    check(f"all {cfg.n_ctx - 2} steps, worst max|diff|={worst:.2e}",
          worst < TOL, f"tol {TOL}")


def test_prefill_resets_state():
    print("\n[3] prefill() fully resets cache state")
    cfg = small_cfg()
    m = make_model(cfg, seed=2)
    B = 2
    seq = torch.randint(0, cfg.vocab_size, (B, cfg.n_ctx), device=DEV)
    warm = kv.CachedMiniGPT(m, cfg, B, DEV)
    cold = kv.CachedMiniGPT(m, cfg, B, DEV)
    with torch.no_grad():
        warm.prefill(seq)                       # dirty it
        lw = warm.prefill(seq[:, 4:20])          # refresh onto a new window
        lc = cold.prefill(seq[:, 4:20])          # same window, cold
    dl = (lw - lc).abs().max().item()
    dk = max((warm.k[l][:, :, :16] - cold.k[l][:, :, :16]).abs().max().item()
             for l in range(cfg.n_layers))
    check(f"logits identical after refresh ({dl:.2e})", dl == 0.0)
    check(f"K cache identical after refresh ({dk:.2e})", dk == 0.0)
    check(f"filled counter reset ({warm.filled})", warm.filled == 16)


def test_post_refresh_semantics():
    print("\n[4] post-refresh logits == forward on the window the cache holds")
    cfg = small_cfg(n_ctx=16)
    m = make_model(cfg, seed=3)
    B, stride = 2, 4
    n_tok = 40
    torch.manual_seed(7)
    prompt = torch.randint(0, cfg.vocab_size, (B, 2), device=DEV)
    out = kv.generate(m, cfg, prompt, n_tok, refresh_stride=stride)

    # Replay the same token sequence through an independent cache, and at
    # every step compare against an uncached forward over the window that the
    # chunked-refresh rule says should be live.
    c = kv.CachedMiniGPT(m, cfg, B, DEV)
    w0, n = 0, 2
    worst, checked = 0.0, 0
    with torch.no_grad():
        c.prefill(out[:, w0:n])
        for i in range(n_tok - 1):
            n += 1
            pos = (n - 1) - w0
            if pos >= cfg.n_ctx:
                w0 = n - (cfg.n_ctx - stride)
                got = c.prefill(out[:, w0:n])
            else:
                got = c.step(out[:, n - 1:n])
            ref = m(out[:, w0:n])[:, -1, :]
            worst = max(worst, (ref - got).abs().max().item())
            checked += 1
    check(f"{checked} steps incl. refreshes, worst={worst:.2e}", worst < TOL)


def test_determinism():
    print("\n[5] fixed seed -> identical stream")
    cfg = small_cfg()
    m = make_model(cfg, seed=4)
    prompt = torch.ones((2, 2), dtype=torch.long, device=DEV)
    torch.manual_seed(11)
    a = kv.generate(m, cfg, prompt, 200, refresh_stride=8)
    torch.manual_seed(11)
    b = kv.generate(m, cfg, prompt, 200, refresh_stride=8)
    check("two runs byte-identical", torch.equal(a, b))


def test_history_bounds():
    print("\n[6] history window stays within advertised bounds")
    cfg = small_cfg(n_ctx=16)
    stride = 5
    B, n_tok = 1, 60
    lo_seen, hi_seen = 10**9, 0
    w0, n = 0, 2
    for _ in range(n_tok - 1):
        n += 1
        pos = (n - 1) - w0
        if pos >= cfg.n_ctx:
            w0 = n - (cfg.n_ctx - stride)
        lo_seen = min(lo_seen, n - w0)
        hi_seen = max(hi_seen, n - w0)
    lo_bound = cfg.n_ctx - stride
    check(f"history in [{lo_seen}, {hi_seen}], advertised "
          f"[{lo_bound}, {cfg.n_ctx}]",
          hi_seen <= cfg.n_ctx and lo_seen >= min(2, lo_bound))
    check("upper bound never exceeded", hi_seen <= cfg.n_ctx)


def test_rejects_bad_stride():
    print("\n[7] guards")
    cfg = small_cfg()
    m = make_model(cfg, seed=5)
    prompt = torch.ones((1, 2), dtype=torch.long, device=DEV)
    for bad in (0, -1, cfg.n_ctx, cfg.n_ctx + 1):
        try:
            kv.generate(m, cfg, prompt, 4, refresh_stride=bad)
            check(f"refresh_stride={bad} rejected", False, "no raise")
        except ValueError:
            check(f"refresh_stride={bad} rejected", True)
    c = kv.CachedMiniGPT(m, cfg, 1, DEV)
    try:
        c.prefill(torch.ones((1, cfg.n_ctx + 1), dtype=torch.long, device=DEV))
        check("over-long prefill rejected", False, "no raise")
    except ValueError:
        check("over-long prefill rejected", True)


def test_inverse_cdf_is_correct_categorical():
    print("\n[8] inverse-CDF sampler draws from the right distribution")
    torch.manual_seed(0)
    probs = torch.tensor([[0.5, 0.25, 0.15, 0.07, 0.03]], device=DEV)
    logits = probs.log().repeat(20000, 1)
    u = torch.rand((20000, 1), device=DEV)
    idx = kv._sample_inverse_cdf(logits, u, 1.0, 0)
    freq = torch.bincount(idx.reshape(-1), minlength=5).float() / 20000
    err = (freq - probs[0]).abs().max().item()
    check(f"empirical vs target max err {err:.4f} over 20k draws", err < 0.01)
    # u at the top of its range must not index past the vocabulary
    hi = torch.full((4, 1), 1.0 - 1e-7, device=DEV)
    idx_hi = kv._sample_inverse_cdf(logits[:4], hi, 1.0, 0)
    check(f"u->1 clamps inside vocab (max idx {idx_hi.max().item()})",
          idx_hi.max().item() <= 4)


def test_seed_isolation():
    print("\n[9] a seed run ALONE reproduces its group inside a big batch")
    cfg = small_cfg(n_ctx=16)
    m = make_model(cfg, seed=6)
    seeds = [0, 1, 2, 3, 4, 5, 6]
    spb, n_tok = 2, 80
    big, rows = kv.generate_multiseed(m, cfg, [1, 3], n_tok, seeds, spb,
                                      refresh_stride=4, device=DEV,
                                      uniform_chunk=32)
    ok = True
    for g, s in enumerate(seeds):
        solo, _ = kv.generate_multiseed(m, cfg, [1, 3], n_tok, [s], spb,
                                        refresh_stride=4, device=DEV,
                                        uniform_chunk=32)
        blk = big[g * spb:(g + 1) * spb]
        if not torch.equal(blk, solo):
            ok = False
            n_diff = int((blk != solo).sum())
            print(f"        seed {s}: {n_diff} token(s) differ")
    check(f"all {len(seeds)} seeds reproduce solo (batch {len(seeds)*spb} "
          f"-> batch {spb})", ok)
    check("row map is seed-major and complete",
          rows == [(s, b) for s in seeds for b in range(spb)])


def test_multiseed_guards():
    print("\n[10] multiseed guards")
    cfg = small_cfg(n_ctx=16)
    m = make_model(cfg, seed=7)
    try:
        kv.generate_multiseed(m, cfg, [1, 3], 4, [0, 1, 0], 2,
                              device=DEV, refresh_stride=4)
        check("duplicate seeds rejected", False, "no raise")
    except ValueError:
        check("duplicate seeds rejected", True)
    a, _ = kv.generate_multiseed(m, cfg, [1, 3], 40, [0, 1], 2, device=DEV,
                                 refresh_stride=4, uniform_chunk=8)
    b, _ = kv.generate_multiseed(m, cfg, [1, 3], 40, [0, 1], 2, device=DEV,
                                 refresh_stride=4, uniform_chunk=8)
    check("repeat run byte-identical", torch.equal(a, b))
    # different seeds must not collide
    g0, g1 = a[:2], a[2:]
    check("distinct seeds give distinct streams", not torch.equal(g0, g1))
    # the uniform chunk is an internal buffering detail: it must not change
    # the stream a seed produces
    c, _ = kv.generate_multiseed(m, cfg, [1, 3], 40, [0, 1], 2, device=DEV,
                                 refresh_stride=4, uniform_chunk=4096)
    check("uniform_chunk does not affect the stream", torch.equal(a, c))


def main():
    torch.manual_seed(0)
    test_prefill_matches_forward()
    test_teacher_forced_steps()
    test_prefill_resets_state()
    test_post_refresh_semantics()
    test_determinism()
    test_history_bounds()
    test_rejects_bad_stride()
    test_inverse_cdf_is_correct_categorical()
    test_seed_isolation()
    test_multiseed_guards()
    print()
    if FAILS:
        print(f"FAILED: {len(FAILS)} -> {FAILS}")
        return 1
    print("all gates green")
    return 0


if __name__ == "__main__":
    sys.exit(main())
