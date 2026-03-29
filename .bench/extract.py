#!/usr/bin/env python3
"""Extract benchmark metrics from 6 rounds of socket vs DPDK tests."""
import re, json, math, sys
from pathlib import Path
from collections import defaultdict

ROUNDS_DIR = Path("/home/ec2-user/ephemeral_dev/.bench/rounds")

def extract_ns_metrics(text, section_label):
    """Extract min/p50/p99/p999/max from a labeled section (ns format)."""
    # Find the section
    idx = text.find(section_label)
    if idx < 0:
        return None
    block = text[idx:idx+600]

    result = {}
    for key in ["min", "p50", "p99", "p99.9", "max", "samples"]:
        pattern = rf"{key}:\s+(\d+)"
        m = re.search(pattern, block)
        if m:
            result[key] = int(m.group(1))
    return result if result else None

def extract_handshake(text):
    """Extract handshake breakdown from transport stats."""
    m = re.search(r"handshake: ([\d.]+)ms \(tcp: ([\d.]+)ms, tls: ([\d.]+)ms, ws: ([\d.]+)ms\)", text)
    if m:
        return {
            "total": float(m.group(1)),
            "tcp": float(m.group(2)),
            "tls": float(m.group(3)),
            "ws": float(m.group(4)),
        }
    return None

def extract_rx_breakdown(text):
    """Extract RX latency breakdown (decrypt, decode) from transport stats (us format)."""
    result = {}
    for label, key in [("decrypt:", "decrypt"), ("decode:", "decode")]:
        m = re.search(rf"{label}\s+RttStats \(\d+ samples\):\s*\n\s*min: ([\d.]+)us, p50: ([\d.]+)us, p99: ([\d.]+)us", text)
        if m:
            result[key] = {"min": float(m.group(1)), "p50": float(m.group(2)), "p99": float(m.group(3))}
    return result if result else None

def extract_messages(text):
    """Extract message count."""
    m = re.search(r"Messages: (\d+)", text)
    return int(m.group(1)) if m else None

def stats(values):
    """Compute mean, std, min, max."""
    n = len(values)
    if n == 0:
        return {"mean": 0, "std": 0, "min": 0, "max": 0, "n": 0}
    mean = sum(values) / n
    var = sum((x - mean) ** 2 for x in values) / n if n > 1 else 0
    return {"mean": mean, "std": math.sqrt(var), "min": min(values), "max": max(values), "n": n}

# Collect all data
data = defaultdict(lambda: defaultdict(list))

for r in range(1, 7):
    # Pingpong
    for backend in ["socket", "dpdk"]:
        f = ROUNDS_DIR / f"r{r}_pingpong_{backend}.txt"
        if not f.exists():
            continue
        text = f.read_text()

        tx = extract_ns_metrics(text, "Metric 1: TX Queue")
        rtt = extract_ns_metrics(text, "Metric 3: RTT")
        hs = extract_handshake(text)

        if tx:
            for k in ["min", "p50", "p99", "p99.9", "max"]:
                if k in tx:
                    data[f"pingpong_{backend}_tx_{k}"]["values"].append(tx[k])
        if rtt:
            for k in ["min", "p50", "p99", "p99.9", "max"]:
                if k in rtt:
                    data[f"pingpong_{backend}_rtt_{k}"]["values"].append(rtt[k])
        if hs:
            for k in ["total", "tcp", "tls", "ws"]:
                data[f"pingpong_{backend}_hs_{k}"]["values"].append(hs[k])

    # Market (single symbol)
    for backend in ["socket", "dpdk"]:
        f = ROUNDS_DIR / f"r{r}_market_{backend}.txt"
        if not f.exists():
            continue
        text = f.read_text()

        rx = extract_ns_metrics(text, "RX Pipeline")
        msgs = extract_messages(text)
        rxb = extract_rx_breakdown(text)

        if rx:
            for k in ["min", "p50", "p99", "p99.9", "max", "samples"]:
                if k in rx:
                    data[f"market_{backend}_rx_{k}"]["values"].append(rx[k])
        if msgs:
            data[f"market_{backend}_msgs"]["values"].append(msgs)
        if rxb:
            for stage, vals in rxb.items():
                for k, v in vals.items():
                    data[f"market_{backend}_{stage}_{k}"]["values"].append(v)

    # Multi-symbol
    for backend in ["socket", "dpdk"]:
        f = ROUNDS_DIR / f"r{r}_multi_{backend}.txt"
        if not f.exists():
            continue
        text = f.read_text()

        rx = extract_ns_metrics(text, "RX Pipeline")
        feed = extract_ns_metrics(text, "Feed Latency")
        msgs = extract_messages(text)
        rxb = extract_rx_breakdown(text)

        if rx:
            for k in ["min", "p50", "p99", "p99.9", "max", "samples"]:
                if k in rx:
                    data[f"multi_{backend}_rx_{k}"]["values"].append(rx[k])
        if feed:
            for k in ["min", "p50", "p99", "p99.9", "max"]:
                if k in feed:
                    data[f"multi_{backend}_feed_{k}"]["values"].append(feed[k])
        if msgs:
            data[f"multi_{backend}_msgs"]["values"].append(msgs)
        if rxb:
            for stage, vals in rxb.items():
                for k, v in vals.items():
                    data[f"multi_{backend}_{stage}_{k}"]["values"].append(v)

# Compute statistics and output
output = {}
for key, d in data.items():
    output[key] = stats(d["values"])

# Print as JSON
json.dump(output, sys.stdout, indent=2)
