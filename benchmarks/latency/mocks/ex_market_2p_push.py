#!/usr/bin/env python3
"""Multi-symbol burst-push mock for lat_ex_market_2p.

Reads ``[lat_ex_market_2p]`` from bench.conf for ``port``, ``push_rate_hz``,
``duration_seconds``, ``burst_size``, ``symbols``. Performs the RFC 6455
server handshake, then pushes timestamped JSON bookTicker frames in bursts.

Each tick (1/push_rate_hz):
  - Generates ``burst_size`` frames, each with a randomly chosen symbol
    from the ``symbols`` list (duplicates allowed within a burst).
  - Stamps every frame with ``"T":<server_ns>`` at the moment of
    construction via clock_gettime(CLOCK_MONOTONIC_RAW).
  - Concatenates all WS binary frames and sends via a single sendall().

This models real exchange behaviour where multiple symbol updates arrive
in one TCP segment, creating the burst pattern that two-phase processing
is designed to optimise.
"""

import argparse
import random
import socket
import sys

import _ws
from _clock import monotonic_raw_ns
from _conf import get_scenario
from _rate import RateLimiter

# Fixed price templates per symbol — benchmark only measures latency,
# not price semantics, so constant values are fine.
_PRICE_TEMPLATES = {
    "BTCUSDT":  (b'"b":"67000","B":"1.5","a":"67001","A":"2.0"',),
    "ETHUSDT":  (b'"b":"3200","B":"5.0","a":"3201","A":"4.0"',),
    "SOLUSDT":  (b'"b":"145","B":"20.0","a":"145.1","A":"15.0"',),
    "BNBUSDT":  (b'"b":"580","B":"3.0","a":"580.1","A":"2.5"',),
    "XRPUSDT":  (b'"b":"0.52","B":"1000","a":"0.521","A":"800"',),
}

# Default fallback for symbols not in the template table.
_DEFAULT_PRICES = b'"b":"100","B":"10.0","a":"100.1","A":"9.0"'


def _build_prefix_suffix(sym: str) -> tuple[bytes, bytes]:
    """Return (prefix, suffix) for a given symbol.

    A frame is: prefix + str(T_ns).encode() + suffix
    """
    prices = _PRICE_TEMPLATES.get(sym, (_DEFAULT_PRICES,))[0]
    prefix = b'{"e":"bookTicker","s":"' + sym.encode("ascii") + b'",' + prices + b',"T":'
    suffix = b"}"
    return prefix, suffix


def push_loop(conn: socket.socket, rate_hz: float, duration_s: float,
              burst_size: int, symbols: list[str]) -> None:
    _ws.accept_handshake(conn)
    limiter = RateLimiter(rate_hz)
    deadline_ns = monotonic_raw_ns() + int(duration_s * 1_000_000_000)

    # Pre-build per-symbol prefix/suffix to avoid repeated allocation.
    sym_parts = [(s, *_build_prefix_suffix(s)) for s in symbols]
    n_symbols = len(sym_parts)

    while True:
        limiter.wait()
        t_check = monotonic_raw_ns()
        if t_check >= deadline_ns:
            return

        # Build burst: burst_size frames, symbol chosen randomly.
        buf = bytearray()
        for _ in range(burst_size):
            _, prefix, suffix = sym_parts[random.randint(0, n_symbols - 1)]
            t_ns = monotonic_raw_ns()
            body = prefix + str(t_ns).encode("ascii") + suffix
            buf.extend(_ws.encode_frame(_ws.OPCODE_BINARY, body))

        conn.sendall(buf)


def main() -> int:
    p = argparse.ArgumentParser(
        description="Multi-symbol burst WS push mock (lat_ex_market_2p)")
    p.add_argument("--config", default="benchmarks/latency/bench.conf")
    p.add_argument("--host", default=None, help="override mock_ip")
    p.add_argument("--port", type=int, default=None, help="override section port")
    args = p.parse_args()

    cfg = get_scenario(args.config, "lat_ex_market_2p")
    host = args.host or cfg.get("mock_ip", "127.0.0.1")
    port = args.port if args.port is not None else int(cfg["port"])
    rate_hz = float(cfg.get("push_rate_hz", "10000"))
    duration_s = float(cfg.get("duration_seconds", "300"))
    burst_size = int(cfg.get("burst_size", "10"))
    symbols = [s.strip() for s in cfg.get("symbols", "BTCUSDT,ETHUSDT,SOLUSDT,BNBUSDT,XRPUSDT").split(",")]

    print(
        "[ex_market_2p_push] listening on %s:%d rate=%.0fHz burst=%d "
        "symbols=%s duration=%.1fs"
        % (host, port, rate_hz, burst_size, ",".join(symbols), duration_s),
        file=sys.stderr,
        flush=True,
    )

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((host, port))
    srv.listen(1)

    try:
        while True:
            conn, addr = srv.accept()
            conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            print("[ex_market_2p_push] client connected: %s" % (addr,),
                  file=sys.stderr, flush=True)
            try:
                push_loop(conn, rate_hz, duration_s, burst_size, symbols)
            except (ValueError, ConnectionError, OSError) as e:
                print("[ex_market_2p_push] client error: %s" % e,
                      file=sys.stderr, flush=True)
            finally:
                conn.close()
    finally:
        srv.close()
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(0)
