#!/usr/bin/env python3
"""UDP echo mock for lat_ex_md_udp (Phase 11.1).

Reads ``[lat_ex_md_udp]`` from bench.conf for ``port``. Binds to
``mock_ip`` (from globals) and echoes each datagram back to its
sender. Overwrites bytes ``[8:16]`` with ``t_mock_recv`` and bytes
``[16:24]`` with ``t_mock_send`` so the client can decompose the
round-trip into TX / RX legs.

History: Phase 10 shipped this scenario as a 1-way Mold64 push mock
(client was passive, mock drove rate). Phase 11.1 D-1 reverted that
decision because TX/RX leg decomposition is only meaningful with a
round-trip — the scenario is now structurally identical to udp_echo
with a different port/section name. The old push-loop, RateLimiter,
Mold64 header, and inner-message construction are all removed.
"""

import argparse
import socket
import struct
import sys

from _clock import monotonic_raw_ns
from _conf import get_scenario

_TS_BLOCK_SIZE = 24


def main() -> int:
    p = argparse.ArgumentParser(description="UDP echo mock (lat_ex_md_udp)")
    p.add_argument("--config", default="benchmarks/latency/bench.conf")
    p.add_argument("--host", default=None, help="override mock_ip")
    p.add_argument("--port", type=int, default=None, help="override section port")
    args = p.parse_args()

    cfg = get_scenario(args.config, "lat_ex_md_udp")
    host = args.host or cfg.get("mock_ip", "127.0.0.1")
    port = args.port if args.port is not None else int(cfg["port"])

    print("[ex_md_udp_echo] listening on %s:%d" % (host, port),
          file=sys.stderr, flush=True)

    srv = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
    except OSError:
        pass
    srv.bind((host, port))

    # Scratch buffer to avoid per-packet allocation (same optimisation
    # as udp_echo.py — the mock is expected to keep up with wire-speed
    # client RTT so every saved allocation shows up in the baseline).
    scratch = bytearray(65536)
    scratch_mv = memoryview(scratch)
    try:
        while True:
            n, addr = srv.recvfrom_into(scratch, 65536)
            if n == 0:
                continue
            if n >= _TS_BLOCK_SIZE:
                t_recv = monotonic_raw_ns()
                struct.pack_into("<Q", scratch, 8, t_recv)
                t_send = monotonic_raw_ns()
                struct.pack_into("<Q", scratch, 16, t_send)
            srv.sendto(scratch_mv[:n], addr)
    finally:
        srv.close()
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(0)
