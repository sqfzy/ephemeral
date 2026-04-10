#!/usr/bin/env python3
"""Parse lat baseline files into a markdown summary table.

Each baseline file contains spdlog output with this shape:

  [timestamp] [info] == BENCH <name> ==
  [timestamp] [info]     RTT n=N min=X p50=Y p99=Z p999=W max=M
  [timestamp] [info]     TX  ...
  [timestamp] [info]     RX  ...
  [timestamp] [info]     SRV ...

The "BENCH" header line varies by scenario:
  - tcp/udp/ws:    "== BENCH tcp (dpdk) payload=1024B =="
  - ex_market:     "== BENCH exchange/market (dpdk) (oneway) =="
  - ex_order:      "== BENCH exchange/order (dpdk) inflight=16 =="
  - ex_md_udp:     "== BENCH exchange/md_udp (dpdk) payload=1024B =="
"""

from __future__ import annotations

import glob
import re
import sys
from dataclasses import dataclass, field

BENCH_RE = re.compile(r"== BENCH (.+?) ==\s*$")
METRIC_RE = re.compile(
    r"(RTT|TX|RX|SRV)\s+n=\s*(\d+)\s+min=\s*(\d+)\s+p50=\s*(\d+)\s+p99=\s*(\d+)\s+p999=\s*(\d+)\s+max=\s*(\d+)"
)


@dataclass
class Bench:
    label: str
    metrics: dict[str, dict[str, int]] = field(default_factory=dict)


def normalize_label(raw: str) -> str:
    """Strip the (kernel)/(dpdk) tag from a BENCH header so kernel and dpdk
    runs of the same scenario+payload get the same key."""
    return re.sub(r"\s*\((kernel|dpdk)\)\s*", " ", raw).strip()


def parse(path: str) -> list[Bench]:
    benches: list[Bench] = []
    current: Bench | None = None
    with open(path) as f:
        for line in f:
            m = BENCH_RE.search(line)
            if m:
                current = Bench(label=normalize_label(m.group(1)))
                benches.append(current)
                continue
            m = METRIC_RE.search(line)
            if m and current is not None:
                kind, n, mn, p50, p99, p999, mx = m.groups()
                current.metrics[kind] = {
                    "n": int(n), "min": int(mn), "p50": int(p50),
                    "p99": int(p99), "p999": int(p999), "max": int(mx),
                }
    return benches


def fmt_ns(v: int) -> str:
    """ns -> human-readable (ns or us)."""
    if v < 1000:
        return f"{v}"
    elif v < 100_000:
        return f"{v/1000:.1f}k"
    else:
        return f"{v/1000:.0f}k"


def main() -> int:
    files = sorted(glob.glob(".artifacts/bench-data-lat-baseline-*-20260409.txt"))
    print("# Latency benchmark baseline — 2026-04-09")
    print()
    print("Captured at commit `c88e1dc` on EC2 ARM64 (ens34=kernel, ens35=vfio-pci).")
    print("Each scenario was run via `sudo ./benchmarks/latency/lat <scenario> [--dpdk]`.")
    print("All values in **nanoseconds**. n = sample count.")
    print()
    print("## Files")
    print()
    print("| File | Size |")
    print("| --- | --- |")
    for f in files:
        import os
        print(f"| `{os.path.basename(f)}` | {os.path.getsize(f)} bytes |")
    print()

    # Group by scenario, mode
    by_scenario: dict[tuple[str, str], list[Bench]] = {}
    for f in files:
        # bench-data-lat-baseline-<scenario>-<mode>-20260409.txt
        import os
        base = os.path.basename(f).removeprefix("bench-data-lat-baseline-").removesuffix("-20260409.txt")
        # split on last "-"
        parts = base.rsplit("-", 1)
        scenario, mode = parts[0], parts[1]
        by_scenario[(scenario, mode)] = parse(f)

    # Output per scenario, kernel/dpdk side-by-side
    scenarios = sorted({s for s, _ in by_scenario.keys()})
    for sc in scenarios:
        print(f"## `{sc}`")
        print()

        kernel = by_scenario.get((sc, "kernel"), [])
        dpdk = by_scenario.get((sc, "dpdk"), [])

        # Use the union of labels (each scenario has same labels in both modes)
        labels = []
        seen = set()
        for b in kernel + dpdk:
            if b.label not in seen:
                labels.append(b.label)
                seen.add(b.label)

        for label in labels:
            kb = next((b for b in kernel if b.label == label), None)
            db = next((b for b in dpdk if b.label == label), None)

            print(f"### {label}")
            print()
            print("| metric | mode | n | min | p50 | p99 | p999 | max |")
            print("| --- | --- | --- | --- | --- | --- | --- | --- |")
            for kind in ("RTT", "TX", "RX", "SRV"):
                for tag, b in (("kernel", kb), ("dpdk", db)):
                    if b is None or kind not in b.metrics:
                        continue
                    m = b.metrics[kind]
                    print(f"| {kind} | {tag} | {m['n']:,} | {fmt_ns(m['min'])} | "
                          f"{fmt_ns(m['p50'])} | {fmt_ns(m['p99'])} | "
                          f"{fmt_ns(m['p999'])} | {fmt_ns(m['max'])} |")
            print()

    return 0


if __name__ == "__main__":
    sys.exit(main())
