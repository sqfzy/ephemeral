#!/usr/bin/env python3
"""Compare baseline vs postclean lat output for the rerun scenarios.

Reads pairs of bench-data-lat-{baseline,postclean}-*.txt files and prints
a side-by-side diff with the percent change for each metric. Anything > 5%
triggers a warning marker.

This is the verification step of the cleanup workflow — it answers the
question "did my refactor introduce a perf regression?". Since the cleanup
in question (commits bcd6e71..57e90b2) deliberately left every
`lat_*.cpp` source file untouched, we expect the diff to be within
measurement noise (~±2%).
"""
from __future__ import annotations

import os
import re
import sys

BENCH_RE = re.compile(r"== BENCH (.+?) ==\s*$")
METRIC_RE = re.compile(
    r"(RTT|TX|RX|SRV)\s+n=\s*(\d+)\s+min=\s*(\d+)\s+p50=\s*(\d+)\s+p99=\s*(\d+)\s+p999=\s*(\d+)\s+max=\s*(\d+)"
)


def normalize_label(raw: str) -> str:
    return re.sub(r"\s*\((kernel|dpdk)\)\s*", " ", raw).strip()


def parse(path: str) -> dict[str, dict[str, dict[str, int]]]:
    out: dict[str, dict[str, dict[str, int]]] = {}
    current = None
    if not os.path.exists(path):
        return out
    with open(path) as f:
        for line in f:
            m = BENCH_RE.search(line)
            if m:
                label = normalize_label(m.group(1))
                current = {}
                out[label] = current
                continue
            m = METRIC_RE.search(line)
            if m and current is not None:
                kind, n, mn, p50, p99, p999, mx = m.groups()
                current[kind] = {
                    "n": int(n), "min": int(mn), "p50": int(p50),
                    "p99": int(p99), "p999": int(p999), "max": int(mx),
                }
    return out


def pct(new: int, old: int) -> str:
    if old == 0:
        return "  -"
    p = (new - old) / old * 100
    sign = "+" if p >= 0 else ""
    return f"{sign}{p:.1f}%"


def fmt_ns(v: int) -> str:
    if v < 1000:
        return f"{v}"
    elif v < 100_000:
        return f"{v/1000:.1f}k"
    else:
        return f"{v/1000:.0f}k"


def compare(baseline_path: str, postclean_path: str, label: str) -> None:
    print(f"## {label}")
    print(f"  baseline:  `{os.path.basename(baseline_path)}`")
    print(f"  postclean: `{os.path.basename(postclean_path)}`")
    print()

    base = parse(baseline_path)
    post = parse(postclean_path)
    if not base:
        print(f"  ⚠ baseline file empty or unparseable")
        return
    if not post:
        print(f"  ⚠ postclean file empty or unparseable")
        return

    # Pair entries by label.
    for lbl, b_metrics in base.items():
        p_metrics = post.get(lbl)
        if p_metrics is None:
            print(f"  ⚠ missing post entry for `{lbl}`")
            continue
        print(f"### {lbl}")
        print(f"| metric | base p50 | post p50 | Δp50 | base p99 | post p99 | Δp99 | base p999 | post p999 | Δp999 |")
        print(f"| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |")
        for kind in ("RTT", "TX", "RX", "SRV"):
            if kind not in b_metrics or kind not in p_metrics:
                continue
            b = b_metrics[kind]
            p = p_metrics[kind]
            warn50  = "⚠" if abs(p["p50"]  - b["p50"])  / max(b["p50"], 1)  > 0.05 else " "
            warn99  = "⚠" if abs(p["p99"]  - b["p99"])  / max(b["p99"], 1)  > 0.05 else " "
            warn999 = "⚠" if abs(p["p999"] - b["p999"]) / max(b["p999"], 1) > 0.10 else " "
            print(f"| {kind} | {fmt_ns(b['p50'])} | {fmt_ns(p['p50'])} | {pct(p['p50'], b['p50'])}{warn50} "
                  f"| {fmt_ns(b['p99'])} | {fmt_ns(p['p99'])} | {pct(p['p99'], b['p99'])}{warn99} "
                  f"| {fmt_ns(b['p999'])} | {fmt_ns(p['p999'])} | {pct(p['p999'], b['p999'])}{warn999} |")
        print()


if __name__ == "__main__":
    pairs = [
        (".artifacts/bench-data-lat-baseline-tcp-dpdk-20260409.txt",
         ".artifacts/bench-data-lat-postclean-tcp-dpdk-20260410-postclean.txt",
         "tcp --dpdk"),
        (".artifacts/bench-data-lat-baseline-tcp-kernel-20260409.txt",
         ".artifacts/bench-data-lat-postclean-tcp-kernel-20260410-postclean.txt",
         "tcp (kernel)"),
        (".artifacts/bench-data-lat-baseline-ex_market-dpdk-20260409.txt",
         ".artifacts/bench-data-lat-postclean-ex_market-dpdk-20260410-postclean.txt",
         "ex_market --dpdk"),
    ]
    print("# Cleanup verification — baseline vs postclean")
    print()
    print("Threshold: ⚠ on |Δp50| > 5%, |Δp99| > 5%, |Δp999| > 10%.")
    print()
    for b, p, lbl in pairs:
        compare(b, p, lbl)
        print()
