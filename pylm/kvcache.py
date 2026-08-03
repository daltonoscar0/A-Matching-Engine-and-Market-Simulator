"""KV-cached incremental generation for tape's MiniGPT.

WHY THIS EXISTS
    out/tokens/sample.py generated one token by running a full forward pass
    over the entire n_ctx=320 window and discarding 319 of 320 outputs.
    Measured on this machine (M4, MPS, the 227k-param budget32k_v2 config,
    batch 8): 16.65 ms per forward, 18.96 ms per full step, ~53 tok/s/stream.
    The V1 viability bar needs ~0.5M tokens per stream, so a 7-seed TEST pass
    was ~19 hours of pure sampling.

THE SEMANTIC PROBLEM, STATED UP FRONT
    MiniGPT uses LEARNED ABSOLUTE position embeddings (pos_embed[0..n_ctx-1])
    and the old sampler slid its window by one token per step. Under a
    sliding window every token's position changes every step, so every cached
    key/value is invalidated every step: an exactly-equivalent KV cache for
    this model DOES NOT EXIST. That is a property of absolute-PE + sliding
    window, not an implementation shortcut.

    What this module does instead is CHUNKED REFRESH. The window is held
    fixed while the cache fills, then advanced by `refresh_stride` tokens and
    the cache is rebuilt by one prefill. Consequences, both of which the
    caller must know:
      - Tokens generated before the first slide (the first n_ctx-1 of them)
        are EXACT: the window has not moved, so no position has changed.
      - After that the model conditions on between (n_ctx - refresh_stride)
        and n_ctx tokens of history, where the old sampler always gave it
        exactly n_ctx. Mean history is n_ctx - refresh_stride/2.
    So sampled streams DIVERGE from the old sampler after the first slide,
    and cannot be compared byte-for-byte. Equivalence has to be argued
    distributionally; see test_kvcache.py and the RESULTS.md row.

WHAT IS VERIFIED AS EXACT
    The cache arithmetic itself. prefill() reproduces MiniGPT.forward() on
    the same window to fp tolerance, and cached step-by-step generation
    reproduces reference sliding-window generation token-for-token in the
    pre-slide regime. Those two properties are what test_kvcache.py pins.
"""

from __future__ import annotations

import math

import torch
import torch.nn.functional as F


# Each prefill creates ~112 MB of transient tensors at batch 56, and the MPS
# caching allocator does not reuse them across calls: driver-allocated memory
# grew LINEARLY at ~1.8 MB/token (1.68 GB at 500 tokens -> 13.78 GB at 7,500)
# while live-tensor memory stayed flat at 41.8 MB. That is unified memory on
# Apple Silicon, so it is invisible in RSS and gets the process SIGKILLed by
# jetsam - measured, exit 137 at 20k tokens, and it would have projected to
# ~900 GB over a 500k-token run. Emptying the cache every N refreshes bounds
# it (~1.7 GB at N=8) at no measurable throughput cost.
DEFAULT_EMPTY_CACHE_EVERY = 8


def _maybe_empty_cache(device, n_refresh: int, every: int) -> None:
    if every and n_refresh % every == 0 and str(device).startswith("mps"):
        torch.mps.empty_cache()


def _heads(t: torch.Tensor, n_heads: int) -> torch.Tensor:
    """[B, T, d] -> [B, H, T, dh]."""
    B, T, d = t.shape
    return t.view(B, T, n_heads, d // n_heads).transpose(1, 2)


def _merge(t: torch.Tensor) -> torch.Tensor:
    """[B, H, T, dh] -> [B, T, d]."""
    B, H, T, dh = t.shape
    return t.transpose(1, 2).reshape(B, T, H * dh)


class CachedMiniGPT:
    """Incremental-decoding wrapper around a tape MiniGPT.

    Re-implements the forward pass so that prefill and step share identical
    arithmetic. nn.MultiheadAttention exposes no KV cache, so q/k/v are taken
    straight from its packed in_proj weights; attention is
    scaled_dot_product_attention, which is what MultiheadAttention dispatches
    to internally, so the two agree to fp noise rather than by luck.
    """

    def __init__(self, model, cfg, batch: int, device, dtype=torch.float32):
        self.m = model
        self.cfg = cfg
        self.batch = batch
        self.device = device
        H = cfg.n_heads
        dh = cfg.d_model // H
        if H * dh != cfg.d_model:
            raise ValueError(f"d_model {cfg.d_model} not divisible by "
                             f"n_heads {H}")
        self.n_heads, self.head_dim = H, dh
        # One [B, H, n_ctx, dh] buffer per layer per tensor, allocated once.
        L = cfg.n_layers
        shape = (batch, H, cfg.n_ctx, dh)
        self.k = [torch.zeros(shape, device=device, dtype=dtype)
                  for _ in range(L)]
        self.v = [torch.zeros(shape, device=device, dtype=dtype)
                  for _ in range(L)]
        self.filled = 0  # cache slots 0..filled-1 are valid

    # -- one block, shared by both paths -------------------------------

    def _qkv(self, blk, h: torch.Tensor):
        qkv = F.linear(h, blk.attn.in_proj_weight, blk.attn.in_proj_bias)
        q, k, v = qkv.chunk(3, dim=-1)
        return (_heads(q, self.n_heads), _heads(k, self.n_heads),
                _heads(v, self.n_heads))

    def _finish_block(self, blk, x, attn_out):
        a = F.linear(_merge(attn_out), blk.attn.out_proj.weight,
                     blk.attn.out_proj.bias)
        mid = x + a
        return mid + blk.mlp(blk.ln2(mid))

    def _logits(self, x):
        return self.m.unembed(self.m.ln_final(x))

    # -- public API ----------------------------------------------------

    def prefill(self, tokens: torch.Tensor) -> torch.Tensor:
        """Seed the cache from a full window. tokens [B, T] -> logits [B, V]
        for the NEXT token. Resets the cache to exactly these T positions."""
        B, T = tokens.shape
        if T > self.cfg.n_ctx:
            raise ValueError(f"prefill length {T} > n_ctx {self.cfg.n_ctx}")
        pos = torch.arange(T, device=tokens.device)
        x = self.m.embed(tokens) + self.m.pos_embed(pos)[None]
        for l, blk in enumerate(self.m.blocks):
            h = blk.ln1(x)
            q, k, v = self._qkv(blk, h)
            self.k[l][:, :, :T] = k
            self.v[l][:, :, :T] = v
            a = F.scaled_dot_product_attention(q, k, v, is_causal=True)
            x = self._finish_block(blk, x, a)
        self.filled = T
        return self._logits(x[:, -1:, :])[:, -1, :]

    def step(self, token: torch.Tensor) -> torch.Tensor:
        """Advance one token. token [B, 1] -> logits [B, V] for the next.

        The new token takes cache slot `self.filled`; its query attends to
        slots 0..filled, all of which precede or are it, so causal masking is
        automatic and no mask is needed.
        """
        p = self.filled
        if p >= self.cfg.n_ctx:
            raise RuntimeError(
                f"cache full at n_ctx={self.cfg.n_ctx}; caller must refresh")
        pos = torch.full((1,), p, device=token.device, dtype=torch.long)
        x = self.m.embed(token) + self.m.pos_embed(pos)[None]
        end = p + 1
        for l, blk in enumerate(self.m.blocks):
            h = blk.ln1(x)
            q, k, v = self._qkv(blk, h)
            self.k[l][:, :, p:end] = k
            self.v[l][:, :, p:end] = v
            a = F.scaled_dot_product_attention(
                q, self.k[l][:, :, :end], self.v[l][:, :, :end])
            x = self._finish_block(blk, x, a)
        self.filled = end
        return self._logits(x)[:, -1, :]


def sample_from_logits(logits, temperature: float, top_k: int):
    """Same order and number of RNG draws as the original sampler: one
    multinomial per step. Kept deliberately identical so that a stream
    difference is attributable to context, not to the sampling rule."""
    logits = logits / max(temperature, 1e-6)
    if top_k > 0:
        kth = torch.topk(logits, top_k, dim=-1).values[:, -1:]
        logits = logits.masked_fill(logits < kth, float("-inf"))
    return torch.multinomial(torch.softmax(logits, dim=-1), 1)


def generate(model, cfg, prompt: torch.Tensor, n_tokens: int,
             temperature: float = 1.0, top_k: int = 0,
             refresh_stride: int = 64, progress_every: int = 0,
             progress_fn=None,
             empty_cache_every: int = DEFAULT_EMPTY_CACHE_EVERY,
             checkpoint_every: int = 0, checkpoint_fn=None
             ) -> torch.Tensor:
    """Generate n_tokens continuations of `prompt` [B, p] with a KV cache.

    refresh_stride trades fidelity against speed: the model's history window
    oscillates in [n_ctx - refresh_stride, n_ctx], and one prefill of
    (n_ctx - refresh_stride) positions is paid every refresh_stride tokens.
    stride=64 at n_ctx=320 keeps history at >= 80% of full while amortising
    the prefill to well under a millisecond per token.

    Returns [B, p + n_tokens] on the model's device.
    """
    if refresh_stride < 1 or refresh_stride >= cfg.n_ctx:
        raise ValueError(f"refresh_stride {refresh_stride} must be in "
                         f"[1, {cfg.n_ctx})")
    B, p0 = prompt.shape
    device = prompt.device
    total = p0 + n_tokens
    buf = torch.zeros((B, total), dtype=torch.long, device=device)
    buf[:, :p0] = prompt

    cache = CachedMiniGPT(model, cfg, B, device)
    w0 = 0                       # absolute index of window start
    n = p0                       # tokens currently in buf
    n_refresh = 0
    logits = cache.prefill(buf[:, w0:n])

    with torch.no_grad():
        for i in range(n_tokens):
            buf[:, n:n + 1] = sample_from_logits(logits, temperature, top_k)
            n += 1
            if checkpoint_every and checkpoint_fn and \
                    (i + 1) % checkpoint_every == 0:
                # Partial output survives a kill. sample.py wrote only at the
                # END until 2026-08-03, which is how the 100k-token run on
                # 2026-08-02 was lost in full.
                checkpoint_fn(buf, n)
            if i + 1 == n_tokens:
                break
            pos = (n - 1) - w0   # slot the new token wants
            if pos >= cfg.n_ctx:
                # Cache is full: drop the oldest `refresh_stride` tokens and
                # rebuild. This is the one place semantics depart from the
                # old sliding-window sampler (see module docstring).
                w0 = n - (cfg.n_ctx - refresh_stride)
                logits = cache.prefill(buf[:, w0:n])
                n_refresh += 1
                _maybe_empty_cache(device, n_refresh, empty_cache_every)
            else:
                logits = cache.step(buf[:, n - 1:n])
            if progress_every and (i + 1) % progress_every == 0 and progress_fn:
                progress_fn(i + 1, n_refresh)
    return buf


def _sample_inverse_cdf(logits, u, temperature: float, top_k: int):
    """Categorical sample by inverse CDF from a PRE-DRAWN uniform.

    Why not torch.multinomial: multinomial takes ONE generator per call, so
    keeping 7 genuinely distinct seeds would need 7 calls per step (~3 ms
    each at batch 8 = ~21 ms, worse than the problem being solved). Drawing
    each seed-group's uniforms from its own generator in bulk, then sampling
    them all in one op, keeps the seeds separate AND is cheaper than
    multinomial (0.36 ms vs 3.00 ms measured at batch 8).
    """
    logits = logits / max(temperature, 1e-6)
    if top_k > 0:
        kth = torch.topk(logits, top_k, dim=-1).values[:, -1:]
        logits = logits.masked_fill(logits < kth, float("-inf"))
    p = torch.softmax(logits, dim=-1)
    c = torch.cumsum(p, dim=-1)
    # fp32 cumsum can land a hair under 1.0, so a u above it would index past
    # the vocabulary. Clamp rather than renormalise: the bias is one ULP on
    # the last token, renormalising would touch every draw.
    return torch.searchsorted(c, u).clamp_(max=p.shape[-1] - 1)


class _SeedStreams:
    """Per-seed uniform supply. Group g's draws come only from generator g,
    in the same order regardless of how many groups share the batch -- which
    is what makes a single seed reproducible on its own (gate 9)."""

    def __init__(self, seeds, streams_per_seed, device, chunk=4096):
        self.gens = []
        for s in seeds:
            g = torch.Generator(device=device)
            g.manual_seed(int(s))
            self.gens.append(g)
        self.B = streams_per_seed
        self.device = device
        self.chunk = chunk
        self.buf = None
        self.pos = chunk  # force a refill on first use

    def next(self):
        if self.pos >= self.chunk:
            # Draw [chunk, B] per generator, NOT [B, chunk]. torch fills
            # row-major, so with this layout element (t, b) is always RNG
            # draw t*B + b -- successive chunks then continue the same
            # sequence a single [n_tokens, B] draw would have produced, and
            # the buffer size stops being part of the stream definition.
            # (The [B, chunk] version failed gate 10 for exactly this.)
            parts = [torch.rand((self.chunk, self.B), generator=g,
                                device=self.device) for g in self.gens]
            self.buf = torch.stack(parts, 1).reshape(self.chunk, -1)
            self.pos = 0
        u = self.buf[self.pos].unsqueeze(1)
        self.pos += 1
        return u


def generate_multiseed(model, cfg, prompt_row, n_tokens: int, seeds,
                       streams_per_seed: int, temperature: float = 1.0,
                       top_k: int = 0, refresh_stride: int = 64,
                       device="mps", uniform_chunk: int = 4096,
                       progress_every: int = 0, progress_fn=None,
                       empty_cache_every: int = DEFAULT_EMPTY_CACHE_EVERY,
                       checkpoint_every: int = 0, checkpoint_fn=None):
    """All seeds in ONE batch, each with its own RNG stream.

    Rows are seed-major: row (g * streams_per_seed + b) is stream b of
    seeds[g]. Returns (buf [S*B, 2+n_tokens], rows) where rows[i] = (seed,
    stream_index).

    The point of batching: at T=1 this model is dispatch-bound, so batch is
    nearly free (2.96 ms at B=8, 3.73 ms at B=56) while the uncached path
    scales linearly with it. Running 7 seeds together therefore costs about
    what one seed costs alone.
    """
    if refresh_stride < 1 or refresh_stride >= cfg.n_ctx:
        raise ValueError(f"refresh_stride {refresh_stride} must be in "
                         f"[1, {cfg.n_ctx})")
    if len(set(int(s) for s in seeds)) != len(seeds):
        raise ValueError(f"duplicate seeds {list(seeds)}: they would produce "
                         f"identical streams, not independent draws")
    S = len(seeds)
    B = S * streams_per_seed
    p0 = len(prompt_row)
    total = p0 + n_tokens
    buf = torch.zeros((B, total), dtype=torch.long, device=device)
    for j, tok in enumerate(prompt_row):
        buf[:, j] = tok

    unif = _SeedStreams(seeds, streams_per_seed, device, uniform_chunk)
    cache = CachedMiniGPT(model, cfg, B, device)
    w0, n, n_refresh = 0, p0, 0
    logits = cache.prefill(buf[:, w0:n])

    with torch.no_grad():
        for i in range(n_tokens):
            buf[:, n:n + 1] = _sample_inverse_cdf(
                logits, unif.next(), temperature, top_k)
            n += 1
            if checkpoint_every and checkpoint_fn and \
                    (i + 1) % checkpoint_every == 0:
                # Partial output survives a kill. sample.py wrote only at the
                # END until 2026-08-03, which is how the 100k-token run on
                # 2026-08-02 was lost in full.
                checkpoint_fn(buf, n)
            if i + 1 == n_tokens:
                break
            pos = (n - 1) - w0
            if pos >= cfg.n_ctx:
                w0 = n - (cfg.n_ctx - refresh_stride)
                logits = cache.prefill(buf[:, w0:n])
                n_refresh += 1
                _maybe_empty_cache(device, n_refresh, empty_cache_every)
            else:
                logits = cache.step(buf[:, n - 1:n])
            if progress_every and (i + 1) % progress_every == 0 and progress_fn:
                progress_fn(i + 1, n_refresh)

    rows = [(int(seeds[g]), b) for g in range(S)
            for b in range(streams_per_seed)]
    return buf, rows


def generate_reference(model, cfg, prompt: torch.Tensor, n_tokens: int,
                       temperature: float = 1.0, top_k: int = 0,
                       progress_every: int = 0,
                       progress_fn=None) -> torch.Tensor:
    """The ORIGINAL uncached sliding-window loop, kept verbatim in behaviour.

    This is not dead code: it is the reference the cached path is checked
    against, and the only way to reproduce pre-cache RESULTS rows.
    """
    B, p0 = prompt.shape
    device = prompt.device
    T = cfg.n_ctx
    total = p0 + n_tokens
    buf = torch.zeros((B, total), dtype=torch.long, device=device)
    buf[:, :p0] = prompt
    n = p0
    with torch.no_grad():
        for i in range(n_tokens):
            ctx = buf[:, max(0, n - T):n]
            logits = model(ctx)[:, -1, :]
            buf[:, n:n + 1] = sample_from_logits(logits, temperature, top_k)
            n += 1
            if progress_every and (i + 1) % progress_every == 0 and progress_fn:
                progress_fn(i + 1, 0)
    return buf
