#!/usr/bin/env python3
"""TCP echo mock for lat_tcp.

Reads ``[lat_tcp]`` from bench.conf for ``port``. Binds to ``mock_ip``
(from globals) and echoes every byte back to the client. Used as the
kernel echo peer for both the kernel and DPDK client variants so the
comparison is fair — only the client path differs.
"""

import argparse
import socket
import sys

from _conf import get_scenario


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

    try:
        while True:
            conn, addr = srv.accept()
            conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            print("[tcp_echo] client connected: %s" % (addr,), file=sys.stderr, flush=True)
            try:
                while True:
                    data = conn.recv(65536)
                    if not data:
                        break
                    conn.sendall(data)
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
