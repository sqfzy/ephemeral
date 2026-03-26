#!/usr/bin/env python3
"""Mock Binance WebSocket server for latency benchmarking.

Generates Binance combined bookTicker stream data with controllable
TLS record batching. Each --batch-size frames are concatenated into
a single buffer and sent via one ssl.send() call, producing one TLS
record — matching Binance's real behavior under high traffic.

Usage:
    python3 tools/mock_binance_server.py \
        --port 9443 --symbols btcusdt,ethusdt,solusdt \
        --batch-size 30 --rate 2000 --duration 30

Then connect the DPDK benchmark:
    sudo ./build/linux/arm64/release/bench_market_multi_dpdk \
        -a 0000:28:00.0 -l 4-7 -- \
        --host <kernel-nic-ip> --port 9443 \
        --local-ip 172.31.23.112 --gateway-ip 172.31.16.1 \
        --rx-cpu 8 --tx-cpu 9 --main-cpu 10 \
        --mode twophase --duration 30 --no-tls
        # NOTE: use --no-tls if connecting without TLS, or omit for TLS
"""

import argparse
import hashlib
import base64
import json
import os
import random
import signal
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import time


def generate_self_signed_cert():
    """Generate a self-signed TLS certificate using openssl CLI. Returns (cert_path, key_path)."""
    tmpdir = tempfile.mkdtemp(prefix="mock_binance_")
    cert_path = os.path.join(tmpdir, "cert.pem")
    key_path = os.path.join(tmpdir, "key.pem")
    subprocess.run(
        [
            "openssl", "req", "-x509", "-newkey", "rsa:2048",
            "-keyout", key_path, "-out", cert_path,
            "-days", "1", "-nodes",
            "-subj", "/CN=mock-binance-server",
        ],
        check=True, capture_output=True,
    )
    return cert_path, key_path, tmpdir


def ws_accept_key(client_key: str) -> str:
    """Compute Sec-WebSocket-Accept matching the project's WS GUID."""
    # Binance uses a non-standard GUID (verified empirically 2026-03-26).
    # RFC 6455 standard is 258EAFA5-E914-47DA-95CA-5AB5FC6D97BA.
    magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
    digest = hashlib.sha1((client_key.strip() + magic).encode()).digest()
    return base64.b64encode(digest).decode()


def encode_ws_text_frame(payload: bytes) -> bytes:
    """Encode a WebSocket text frame (server→client, no mask)."""
    length = len(payload)
    if length < 126:
        header = struct.pack("BB", 0x81, length)
    elif length < 65536:
        header = struct.pack("!BBH", 0x81, 126, length)
    else:
        header = struct.pack("!BBQ", 0x81, 127, length)
    return header + payload


def make_book_ticker(symbol: str, seq: int) -> bytes:
    """Generate a Binance combined stream bookTicker JSON."""
    price_base = {"btcusdt": 67000, "ethusdt": 3500, "solusdt": 150,
                  "bnbusdt": 600, "xrpusdt": 0.5, "dogeusdt": 0.08}
    base = price_base.get(symbol, 100)
    jitter = random.uniform(-0.01, 0.01)
    bid = round(base * (1 + jitter), 2)
    ask = round(bid + base * 0.0001, 2)
    now_ms = int(time.time() * 1000)
    msg = {
        "stream": f"{symbol}@bookTicker",
        "data": {
            "e": "bookTicker",
            "u": 1000000 + seq,
            "s": symbol.upper(),
            "b": f"{bid:.2f}",
            "B": f"{random.uniform(0.1, 10):.3f}",
            "a": f"{ask:.2f}",
            "A": f"{random.uniform(0.1, 10):.3f}",
            "T": now_ms,
            "E": now_ms,
        },
    }
    return json.dumps(msg, separators=(",", ":")).encode()


def handle_ws_upgrade(conn):
    """Read HTTP upgrade request, send 101 response."""
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = conn.recv(4096)
        if not chunk:
            raise ConnectionError("Client disconnected during upgrade")
        data += chunk

    # Extract Sec-WebSocket-Key
    ws_key = None
    for line in data.decode(errors="replace").split("\r\n"):
        if line.lower().startswith("sec-websocket-key:"):
            ws_key = line.split(":", 1)[1].strip()
            break

    if not ws_key:
        raise ValueError("No Sec-WebSocket-Key in upgrade request")

    accept = ws_accept_key(ws_key)
    print(f"  WS key: {ws_key!r} -> accept: {accept!r}")
    response = (
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Accept: {accept}\r\n"
        "\r\n"
    ).encode()
    conn.sendall(response)


def run_server(args):
    # Generate TLS cert
    print("Generating self-signed certificate...")
    cert_path, key_path, tmpdir = generate_self_signed_cert()

    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(cert_path, key_path)
    # Allow TLS 1.3 (default) — matches Binance
    ctx.minimum_version = ssl.TLSVersion.TLSv1_3

    symbols = args.symbols.split(",")
    batch_size = args.batch_size
    rate = args.rate  # msg/s, 0 = unlimited
    duration = args.duration

    # Bind
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", args.port))
    srv.listen(1)

    print(f"Listening on 0.0.0.0:{args.port} (TLS 1.3)")
    print(f"  symbols:    {symbols}")
    print(f"  batch-size: {batch_size} frames/record")
    print(f"  rate:       {rate} msg/s {'(unlimited)' if rate == 0 else ''}")
    print(f"  duration:   {duration}s")
    print(f"Waiting for client connection...")

    raw_conn, addr = srv.accept()
    print(f"Client connected: {addr}")

    conn = ctx.wrap_socket(raw_conn, server_side=True)
    print(f"TLS handshake complete: {conn.version()}, {conn.cipher()[0]}")

    handle_ws_upgrade(conn)
    print("WebSocket upgrade complete. Sending data...")

    # Disable Nagle for precise record control
    conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    # Main send loop
    running = True
    def on_signal(sig, frame):
        nonlocal running
        running = False
    signal.signal(signal.SIGINT, on_signal)
    signal.signal(signal.SIGTERM, on_signal)

    total_msgs = 0
    total_bytes = 0
    total_records = 0
    seq = 0
    start = time.monotonic()
    sym_idx = 0

    # Interval between batches
    if rate > 0:
        batch_interval = batch_size / rate  # seconds per batch
    else:
        batch_interval = 0

    try:
        while running:
            elapsed = time.monotonic() - start
            if duration > 0 and elapsed >= duration:
                break

            # Build one TLS record = batch_size WS frames
            buf = bytearray()
            for _ in range(batch_size):
                symbol = symbols[sym_idx % len(symbols)]
                sym_idx += 1
                payload = make_book_ticker(symbol, seq)
                seq += 1
                buf.extend(encode_ws_text_frame(payload))

            # Single send = single TLS record
            conn.sendall(bytes(buf))
            total_msgs += batch_size
            total_bytes += len(buf)
            total_records += 1

            # Rate limiting
            if batch_interval > 0:
                target_time = start + total_records * batch_interval
                now = time.monotonic()
                if target_time > now:
                    time.sleep(target_time - now)

    except (BrokenPipeError, ConnectionResetError, OSError) as e:
        print(f"Connection closed: {e}")

    elapsed = time.monotonic() - start
    print(f"\n=== Summary ===")
    print(f"  Duration:    {elapsed:.1f}s")
    print(f"  Messages:    {total_msgs}")
    print(f"  TLS records: {total_records}")
    print(f"  Msg rate:    {total_msgs / elapsed:.0f} msg/s")
    print(f"  Record rate: {total_records / elapsed:.0f} rec/s")
    print(f"  Avg batch:   {total_msgs / max(total_records, 1):.1f} frames/record")
    print(f"  Total bytes: {total_bytes:,}")

    conn.close()
    srv.close()

    # Cleanup temp cert
    import shutil
    shutil.rmtree(tmpdir, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser(
        description="Mock Binance WebSocket server for latency benchmarking")
    parser.add_argument("--port", type=int, default=9443,
                        help="Listen port (default: 9443)")
    parser.add_argument("--symbols", default="btcusdt,ethusdt,solusdt",
                        help="Comma-separated symbol list")
    parser.add_argument("--batch-size", type=int, default=1,
                        help="WS frames per TLS record (default: 1)")
    parser.add_argument("--rate", type=int, default=0,
                        help="Target msg/s rate, 0=unlimited (default: 0)")
    parser.add_argument("--duration", type=int, default=30,
                        help="Run duration in seconds (default: 30)")
    args = parser.parse_args()
    run_server(args)


if __name__ == "__main__":
    main()
