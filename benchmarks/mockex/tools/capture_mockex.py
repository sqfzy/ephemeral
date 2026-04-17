#!/usr/bin/env python3
"""Capture `<stem>.jsonl` + `<stem>.arrivals` from a running mockex server.

Symmetric to ``capture_binance.py`` but pointed at a local
``ws://mock_ip:port`` endpoint instead of ``wss://stream.binance.com``.
Produces the exact same file layout so ``fit_mmpp.py`` and
``ks_validate.py`` work on the output without changes.

Usage::

    python3 capture_mockex.py \\
        --host 127.0.0.1 --port 20003 \\
        --duration 60 \\
        --out /tmp/mock_ex_market

Pair with ``ks_validate.py`` to assert the mockex output matches a real
Binance capture for the same scenario::

    python3 ks_validate.py \\
        --real real_btc_trade.arrivals \\
        --mock /tmp/mock_ex_market.arrivals
"""
from __future__ import annotations

import argparse
import asyncio
import sys
import time
from pathlib import Path

try:
    import websockets
except ImportError:  # pragma: no cover
    sys.exit("capture_mockex.py requires 'websockets': pip install websockets")


async def capture(host: str, port: int, duration_s: float,
                  out_stem: Path) -> int:
    url = f"ws://{host}:{port}/ws/btcusdt@trade"
    print(f"[capture_mockex] connecting to {url}", file=sys.stderr)
    jsonl_path = out_stem.with_suffix(".jsonl")
    arrivals_path = out_stem.with_suffix(".arrivals")
    deadline = time.monotonic() + duration_s
    written = 0
    with jsonl_path.open("w", encoding="utf-8") as jfh, \
            arrivals_path.open("w", encoding="utf-8") as afh:
        async with websockets.connect(url) as ws:
            while time.monotonic() < deadline:
                try:
                    raw = await asyncio.wait_for(
                        ws.recv(), timeout=max(1.0, deadline - time.monotonic()))
                except asyncio.TimeoutError:
                    break
                arrival_ns = time.clock_gettime_ns(time.CLOCK_MONOTONIC_RAW)
                if isinstance(raw, bytes):
                    raw = raw.decode("utf-8", errors="replace")
                jfh.write(raw.strip() + "\n")
                afh.write(f"{arrival_ns}\n")
                written += 1
    print(f"[capture_mockex] wrote {written} frames to {jsonl_path}",
          file=sys.stderr)
    return 0


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, required=True,
                   help="mockex listen port (matches [lat_*].port)")
    p.add_argument("--duration", type=float, default=60.0)
    p.add_argument("--out", type=Path, required=True,
                   help="path stem — writes <out>.jsonl + <out>.arrivals")
    args = p.parse_args()
    return asyncio.run(capture(args.host, args.port, args.duration, args.out))


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
