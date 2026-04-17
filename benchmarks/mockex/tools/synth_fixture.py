#!/usr/bin/env python3
"""Generate a synthetic `<scenario>_sample.jsonl` so Phase 3 scenarios
can compile + smoke-test before a real binance capture exists.

The bytes emitted are shape-compatible with the real stream
(key order, numeric formatting, closing brace) so the bench client's
JSON parser does not choke, but the price/qty values are fabricated.
Once ``capture_binance.py`` has run for real the synthetic fixture
should be *replaced*, not merged — the file header makes the origin
obvious.
"""
from __future__ import annotations

import argparse
import json
import random
from pathlib import Path

BINANCE_SYMBOLS_DEFAULT = (
    "BTCUSDT", "ETHUSDT", "SOLUSDT", "BNBUSDT", "XRPUSDT"
)


def synth_bookticker(symbol: str, rng: random.Random) -> str:
    """Shape a single bookTicker-style JSON object. The numeric
    formatting mirrors Binance's exchange output (whole-number prices
    for high-price symbols, 8 decimal places for qty).
    """
    px = {
        "BTCUSDT": 50000,
        "ETHUSDT": 3000,
        "SOLUSDT": 140,
        "BNBUSDT": 550,
        "XRPUSDT": 2,
    }.get(symbol, 100)
    bid = px + rng.uniform(-5, 5)
    ask = bid + rng.uniform(0.01, 0.10)
    bid_q = rng.uniform(0.1, 10.0)
    ask_q = rng.uniform(0.1, 10.0)
    obj = {
        "e": "bookTicker",
        "u": rng.randint(1_000_000_000, 2_000_000_000),
        "s": symbol,
        "b": f"{bid:.2f}",
        "B": f"{bid_q:.8f}",
        "a": f"{ask:.2f}",
        "A": f"{ask_q:.8f}",
        # "T" reserved with 19 digits so PayloadPool's in-place patch
        # (fixed-width write) has room for any monotonic_raw_ns stamp.
        # We sort keys to keep the binary layout deterministic — real
        # captures won't be sorted, but a fresh bootstrap should be
        # stable across regenerations.
        "T": "0000000000000000000",
    }
    serialised = json.dumps(obj, separators=(",", ":"))
    # json.dumps quoted the "T" digit string. Strip the quotes so it
    # becomes a raw integer literal (matches Binance's wire format).
    return serialised.replace('"T":"', '"T":').replace('0000000000000000000"',
                                                       '0000000000000000000')


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--scenario", required=True,
                   choices=("ex_market", "ex_market_2p"))
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--count", type=int, default=200,
                   help="number of frames (default 200)")
    p.add_argument("--seed", type=int, default=0)
    args = p.parse_args()

    rng = random.Random(args.seed)
    if args.scenario == "ex_market":
        symbols = ("BTCUSDT",)
    else:  # ex_market_2p: five symbols to match bench.conf default
        symbols = BINANCE_SYMBOLS_DEFAULT

    with args.out.open("w", encoding="utf-8") as fh:
        fh.write("# SYNTHETIC fixture. Regenerate from a real capture via:\n")
        fh.write("#   python3 capture_binance.py --stream btcusdt@bookTicker "
                 "--duration 1800 --out fixtures/ex_market_sample\n")
        fh.write("# The '#'-prefixed lines are skipped by PayloadPool.\n")
        for i in range(args.count):
            sym = symbols[i % len(symbols)]
            fh.write(synth_bookticker(sym, rng))
            fh.write("\n")
    print(f"[synth_fixture] wrote {args.count} frames to {args.out}",
          file=__import__("sys").stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
