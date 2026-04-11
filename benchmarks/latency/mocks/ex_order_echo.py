#!/usr/bin/env python3
"""Order-path WebSocket echo mock for lat_ex_order.

Reads ``[lat_ex_order]`` from bench.conf for ``port``. Performs the
RFC 6455 server handshake, then echoes each binary frame as JSON with
two extra timestamp fields for the Phase 11.1 TX/RX leg decomposition.

Phase 11.1 protocol:
  * client sends ``{"e":"NewOrder","id":N,"t_client":<ns>}``
  * mock parses via json.loads, adds ``t_mock_recv`` (captured just
    after recv) and ``t_mock_send`` (captured just before send), dumps
    back to bytes, and encodes a server->client binary frame
  * client matches on ``id`` and extracts all three timestamps

Order-path payloads are small (< 200 B) — json.loads/dumps cost is
small vs. wire RTT, which the TX/RX legs will reveal if the mock ever
becomes the bottleneck.
"""

import argparse
import json
import socket
import sys

import _ws
from _clock import monotonic_raw_ns
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
        # Phase 11.1: parse JSON, stamp t_mock_recv / t_mock_send,
        # re-dump, echo as binary. On malformed input fall back to
        # verbatim echo so the client's malformed-counter catches it
        # and skips the sample rather than blocking the pipeline.
        t_recv = monotonic_raw_ns()
        try:
            obj = json.loads(payload)
        except (ValueError, UnicodeDecodeError):
            conn.sendall(_ws.encode_frame(_ws.OPCODE_BINARY, payload))
            continue
        if isinstance(obj, dict):
            obj["t_mock_recv"] = t_recv
            obj["t_mock_send"] = monotonic_raw_ns()
            out = json.dumps(obj, separators=(",", ":")).encode("utf-8")
        else:
            out = payload
        conn.sendall(_ws.encode_frame(_ws.OPCODE_BINARY, out))


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
