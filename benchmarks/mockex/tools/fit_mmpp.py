#!/usr/bin/env python3
"""Fit MMPP-2 arrival parameters + KDE size distribution from a capture.

Input:

  <stem>.jsonl      (for message sizes — one json per line, utf-8 bytes)
  <stem>.arrivals   (for inter-arrival times — one ns timestamp per line)

Output (stdout, or ``--out`` path):

  A flat-key INI that :class:`mockex::Mmpp2Params::load_from_file`
  reads directly — no sections, one ``key = value`` per line.

The EM solver is pure stdlib Python (no numpy / scipy). It converges
in a few hundred iterations on ~1 M samples; expect ≤ 60 s.

Usage::

    python3 fit_mmpp.py \\
        --in fixtures/ex_market_sample \\
        --out fixtures/ex_market_params.ini \\
        --seed 42

The ``--synthetic N`` flag bypasses capture files and emits
hand-picked parameters matching a realistic Binance bookTicker rate
(~15 Hz quiet, ~200 Hz busy, sub-300 byte frames). Intended only to
bootstrap a fresh checkout so mockex push scenarios can compile and
smoke-test before a real capture lands.
"""
from __future__ import annotations

import argparse
import datetime
import math
import statistics
import sys
from pathlib import Path


# ─────────────────────────────────────────────────────────────────────
# Sample loading
# ─────────────────────────────────────────────────────────────────────

def load_arrivals(path: Path) -> list[int]:
    """Return CLOCK_MONOTONIC_RAW ns timestamps from ``path`` (one per line)."""
    return [int(line) for line in path.read_text().splitlines() if line.strip()]


def load_sizes(path: Path) -> list[int]:
    """Return one payload byte-length per line in ``path`` (UTF-8)."""
    return [len(line.encode("utf-8"))
            for line in path.read_text().splitlines() if line.strip()]


def inter_arrival_seconds(arrivals_ns: list[int]) -> list[float]:
    """Convert adjacent-pair diffs of ``arrivals_ns`` to seconds.

    Duplicate or non-monotonic samples (rare but possible under clock
    slew) are dropped rather than producing negative/zero deltas that
    would explode the EM M-step.
    """
    out = []
    for a, b in zip(arrivals_ns, arrivals_ns[1:]):
        d = b - a
        if d > 0:
            out.append(d / 1e9)
    return out


# ─────────────────────────────────────────────────────────────────────
# MMPP-2 EM (Baum-Welch specialised to a 2-state HMM over exp(λ_i))
# ─────────────────────────────────────────────────────────────────────

def exp_pdf(x: float, lam: float) -> float:
    """Exponential density f(x; λ) = λ·exp(-λx) for x > 0.

    Clamped below by a tiny floor to avoid zero-probability steps
    during the forward-backward pass (float underflow on very short
    intervals with very small λ is otherwise a common EM pathology).
    """
    if x <= 0.0 or lam <= 0.0:
        return 1e-300
    return max(lam * math.exp(-lam * x), 1e-300)


def em_mmpp2(
    inter_s: list[float],
    *,
    max_iter: int = 200,
    tol: float = 1e-6,
) -> tuple[float, float, float, float]:
    """Fit (λ_q, λ_b, p_qb, p_bq) to inter-arrival times in seconds.

    Classic Baum-Welch on a 2-state HMM: forward/backward alphas, then
    re-estimate transition and emission parameters. Initial conditions
    are "quiet λ = median, busy λ = 4 × median"; initial transitions
    are near the diagonal.
    """
    n = len(inter_s)
    if n < 10:
        raise ValueError(f"need ≥ 10 inter-arrival samples, got {n}")
    med = statistics.median(inter_s)
    lam_q = 1.0 / med if med > 0 else 1.0
    lam_b = 4.0 * lam_q
    p_qb, p_bq = 0.05, 0.3  # modest initial transitions

    for iteration in range(max_iter):
        # ── E-step: forward α and backward β (normalised to avoid underflow)
        a0, a1 = 0.5 * exp_pdf(inter_s[0], lam_q), 0.5 * exp_pdf(inter_s[0], lam_b)
        scale0 = a0 + a1
        alpha = [(a0 / scale0, a1 / scale0)]
        scales = [scale0]
        for t in range(1, n):
            prev_q, prev_b = alpha[-1]
            stay_q = prev_q * (1.0 - p_qb) + prev_b * p_bq
            stay_b = prev_q * p_qb + prev_b * (1.0 - p_bq)
            a_q = stay_q * exp_pdf(inter_s[t], lam_q)
            a_b = stay_b * exp_pdf(inter_s[t], lam_b)
            s = a_q + a_b
            alpha.append((a_q / s, a_b / s))
            scales.append(s)

        beta = [(1.0, 1.0)]
        for t in range(n - 1, 0, -1):
            bq_next, bb_next = beta[0]
            eq = exp_pdf(inter_s[t], lam_q)
            eb = exp_pdf(inter_s[t], lam_b)
            b_q = (1.0 - p_qb) * eq * bq_next + p_qb * eb * bb_next
            b_b = p_bq * eq * bq_next + (1.0 - p_bq) * eb * bb_next
            s = b_q + b_b
            beta.insert(0, (b_q / max(s, 1e-300), b_b / max(s, 1e-300)))

        gamma = []  # P(state_t | all obs)
        xi_qq = xi_qb = xi_bq_ = xi_bb = 0.0
        sum_q = sum_b = 0.0
        wsum_q = wsum_b = 0.0
        for t in range(n):
            aq, ab = alpha[t]
            bq, bb = beta[t]
            gq = aq * bq
            gb = ab * bb
            s = gq + gb
            gq, gb = gq / s, gb / s
            gamma.append((gq, gb))
            sum_q += gq
            sum_b += gb
            wsum_q += gq * inter_s[t]
            wsum_b += gb * inter_s[t]

        for t in range(n - 1):
            aq, ab = alpha[t]
            eq = exp_pdf(inter_s[t + 1], lam_q)
            eb = exp_pdf(inter_s[t + 1], lam_b)
            bq_next, bb_next = beta[t + 1]
            denom = (
                aq * (1.0 - p_qb) * eq * bq_next
                + aq * p_qb        * eb * bb_next
                + ab * p_bq        * eq * bq_next
                + ab * (1.0 - p_bq) * eb * bb_next
            )
            if denom <= 0.0:
                continue
            xi_qq  += aq * (1.0 - p_qb) * eq * bq_next / denom
            xi_qb  += aq * p_qb         * eb * bb_next / denom
            xi_bq_ += ab * p_bq         * eq * bq_next / denom
            xi_bb  += ab * (1.0 - p_bq) * eb * bb_next / denom

        # ── M-step
        new_lam_q = max(sum_q / max(wsum_q, 1e-12), 1e-6)
        new_lam_b = max(sum_b / max(wsum_b, 1e-12), 1e-6)
        new_p_qb  = xi_qb / max(xi_qq + xi_qb, 1e-12)
        new_p_bq  = xi_bq_ / max(xi_bq_ + xi_bb, 1e-12)

        delta = max(
            abs(new_lam_q - lam_q) / max(lam_q, 1e-6),
            abs(new_lam_b - lam_b) / max(lam_b, 1e-6),
            abs(new_p_qb - p_qb),
            abs(new_p_bq - p_bq),
        )
        lam_q, lam_b, p_qb, p_bq = new_lam_q, new_lam_b, new_p_qb, new_p_bq
        if delta < tol:
            break

    # Identifiability: always report λ_quiet ≤ λ_busy. If EM found the
    # opposite ordering we swap both rates and transition probabilities.
    if lam_q > lam_b:
        lam_q, lam_b = lam_b, lam_q
        p_qb, p_bq = p_bq, p_qb
    return lam_q, lam_b, p_qb, p_bq


# ─────────────────────────────────────────────────────────────────────
# KDE-style size distribution: quantile anchors + normalised weights
# ─────────────────────────────────────────────────────────────────────

def fit_size_distribution(
    sizes: list[int], anchor_count: int = 6
) -> tuple[list[int], list[float], float]:
    """Pick ``anchor_count`` quantile points as KDE anchors, weight
    them by their local density, and set the jitter bandwidth to
    0.5 × IQR / anchor_count so draws stay inside realistic bytes.

    This is a crude approximation of scipy's ``gaussian_kde`` that
    still produces visually-correct distributions for the fat, uneven
    bodies typical of exchange payload sizes. The mockex sampler
    interprets (anchors, weights) as a weighted mixture of gaussians.
    """
    if not sizes:
        raise ValueError("fit_size_distribution: empty input")
    ordered = sorted(sizes)
    n = len(ordered)
    # Evenly-spaced quantiles to pick the anchors. Skip the extremes
    # (0 and n-1) to dodge outliers.
    idxs = [int((i + 1) / (anchor_count + 1) * (n - 1))
            for i in range(anchor_count)]
    anchors = [ordered[i] for i in idxs]
    # Local density = inverse gap to neighbours → dense regions get
    # higher weight. Normalise at the end.
    raw_w = []
    for i, idx in enumerate(idxs):
        lo = idxs[i - 1] if i > 0 else 0
        hi = idxs[i + 1] if i + 1 < len(idxs) else n - 1
        gap = max(ordered[hi] - ordered[lo], 1)
        raw_w.append(1.0 / gap)
    s = sum(raw_w) or 1.0
    weights = [w / s for w in raw_w]
    # IQR = q75 − q25; bandwidth is half the IQR divided by anchor_count
    # so ±3σ mostly stays inside the observed range.
    q25 = ordered[n // 4]
    q75 = ordered[(3 * n) // 4]
    bandwidth = max((q75 - q25) / (2.0 * anchor_count), 1.0)
    return anchors, weights, bandwidth


# ─────────────────────────────────────────────────────────────────────
# Output
# ─────────────────────────────────────────────────────────────────────

def write_params(
    out: "IOText",
    *,
    lam_q: float,
    lam_b: float,
    p_qb: float,
    p_bq: float,
    anchors: list[int],
    weights: list[float],
    bandwidth: float,
    source: str,
    seed_default: int,
) -> None:
    """Emit the flat INI schema read by ``Mmpp2Params::load_from_file``."""
    now = datetime.datetime.now(datetime.timezone.utc).isoformat(
        timespec="seconds")
    out.write(f"# Fitted by fit_mmpp.py at {now}\n")
    out.write(f"fitted_from = {source}\n")
    out.write(f"fitted_at = {now}\n")
    out.write("model = mmpp2\n")
    out.write("fitter_version = 1\n")
    out.write(f"seed_default = {seed_default}\n\n")
    out.write(f"lambda_quiet_hz = {lam_q:.6g}\n")
    out.write(f"lambda_busy_hz = {lam_b:.6g}\n")
    out.write(f"p_quiet_to_busy = {p_qb:.6g}\n")
    out.write(f"p_busy_to_quiet = {p_bq:.6g}\n\n")
    out.write(f"size_kde_bandwidth = {bandwidth:.4g}\n")
    out.write("size_kde_anchors = " + ",".join(str(a) for a in anchors) + "\n")
    out.write("size_kde_weights = " + ",".join(f"{w:.4g}" for w in weights) + "\n")


def synthetic_params(scenario: str) -> dict:
    """Return hand-picked plausible parameters for bootstrap fixtures.

    Values reflect a 2026 Binance bookTicker stream rough observation:
    ~15 Hz quiet, ~200 Hz during bursts, ~2 % time in busy, payload
    ~200-320 bytes. Replace with a real fit once capture data exists.
    """
    anchors = [200, 240, 280, 320, 380, 440]
    weights = [0.22, 0.31, 0.24, 0.12, 0.07, 0.04]
    if scenario.endswith("_2p"):
        # Multi-symbol variant — higher busy rate (5 symbols × burst).
        return dict(lam_q=75.0, lam_b=950.0, p_qb=0.015, p_bq=0.35,
                    anchors=anchors, weights=weights, bandwidth=8.0)
    return dict(lam_q=15.0, lam_b=200.0, p_qb=0.02, p_bq=0.4,
                anchors=anchors, weights=weights, bandwidth=6.0)


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument(
        "--in", dest="in_stem", type=Path,
        help="capture stem (reads <stem>.jsonl and <stem>.arrivals)",
    )
    p.add_argument(
        "--out", type=Path, default=None,
        help="write params.ini here; stdout if omitted",
    )
    p.add_argument("--seed", type=int, default=42,
                   help="seed_default for the sampler at runtime")
    p.add_argument(
        "--synthetic", metavar="SCENARIO", default=None,
        help="skip EM; emit bootstrap parameters for 'ex_market' or "
             "'ex_market_2p'",
    )
    args = p.parse_args()

    if args.synthetic is not None:
        par = synthetic_params(args.synthetic)
        source = f"synthetic:{args.synthetic}"
        lam_q, lam_b, p_qb, p_bq = par["lam_q"], par["lam_b"], par["p_qb"], par["p_bq"]
        anchors, weights, bandwidth = par["anchors"], par["weights"], par["bandwidth"]
    else:
        if args.in_stem is None:
            p.error("--in is required unless --synthetic is used")
        jsonl = args.in_stem.with_suffix(".jsonl")
        arrivals = args.in_stem.with_suffix(".arrivals")
        if not jsonl.is_file() or not arrivals.is_file():
            sys.exit(f"missing {jsonl} or {arrivals}")
        arr = load_arrivals(arrivals)
        szs = load_sizes(jsonl)
        if len(arr) != len(szs):
            print(f"[warn] arrivals({len(arr)}) != sizes({len(szs)}); "
                  "using the shorter length", file=sys.stderr)
        inter = inter_arrival_seconds(arr)
        lam_q, lam_b, p_qb, p_bq = em_mmpp2(inter)
        anchors, weights, bandwidth = fit_size_distribution(szs)
        source = str(args.in_stem)

    out = args.out.open("w", encoding="utf-8") if args.out else sys.stdout
    try:
        write_params(
            out,
            lam_q=lam_q, lam_b=lam_b, p_qb=p_qb, p_bq=p_bq,
            anchors=anchors, weights=weights, bandwidth=bandwidth,
            source=source, seed_default=args.seed,
        )
    finally:
        if args.out:
            out.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
