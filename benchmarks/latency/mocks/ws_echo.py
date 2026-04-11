#!/usr/bin/env python3
"""WebSocket echo mock for lat_ws.

Reads ``[lat_ws]`` from bench.conf for ``port``. Performs an RFC 6455
server handshake on each new connection, then echoes every binary
frame back to the client as an unmasked binary frame.
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
            # Reply with a close of our own to be polite, then bail.
            try:
                conn.sendall(_ws.encode_frame(_ws.OPCODE_CLOSE, b""))
            except OSError:
                pass
            return
        if opcode == _ws.OPCODE_PING:
            conn.sendall(_ws.encode_frame(_ws.OPCODE_PONG, payload))
            continue
        # Text/binary: echo as binary, unmasked.
        conn.sendall(_ws.encode_frame(_ws.OPCODE_BINARY, payload))


def main() -> int:
    p = argparse.ArgumentParser(description="WebSocket echo mock (lat_ws)")
    p.add_argument("--config", default="benchmarks/latency/bench.conf")
    p.add_argument("--host", default=None, help="override mock_ip")
    p.add_argument("--port", type=int, default=None, help="override section port")
    args = p.parse_args()

    cfg = get_scenario(args.config, "lat_ws")
    host = args.host or cfg.get("mock_ip", "127.0.0.1")
    port = args.port if args.port is not None else int(cfg["port"])

    print("[ws_echo] listening on %s:%d" % (host, port), file=sys.stderr, flush=True)

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((host, port))
    srv.listen(1)

    try:
        while True:
            conn, addr = srv.accept()
            conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            print("[ws_echo] client connected: %s" % (addr,), file=sys.stderr, flush=True)
            try:
                handle(conn)
            except (ValueError, ConnectionError, OSError) as e:
                print("[ws_echo] client error: %s" % e, file=sys.stderr, flush=True)
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
