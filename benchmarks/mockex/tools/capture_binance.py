#!/usr/bin/env python3
"""Capture a Binance WebSocket market-data stream to JSONL for the mockex fitter.

Produces two files:

  <out>.jsonl      one JSON frame per line, ``"T":<digits>`` padded to
                   19 characters so ``PayloadPool`` can rewrite it in
                   place at runtime. This is the artifact mockex consumes.
  <out>.arrivals   one CLOCK_MONOTONIC_RAW ns per line, aligned 1:1 with
                   ``<out>.jsonl``. Read by ``fit_mmpp.py`` to recover
                   inter-arrival time statistics.

Usage::

    python3 capture_binance.py \\
        --stream btcusdt@bookTicker \\
        --duration 1800 \\
        --out fixtures/ex_market_sample

Run across different times of day (quiet / open / close) for a
representative fit — stream behaviour varies with session, so a fit
from a single hour overstates how uniform the arrival rate is.

Requires the ``websockets`` library (py3-stdlib has no WS client).
"""
from __future__ import annotations

import argparse
import asyncio
import json
import sys
import time
from pathlib import Path

try:
    import websockets
except ImportError:  # pragma: no cover
    sys.exit(
        "capture_binance.py requires the 'websockets' package: "
        "pip install websockets"
    )

# Binance combined-stream public endpoint. Single-stream wss:// path
# is /ws/<stream>; combined is /stream?streams=<list>. We use the
# single-stream form because one-stream captures are easier to fit.
BINANCE_WSS_BASE = "wss://stream.binance.com:9443/ws"

# monotonic_raw_ns values are uint64 decimal — up to ~19 digits through
# year 2286. We reserve exactly 19 digits in the payload to make the
# C++ side's in-place patch a fixed-width write.
T_FIELD_WIDTH = 19


def pad_t_field(payload: str) -> str:
    """Return ``payload`` with its first ``"T":<digits>`` run left-padded
    to exactly ``T_FIELD_WIDTH`` zero-prefixed digits.

    If the field is wider than the reservation (unlikely before year
    2286 but defensive), we leave it untouched — the C++ side will
    reject at load time with an actionable error.
    """
    key = '"T":'
    idx = payload.find(key)
    if idx < 0:
        return payload
    start = idx + len(key)
    end = start
    while end < len(payload) and payload[end].isdigit():
        end += 1
    digits = payload[start:end]
    if len(digits) > T_FIELD_WIDTH:
        return payload
    padded = digits.rjust(T_FIELD_WIDTH, "0")
    return payload[:start] + padded + payload[end:]


async def capture(stream: str, duration_s: float, out_stem: Path) -> int:
    jsonl_path = out_stem.with_suffix(".jsonl")
    arrivals_path = out_stem.with_suffix(".arrivals")
    url = f"{BINANCE_WSS_BASE}/{stream}"
    print(f"[capture] connecting to {url}", file=sys.stderr)
    deadline = time.monotonic() + duration_s
    written = 0
    with jsonl_path.open("w", encoding="utf-8") as jfh, \
            arrivals_path.open("w", encoding="utf-8") as afh:
        async with websockets.connect(url, ping_interval=20, ping_timeout=20) as ws:
            print(f"[capture] connected, writing to {jsonl_path}", file=sys.stderr)
            while time.monotonic() < deadline:
                try:
                    raw = await asyncio.wait_for(
                        ws.recv(), timeout=max(1.0, deadline - time.monotonic())
                    )
                except asyncio.TimeoutError:
                    break
                arrival_ns = time.clock_gettime_ns(time.CLOCK_MONOTONIC_RAW)
                if isinstance(raw, bytes):
                    raw = raw.decode("utf-8", errors="replace")
                padded = pad_t_field(raw.strip())
                jfh.write(padded)
                jfh.write("\n")
                afh.write(f"{arrival_ns}\n")
                written += 1
                if written % 1000 == 0:
                    print(f"[capture] wrote {written} frames", file=sys.stderr)
    print(
        f"[capture] done, {written} frames → {jsonl_path} "
        f"(+ {arrivals_path} sidecar)",
        file=sys.stderr,
    )
    return 0


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument(
        "--stream", required=True,
        help="Binance stream name, e.g. btcusdt@bookTicker or bnbusdt@depth@100ms",
    )
    p.add_argument(
        "--duration", type=float, default=1800.0,
        help="capture window in seconds (default 30 min)",
    )
    p.add_argument(
        "--out", type=Path, required=True,
        help="path stem — writes <out>.jsonl and <out>.arrivals (both overwritten)",
    )
    args = p.parse_args()
    return asyncio.run(capture(args.stream, args.duration, args.out))


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("[capture] interrupted", file=sys.stderr)
        sys.exit(130)
