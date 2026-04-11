#!/usr/bin/env python3
"""Order-path WebSocket echo mock for lat_ex_order.

Reads ``[lat_ex_order]`` from bench.conf for ``port``. Performs the
RFC 6455 server handshake, then echoes each binary frame verbatim.
The bench client sends JSON payloads like ``{"e":"NewOrder","id":N}``
and matches responses by the ``id`` field regardless of event type,
so verbatim echo is sufficient to measure order-path round trip.
"""

import argparse
import socket
import sys

import _ws
from _conf import get_scenario


def handle(conn: socket.socket) -> None:
    _ws.accept_handshake(conn)
    while True:
        frame = _ws.decode_frame(conn)
        if frame is None:
            return
        opcode, payload = frame
        if opcode == _ws.OPCODE_CLOSE:
            try:
                conn.sendall(_ws.encode_frame(_ws.OPCODE_CLOSE, b""))
            except OSError:
                pass
            return
        if opcode == _ws.OPCODE_PING:
            conn.sendall(_ws.encode_frame(_ws.OPCODE_PONG, payload))
            continue
        # Echo back verbatim as binary.
        conn.sendall(_ws.encode_frame(_ws.OPCODE_BINARY, payload))


def main() -> int:
    p = argparse.ArgumentParser(description="Order-path WS echo mock (lat_ex_order)")
    p.add_argument("--config", default="benchmarks/latency/bench.conf")
    p.add_argument("--host", default=None, help="override mock_ip")
    p.add_argument("--port", type=int, default=None, help="override section port")
    args = p.parse_args()

    cfg = get_scenario(args.config, "lat_ex_order")
    host = args.host or cfg.get("mock_ip", "127.0.0.1")
    port = args.port if args.port is not None else int(cfg["port"])

    print("[ex_order_echo] listening on %s:%d" % (host, port), file=sys.stderr, flush=True)

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((host, port))
    srv.listen(1)

    try:
        while True:
            conn, addr = srv.accept()
            conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            print("[ex_order_echo] client connected: %s" % (addr,), file=sys.stderr, flush=True)
            try:
                handle(conn)
            except (ValueError, ConnectionError, OSError) as e:
                print("[ex_order_echo] client error: %s" % e, file=sys.stderr, flush=True)
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
