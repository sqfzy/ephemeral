#!/usr/bin/env python3
"""TCP echo mock for lat_tcp.

Reads ``[lat_tcp]`` from bench.conf for ``port``. Binds to ``mock_ip``
(from globals) and echoes every byte back to the client. Used as the
kernel echo peer for both the kernel and DPDK client variants so the
comparison is fair — only the client path differs.

Phase 11.1: on each chunk received, if it is at least 24 bytes we
overwrite bytes ``[8:16]`` with ``t_mock_recv`` (captured immediately
after recv()) and bytes ``[16:24]`` with ``t_mock_send`` (captured just
before sendall()), preserving the client timestamp at ``[0:8]`` and the
filler payload at ``[24:]``. The client decodes these three timestamps
to compute the RTT / TX / RX leg split.

Rewrites are scoped to the first 24 bytes of the *first* chunk the
kernel hands us — RawStreamCodec streams bytes, so a single send() on
the client side may surface as multiple recv() chunks here. Subsequent
chunks are echoed verbatim (the timestamp block is only at the head of
each application-level frame anyway).
"""

import argparse
import socket
import struct
import sys

from _clock import monotonic_raw_ns
from _conf import get_scenario

_TS_BLOCK_SIZE = 24


def main() -> int:
    p = argparse.ArgumentParser(description="TCP echo mock (lat_tcp)")
    p.add_argument("--config", default="benchmarks/latency/bench.conf")
    p.add_argument("--host", default=None, help="override mock_ip")
    p.add_argument("--port", type=int, default=None, help="override section port")
    args = p.parse_args()

    cfg = get_scenario(args.config, "lat_tcp")
    host = args.host or cfg.get("mock_ip", "127.0.0.1")
    port = args.port if args.port is not None else int(cfg["port"])

    print("[tcp_echo] listening on %s:%d" % (host, port), file=sys.stderr, flush=True)

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((host, port))
    srv.listen(1)

    # Scratch buffer + memoryview: avoid re-allocating per recv() so the
    # mock's wall-clock overhead per echo stays as low as possible. The
    # Phase 11.1 baseline gate is sensitive to mock-side cost since the
    # mock's recv→send delta is on the critical path for RTT p50.
    scratch = bytearray(65536)
    scratch_mv = memoryview(scratch)

    try:
        while True:
            conn, addr = srv.accept()
            conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            print("[tcp_echo] client connected: %s" % (addr,), file=sys.stderr, flush=True)
            try:
                while True:
                    n = conn.recv_into(scratch, 65536)
                    if n == 0:
                        break
                    if n >= _TS_BLOCK_SIZE:
                        t_recv = monotonic_raw_ns()
                        struct.pack_into("<Q", scratch, 8, t_recv)
                        t_send = monotonic_raw_ns()
                        struct.pack_into("<Q", scratch, 16, t_send)
                    conn.sendall(scratch_mv[:n])
            except (ConnectionError, OSError) as e:
                print("[tcp_echo] client error: %s" % e, file=sys.stderr, flush=True)
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
