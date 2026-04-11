#!/usr/bin/env python3
"""UDP market-data push mock for lat_ex_md_udp.

Reads ``[lat_ex_md_udp]`` from bench.conf for ``port``, ``push_rate_hz``,
``msg_per_packet``, ``duration_seconds``. Binds no listening socket;
instead, it constructs UDP datagrams and sends them to ``client_ip:port``
(from globals). The client is the one that bind()s.

Packet layout (Mold64-ish):

    [0:10]   session_id  : ASCII 'EPHBENCH\\0\\0' (10 bytes exact)
    [10:18]  seq         : uint64 BE
    [18:20]  msg_count   : uint16 BE
    [20:..]  N inner messages

Inner message (fake ITCH, 25 bytes each):

    [0:1]    msg_type       = b'T'
    [1:9]    server_send_ns : uint64 LE
    [9:17]   symbol         : 8 ASCII chars, null-padded
    [17:25]  price          : uint64 LE

server_send_ns is captured at the moment of send via
clock_gettime(CLOCK_MONOTONIC_RAW) per each inner message — the
client uses this for per-message one-way latency.
"""

import argparse
import socket
import struct
import sys

from _clock import monotonic_raw_ns
from _conf import get_scenario
from _rate import RateLimiter

_SESSION_ID = b"EPHBENCH\x00\x00"  # 10 bytes
_SYMBOL = b"BTCUSDT\x00"  # 8 bytes
_PRICE = 5_000_000_000  # arbitrary uint64
_INNER_PREFIX = b"T"


def build_packet(seq: int, msg_per_packet: int) -> bytes:
    """Build one Mold64 datagram with msg_per_packet inner messages."""
    # Header (20 bytes): session_id (10) + seq (8 BE) + msg_count (2 BE)
    header = _SESSION_ID + struct.pack("!QH", seq, msg_per_packet)
    # Inner messages carry individual server_send_ns stamps.
    parts = [header]
    for _ in range(msg_per_packet):
        ts = monotonic_raw_ns()
        parts.append(_INNER_PREFIX + struct.pack("<Q", ts) + _SYMBOL + struct.pack("<Q", _PRICE))
    return b"".join(parts)


def main() -> int:
    p = argparse.ArgumentParser(description="UDP market-data push mock (lat_ex_md_udp)")
    p.add_argument("--config", default="benchmarks/latency/bench.conf")
    p.add_argument("--dest-ip", default=None, help="override client_ip destination")
    p.add_argument("--dest-port", type=int, default=None, help="override section port")
    args = p.parse_args()

    cfg = get_scenario(args.config, "lat_ex_md_udp")
    dest_ip = args.dest_ip or cfg.get("client_ip", "127.0.0.1")
    dest_port = args.dest_port if args.dest_port is not None else int(cfg["port"])
    rate_hz = float(cfg.get("push_rate_hz", "100000"))
    msg_per_packet = int(cfg.get("msg_per_packet", "5"))
    duration_s = float(cfg.get("duration_seconds", "30"))

    print(
        "[ex_md_udp_push] pushing to %s:%d rate=%.0fHz mpp=%d duration=%.1fs"
        % (dest_ip, dest_port, rate_hz, msg_per_packet, duration_s),
        file=sys.stderr,
        flush=True,
    )

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4 * 1024 * 1024)
    except OSError:
        pass
    # Phase 11.0: bind the sender's source to (mock_ip, dest_port) so DPDK
    # client (lat_ex_md_udp_dpdk) can register a fixed 4-tuple for inbound
    # routing. Kernel lat_ex_md_udp doesn't care about the source port (it
    # bind()s on (client_ip, dest_port) and receives any incoming UDP to
    # that 2-tuple), so this change is transparent to the kernel mode.
    try:
        src_bind_ip = cfg.get("mock_ip", "0.0.0.0")
        sock.bind((src_bind_ip, dest_port))
    except OSError as e:
        print("[ex_md_udp_push] bind(%s:%d) failed: %s — continuing with ephemeral src_port "
              "(DPDK client routing may miss packets)"
              % (src_bind_ip, dest_port, e), file=sys.stderr, flush=True)

    limiter = RateLimiter(rate_hz)
    deadline_ns = monotonic_raw_ns() + int(duration_s * 1_000_000_000)
    seq = 0
    try:
        while True:
            limiter.wait()
            now_ns = monotonic_raw_ns()
            if now_ns >= deadline_ns:
                break
            pkt = build_packet(seq, msg_per_packet)
            try:
                sock.sendto(pkt, (dest_ip, dest_port))
            except OSError as e:
                # Destination unreachable is expected when the client
                # hasn't started yet; keep going rather than dying.
                print("[ex_md_udp_push] sendto err: %s" % e, file=sys.stderr, flush=True)
            seq += 1
    finally:
        sock.close()
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(0)
