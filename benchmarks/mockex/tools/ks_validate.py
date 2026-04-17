#!/usr/bin/env python3
"""K-S + autocorrelation comparison between a real-binance capture and
a mockex-driven capture. The assertion is "the two distributions look
the same" — if the mockex parameters are well-fit, the K-S p-value
should stay above 0.05 and the lag-1 autocorrelation difference
below 0.10.

Both inputs must be ``<stem>.arrivals`` files (one
CLOCK_MONOTONIC_RAW ns timestamp per line, produced by
``capture_binance.py``). You can generate the mockex capture by
running mockex into ``tee`` with a simple ``awk`` to strip the
envelope, or by repurposing ``capture_binance.py`` with a
``ws://127.0.0.1`` endpoint against mockex.

This is stdlib-only — no numpy/scipy. The K-S critical value and
approximate p-value are computed via the standard Smirnov formula,
which is precise enough for N > 1000 (the only regime worth
comparing anyway).
"""
from __future__ import annotations

import argparse
import math
import sys
from pathlib import Path


def load_intervals_ns(arrivals_path: Path) -> list[float]:
    """Return inter-arrival times (ns) from a monotonic-ns file."""
    raw = [int(line) for line in arrivals_path.read_text().splitlines()
           if line.strip()]
    deltas = []
    for a, b in zip(raw, raw[1:]):
        d = b - a
        if d > 0:
            deltas.append(float(d))
    return deltas


def ks_two_sample(a: list[float], b: list[float]) -> tuple[float, float]:
    """Return (D, p_value) for the two-sample Kolmogorov-Smirnov test.

    Implementation uses the pooled-sorted two-pointer scan, which is
    O((n + m) log(n + m)) for the sort and then linear. The p-value
    uses the Smirnov approximation
    ``p ≈ 2·Σ_k=1..∞ (-1)^(k-1) · exp(-2 k² λ²)`` with
    ``λ = (√n + √m + 0.12 + 0.11/√n + 0.11/√m) · D`` — the standard
    Massey (1951) expression, accurate to three decimals for reasonable N.
    """
    if not a or not b:
        raise ValueError("ks_two_sample: empty sample")
    sa = sorted(a)
    sb = sorted(b)
    n, m = len(sa), len(sb)
    i = j = 0
    cdfa = cdfb = 0.0
    d = 0.0
    while i < n and j < m:
        if sa[i] <= sb[j]:
            i += 1
            cdfa = i / n
        else:
            j += 1
            cdfb = j / m
        d = max(d, abs(cdfa - cdfb))
    # Drain whichever side still has samples.
    if i < n: cdfa = 1.0
    if j < m: cdfb = 1.0
    d = max(d, abs(cdfa - cdfb))

    sqrt_nm = math.sqrt(n * m / (n + m))
    lam = (sqrt_nm + 0.12 + 0.11 / sqrt_nm) * d
    if lam <= 0.0:
        return d, 1.0
    # Infinite alternating series; four terms is plenty for lam > 0.3.
    p = 0.0
    for k in range(1, 40):
        term = (-1.0) ** (k - 1) * math.exp(-2.0 * k * k * lam * lam)
        p += term
        if abs(term) < 1e-10:
            break
    p = max(0.0, min(1.0, 2.0 * p))
    return d, p


def acf_lag1(x: list[float]) -> float:
    """Lag-1 sample autocorrelation — captures the "burstiness" of the
    series. Pearson formula on (x_t, x_{t+1}).
    """
    n = len(x)
    if n < 3:
        return 0.0
    mean = sum(x) / n
    num = den = 0.0
    for a, b in zip(x, x[1:]):
        num += (a - mean) * (b - mean)
    for v in x:
        den += (v - mean) ** 2
    return num / den if den > 0 else 0.0


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--real", type=Path, required=True,
                   help="path to real capture <stem>.arrivals")
    p.add_argument("--mock", type=Path, required=True,
                   help="path to mockex capture <stem>.arrivals")
    p.add_argument("--p-threshold", type=float, default=0.05,
                   help="fail when K-S p-value < this (default 0.05)")
    p.add_argument("--acf-threshold", type=float, default=0.10,
                   help="fail when |ACF_real − ACF_mock| > this "
                        "(default 0.10)")
    args = p.parse_args()

    real = load_intervals_ns(args.real)
    mock = load_intervals_ns(args.mock)
    if len(real) < 100 or len(mock) < 100:
        print(f"[ks_validate] ERROR: too few samples "
              f"(real={len(real)}, mock={len(mock)}; need ≥ 100 each)",
              file=sys.stderr)
        return 2

    d, p_value = ks_two_sample(real, mock)
    acf_r = acf_lag1(real)
    acf_m = acf_lag1(mock)
    acf_diff = abs(acf_r - acf_m)

    real_mean = sum(real) / len(real)
    mock_mean = sum(mock) / len(mock)

    print("=== ks_validate ===")
    print(f"samples     real={len(real):>7} mock={len(mock):>7}")
    print(f"mean Δt(ns) real={real_mean:>10.1f} mock={mock_mean:>10.1f}")
    print(f"K-S         D={d:.4f} p_value={p_value:.4f} "
          f"(threshold {args.p_threshold})")
    print(f"ACF lag-1   real={acf_r:+.3f} mock={acf_m:+.3f} "
          f"diff={acf_diff:.3f} (threshold {args.acf_threshold})")

    ok = (p_value >= args.p_threshold) and (acf_diff <= args.acf_threshold)
    if ok:
        print("verdict: PASS — mockex distribution matches real capture")
        return 0
    print("verdict: FAIL — refit mockex_params or recapture with a longer "
          "window")
    return 1


if __name__ == "__main__":
    sys.exit(main())
