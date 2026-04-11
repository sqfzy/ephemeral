#!/usr/bin/env python3
"""UDP echo mock for lat_udp.

Reads ``[lat_udp]`` from bench.conf for ``port``. Binds to ``mock_ip``
(from globals) and echoes each datagram back to its sender. Unlike
TCP, there is no per-client state — one recvfrom/sendto loop.
"""

import argparse
import socket
import sys

from _conf import get_scenario


def main() -> int:
    p = argparse.ArgumentParser(description="UDP echo mock (lat_udp)")
    p.add_argument("--config", default="benchmarks/latency/bench.conf")
    p.add_argument("--host", default=None, help="override mock_ip")
    p.add_argument("--port", type=int, default=None, help="override section port")
    args = p.parse_args()

    cfg = get_scenario(args.config, "lat_udp")
    host = args.host or cfg.get("mock_ip", "127.0.0.1")
    port = args.port if args.port is not None else int(cfg["port"])

    print("[udp_echo] listening on %s:%d" % (host, port), file=sys.stderr, flush=True)

    srv = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    # Ask the kernel for a generous receive buffer so bursty traffic
    # doesn't drop on the mock side and pollute latency measurements.
    try:
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
    except OSError:
        pass
    srv.bind((host, port))

    try:
        while True:
            data, addr = srv.recvfrom(65536)
            if data:
                srv.sendto(data, addr)
    finally:
        srv.close()
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(0)
