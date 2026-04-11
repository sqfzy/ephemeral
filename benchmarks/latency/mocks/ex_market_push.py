#!/usr/bin/env python3
"""Market-data WebSocket push mock for lat_ex_market.

Reads ``[lat_ex_market]`` from bench.conf for ``port``, ``push_rate_hz``,
``duration_seconds``. Performs the RFC 6455 server handshake, then
pushes timestamped JSON bookTicker frames at the configured rate for
the configured duration, stamping each frame with ``"T":<server_ns>``
captured *at the moment of send* via clock_gettime(CLOCK_MONOTONIC_RAW).

Per D-6: we stamp at the actual send instant, not at the intended
schedule, so rate-limiter jitter becomes measurement noise rather
than a systematic bias in one-way latency.
"""

import argparse
import socket
import sys

import _ws
from _clock import monotonic_raw_ns
from _conf import get_scenario
from _rate import RateLimiter


def _json_prefix() -> bytes:
    # Constant prefix — only the ``"T":<ns>`` tail changes per message.
    return b'{"e":"bookTicker","s":"BTCUSDT","b":"50000","B":"1.5","a":"50001","A":"2.0","T":'


def push_loop(conn: socket.socket, rate_hz: float, duration_s: float) -> None:
    _ws.accept_handshake(conn)
    limiter = RateLimiter(rate_hz)
    deadline_ns = monotonic_raw_ns() + int(duration_s * 1_000_000_000)
    prefix = _json_prefix()
    # Pre-build the trailing ``}`` as bytes.
    suffix = b"}"

    while True:
        limiter.wait()
        t_ns = monotonic_raw_ns()
        if t_ns >= deadline_ns:
            return
        body = prefix + str(t_ns).encode("ascii") + suffix
        conn.sendall(_ws.encode_frame(_ws.OPCODE_BINARY, body))


def main() -> int:
    p = argparse.ArgumentParser(description="Market-data WS push mock (lat_ex_market)")
    p.add_argument("--config", default="benchmarks/latency/bench.conf")
    p.add_argument("--host", default=None, help="override mock_ip")
    p.add_argument("--port", type=int, default=None, help="override section port")
    args = p.parse_args()

    cfg = get_scenario(args.config, "lat_ex_market")
    host = args.host or cfg.get("mock_ip", "127.0.0.1")
    port = args.port if args.port is not None else int(cfg["port"])
    rate_hz = float(cfg.get("push_rate_hz", "100000"))
    duration_s = float(cfg.get("duration_seconds", "30"))

    print(
        "[ex_market_push] listening on %s:%d rate=%.0fHz duration=%.1fs"
        % (host, port, rate_hz, duration_s),
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
            print("[ex_market_push] client connected: %s" % (addr,), file=sys.stderr, flush=True)
            try:
                push_loop(conn, rate_hz, duration_s)
            except (ValueError, ConnectionError, OSError) as e:
                print("[ex_market_push] client error: %s" % e, file=sys.stderr, flush=True)
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
