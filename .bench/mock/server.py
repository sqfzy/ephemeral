#!/usr/bin/env python3
"""Mock WebSocket server for ephemeral benchmarks.

Supports:
  1. Ping/Pong echo: responds to WebSocket PING with PONG (preserving payload)
  2. Market data: pushes Binance-format bookTicker JSON with realistic 5-phase
     market stress simulation (calm → ramp → burst → volatile → cooldown)

Usage:
  # TLS + stress simulation (default):
  python3 server.py --port 9443 --tls --mode stress

  # Constant rate:
  python3 server.py --port 9080 --no-tls --mode constant --rate 500

  # Custom stress profile:
  python3 server.py --port 9443 --tls --mode stress --stress-duration 120
"""

import argparse
import asyncio
import json
import logging
import math
import os
import random
import signal
import ssl
import time
from pathlib import Path
from urllib.parse import parse_qs

import websockets
from websockets.server import serve

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
)
log = logging.getLogger("mock-server")

# ═══════════════════════════════════════════════════════════════════════════════
# 5-Phase Market Stress Profile
# ═══════════════════════════════════════════════════════════════════════════════
#
# Simulates a BTC flash crash / liquidation cascade:
#
#   Phase 1 (Calm)      : 30-80 msg/s,   1-3 bps spread, thick depth
#   Phase 2 (Ramp)      : 200-500 msg/s, 3-10 bps, depth thinning
#   Phase 3 (Burst)     : 2000-5000 msg/s, 20-100+ bps, micro qty, price gaps
#   Phase 4 (Volatile)  : 500-2000 msg/s, 10-50 bps, whipsaw
#   Phase 5 (Cooldown)  : 100-300 msg/s, 3-10 bps, recovery
#
# Total cycle default: ~210s (scales with --stress-duration)

class StressPhase:
    """One phase of the market stress cycle."""
    def __init__(self, name: str, duration_frac: float,
                 rate_lo: float, rate_hi: float,
                 spread_bps_lo: float, spread_bps_hi: float,
                 qty_lo: float, qty_hi: float,
                 price_drift: float = 0.0,
                 price_jump_prob: float = 0.0,
                 price_jump_bps: float = 0.0):
        self.name = name
        self.duration_frac = duration_frac   # fraction of total cycle
        self.rate_lo = rate_lo               # msg/s per symbol (low end)
        self.rate_hi = rate_hi               # msg/s per symbol (high end)
        self.spread_bps_lo = spread_bps_lo
        self.spread_bps_hi = spread_bps_hi
        self.qty_lo = qty_lo                 # bid/ask quantity range
        self.qty_hi = qty_hi
        self.price_drift = price_drift       # cumulative drift per second (bps)
        self.price_jump_prob = price_jump_prob  # probability of price gap per tick
        self.price_jump_bps = price_jump_bps    # gap size in bps

# Default stress profile (fractions must sum to 1.0)
#
# Calibrated to match real Binance BTCUSDT bookTicker characteristics:
#   - avg msg/s: ~1100 (3 symbols combined)
#   - msg size: ~180 bytes
#   - Per rx_burst: ~3.4 WS frames at steady state
#   - Time distribution: 62% at 100-500 msg/s, 13% quiet, 6% burst >2000
#   - Peak: ~14000 msg/s during liquidation cascades
#
# Overnight measurement (2026-03-29, 8h, fstream.binance.com):
#   min=27, p50≈300, avg=524, p99≈5000, max=13943 msg/s
STRESS_PHASES = [
    StressPhase("calm",     duration_frac=0.40,
                rate_lo=30, rate_hi=200,
                spread_bps_lo=1, spread_bps_hi=3,
                qty_lo=5.0,  qty_hi=50.0),
    StressPhase("ramp",     duration_frac=0.10,
                rate_lo=400, rate_hi=1500,
                spread_bps_lo=3, spread_bps_hi=10,
                qty_lo=1.0,  qty_hi=20.0,
                price_drift=-5.0),
    StressPhase("burst",    duration_frac=0.06,
                rate_lo=5000, rate_hi=14000,
                spread_bps_lo=20, spread_bps_hi=100,
                qty_lo=0.01, qty_hi=2.0,
                price_drift=-30.0,
                price_jump_prob=0.05, price_jump_bps=50.0),
    StressPhase("volatile", duration_frac=0.14,
                rate_lo=500,  rate_hi=3000,
                spread_bps_lo=10, spread_bps_hi=50,
                qty_lo=0.5,  qty_hi=10.0,
                price_drift=5.0,
                price_jump_prob=0.01, price_jump_bps=20.0),
    StressPhase("cooldown", duration_frac=0.30,
                rate_lo=50,   rate_hi=300,
                spread_bps_lo=3, spread_bps_hi=10,
                qty_lo=3.0,  qty_hi=30.0,
                price_drift=2.0),
]

# ═══════════════════════════════════════════════════════════════════════════════
# Price state per symbol
# ═══════════════════════════════════════════════════════════════════════════════

BASE_PRICES = {
    "btcusdt": 67500.0, "ethusdt": 3450.0, "solusdt": 185.0,
    "bnbusdt": 610.0, "xrpusdt": 0.62, "dogeusdt": 0.165,
    "adausdt": 0.48, "dotusdt": 7.85, "avaxusdt": 38.5,
    "maticusdt": 0.72,
}


class SymbolState:
    """Mutable price state for one symbol during stress simulation."""
    def __init__(self, symbol: str):
        self.symbol = symbol
        self.price = BASE_PRICES.get(symbol, 100.0)
        self.base_price = self.price

    def tick(self, phase: StressPhase, dt: float) -> str:
        """Generate one bookTicker message, advancing price state."""
        # Price drift
        drift_bps = phase.price_drift * dt
        self.price *= (1.0 + drift_bps * 1e-4)

        # Random price jump
        if random.random() < phase.price_jump_prob:
            direction = -1 if random.random() < 0.7 else 1  # bias down in crash
            self.price *= (1.0 + direction * phase.price_jump_bps * 1e-4)

        # Spread
        spread_bps = random.uniform(phase.spread_bps_lo, phase.spread_bps_hi)
        half_spread = self.price * spread_bps * 0.5e-4
        bid = self.price - half_spread
        ask = self.price + half_spread

        # Quantity
        bid_qty = round(random.uniform(phase.qty_lo, phase.qty_hi), 4)
        ask_qty = round(random.uniform(phase.qty_lo, phase.qty_hi), 4)

        # Use nanosecond timestamp for precise feed latency measurement.
        # The benchmark detects ns vs ms by magnitude (>1.5e18 → nanoseconds).
        event_time_ns = time.time_ns()

        data = {
            "stream": f"{self.symbol}@bookTicker",
            "data": {
                "e": "bookTicker",
                "u": random.randint(1000000, 9999999),
                "E": event_time_ns,
                "T": event_time_ns,
                "s": self.symbol.upper(),
                "b": f"{bid:.2f}",
                "B": f"{bid_qty:.4f}",
                "a": f"{ask:.2f}",
                "A": f"{ask_qty:.4f}",
            },
        }
        return json.dumps(data, separators=(",", ":"))


# ═══════════════════════════════════════════════════════════════════════════════
# WebSocket handler
# ═══════════════════════════════════════════════════════════════════════════════

# Global config (set by main before server starts)
_server_config = {
    "mode": "stress",
    "rate": 200,
    "stress_duration": 210,
    "loop_stress": True,
}


async def handler(websocket):
    # websockets v15: request is available after open
    # Try multiple attribute paths for compatibility
    path = "/"
    try:
        if hasattr(websocket, 'request') and websocket.request:
            req = websocket.request
            if hasattr(req, 'path'):
                path = req.path
            elif hasattr(req, 'url'):
                path = req.url
    except Exception:
        pass
    # Fallback: check websocket.path (older websockets versions)
    if path == "/" and hasattr(websocket, 'path'):
        path = websocket.path or "/"
    peer = websocket.remote_address
    log.info(f"Connection from {peer}, path={path}")

    # Parse subscribed symbols from path.
    # Two URL formats:
    #   /stream?streams=btcusdt@bookTicker/ethusdt@bookTicker  (query-string)
    #   /ws/btcusdt@bookTicker/ethusdt@bookTicker              (/ws/ path segments)
    symbols = []
    if "/stream" in path:
        try:
            query = path.split("?", 1)[1] if "?" in path else ""
            params = parse_qs(query)
            streams_raw = params.get("streams", [""])[0]
            for s in streams_raw.split("/"):
                if "@" in s:
                    symbols.append(s.split("@")[0])
        except Exception:
            pass
    elif path.startswith("/ws/"):
        # /ws/btcusdt@bookTicker/ethusdt@bookTicker/...
        for segment in path[4:].split("/"):
            if "@" in segment:
                symbols.append(segment.split("@")[0])

    if symbols:
        mode = _server_config["mode"]
        # /ws/ path: client sends text frames (order simulation) alongside
        # market data — run market push + echo concurrently.
        if path.startswith("/ws/"):
            log.info(f"  Market data + echo [{mode}]: {len(symbols)} symbols: {symbols}")
            await market_and_echo_loop(websocket, symbols)
        else:
            log.info(f"  Market data [{mode}]: {len(symbols)} symbols: {symbols}")
            if mode == "stress":
                await stress_market_loop(websocket, symbols)
            else:
                await constant_market_loop(websocket, symbols)
    else:
        log.info(f"  Ping/pong echo mode")
        await echo_loop(websocket)


async def echo_loop(websocket):
    """Echo text/binary messages. PING/PONG handled by websockets library."""
    try:
        async for message in websocket:
            await websocket.send(message)
    except websockets.ConnectionClosed:
        log.info(f"Echo connection closed: {websocket.remote_address}")


async def market_and_echo_loop(websocket, symbols: list[str]):
    """Concurrent market data push + text frame echo.

    Used by /ws/<sym>@bookTicker paths where the client sends simulated
    order text frames and expects echo responses alongside market data.
    Market push uses the configured mode (stress/constant).
    """
    mode = _server_config["mode"]

    async def echo_task():
        """Echo incoming text frames (order simulation responses)."""
        try:
            async for message in websocket:
                await websocket.send(message)
        except websockets.ConnectionClosed:
            pass

    async def market_task():
        """Push market data using the configured mode."""
        try:
            if mode == "stress":
                await stress_market_loop(websocket, symbols)
            else:
                await constant_market_loop(websocket, symbols)
        except websockets.ConnectionClosed:
            pass

    # Run both concurrently; when either finishes (connection closed), cancel the other.
    echo = asyncio.create_task(echo_task())
    market = asyncio.create_task(market_task())
    try:
        done, pending = await asyncio.wait(
            [echo, market], return_when=asyncio.FIRST_COMPLETED)
        for task in pending:
            task.cancel()
            try:
                await task
            except asyncio.CancelledError:
                pass
    except Exception:
        echo.cancel()
        market.cancel()
    log.info(f"Market+echo connection closed: {websocket.remote_address}")


async def constant_market_loop(websocket, symbols: list[str]):
    """Push at constant rate (legacy mode)."""
    rate = _server_config["rate"]
    interval = 1.0 / rate if rate > 0 else 0.005
    states = {s: SymbolState(s) for s in symbols}
    calm = STRESS_PHASES[0]  # use calm phase params for price behavior
    count = 0
    try:
        while True:
            for sym in symbols:
                msg = states[sym].tick(calm, interval)
                await websocket.send(msg)
                count += 1
            await asyncio.sleep(interval)
    except websockets.ConnectionClosed:
        log.info(f"Constant feed closed after {count} messages")


async def stress_market_loop(websocket, symbols: list[str]):
    """5-phase stress simulation with realistic rate/spread/price dynamics.

    Simulates TCP batching observed in real Binance feeds: at high rates
    (>1000 msg/s), multiple messages are collected and sent together so
    they arrive in the same TCP segment. This produces realistic
    per-rx_burst frame counts (2-5 WS frames per burst at peak).
    """
    cycle_duration = _server_config["stress_duration"]
    loop_stress = _server_config["loop_stress"]
    states = {s: SymbolState(s) for s in symbols}
    total_msgs = 0
    cycle_num = 0

    try:
        while True:
            cycle_num += 1
            cycle_start = time.monotonic()

            # Reset prices at cycle start
            for st in states.values():
                st.price = st.base_price

            log.info(f"  Stress cycle {cycle_num} started "
                     f"({cycle_duration}s, {len(symbols)} symbols)")

            for phase in STRESS_PHASES:
                phase_duration = phase.duration_frac * cycle_duration
                phase_start = time.monotonic()
                phase_msgs = 0

                # Interpolate rate within the phase (smooth ramp)
                while True:
                    elapsed = time.monotonic() - phase_start
                    if elapsed >= phase_duration:
                        break

                    # Linear interpolation of rate within phase
                    t = elapsed / phase_duration  # 0..1
                    rate_per_sym = phase.rate_lo + (phase.rate_hi - phase.rate_lo) * t
                    total_rate = rate_per_sym * len(symbols)

                    # Simulate TCP batching + network jitter from real Binance.
                    #
                    # Real Binance avg: ~3.4 frames/TCP segment at ~1000 msg/s.
                    # But the distribution is NOT uniform — most bursts are 2-3
                    # frames, with occasional micro-bursts of 6-10 frames caused
                    # by router queue buildup and ECMP path variation. This tail
                    # drives the p99 RX latency (more frames in one burst =
                    # more sequential decrypt+decode work).
                    #
                    # We model this with a base batch + random jitter spike:
                    #   - 90% of batches: base_size (2-3 frames)
                    #   - 10% of batches: spike to 5-10 frames (micro-burst)
                    # Batch size with jitter spike to simulate multi-frame
                    # TLS records observed in real Binance (up to 14 frames/record
                    # during burst, measured 2026-03-30).
                    if total_rate > 5000:
                        base = 3
                        spike = random.randint(8, 14)
                    elif total_rate > 1000:
                        base = 2
                        spike = random.randint(6, 10)
                    elif total_rate > 500:
                        base = 2
                        spike = random.randint(4, 6)
                    else:
                        base = 1
                        spike = 1

                    if spike > base and random.random() < 0.10:
                        batch_size = spike
                    else:
                        batch_size = base

                    msgs_in_batch = []
                    for _ in range(batch_size):
                        for sym in symbols:
                            msg = states[sym].tick(phase, 1.0 / max(rate_per_sym, 1))
                            msgs_in_batch.append(msg)

                    if batch_size > 1:
                        # Simulate Binance's multi-frame TLS records:
                        # Build multiple WS frame byte sequences, concatenate
                        # them, then write as one chunk through the TLS layer.
                        # This produces ONE TLS record containing N WS frames,
                        # matching observed Binance behavior (up to 14 frames
                        # per record during burst).
                        import struct
                        raw = bytearray()
                        for msg in msgs_in_batch:
                            payload = msg.encode("utf-8")
                            plen = len(payload)
                            # Build a complete unmasked WS text frame (server→client)
                            if plen <= 125:
                                raw += struct.pack("!BB", 0x81, plen) + payload
                            elif plen <= 65535:
                                raw += struct.pack("!BBH", 0x81, 126, plen) + payload
                            else:
                                raw += struct.pack("!BBQ", 0x81, 127, plen) + payload
                        # Write raw WS frames directly through the transport.
                        # asyncio SSL transport encrypts the entire buffer as
                        # one TLS record.
                        # Log first few batch sizes for debugging
                        if phase_msgs < 50:
                            log.debug(f"    batch raw: {len(raw)}B, {len(msgs_in_batch)} frames")
                        websocket.transport.write(raw)
                    else:
                        await websocket.send(msgs_in_batch[0])

                    phase_msgs += len(msgs_in_batch)

                    # Sleep to achieve target rate (adjusted for batch)
                    batch_interval = len(msgs_in_batch) / max(total_rate, 1)
                    await asyncio.sleep(batch_interval)

                total_msgs += phase_msgs
                log.info(f"    [{phase.name:>9s}] {phase_msgs:>6d} msgs, "
                         f"{phase_msgs/max(phase_duration,0.01):.0f} msg/s avg, "
                         f"BTC={states.get('btcusdt', list(states.values())[0]).price:.0f}")

            cycle_elapsed = time.monotonic() - cycle_start
            log.info(f"  Cycle {cycle_num} done: {total_msgs} msgs total, "
                     f"{cycle_elapsed:.1f}s elapsed")

            if not loop_stress:
                break

    except websockets.ConnectionClosed:
        log.info(f"Stress feed closed after {total_msgs} messages, "
                 f"{cycle_num} cycles")


# ═══════════════════════════════════════════════════════════════════════════════
# Server startup
# ═══════════════════════════════════════════════════════════════════════════════

async def run_server(args):
    ssl_ctx = None
    if args.tls:
        ssl_ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        cert_dir = Path(__file__).parent
        ssl_ctx.load_cert_chain(cert_dir / "cert.pem", cert_dir / "key.pem")
        log.info(f"TLS enabled (cert={cert_dir / 'cert.pem'})")

    _server_config["mode"] = args.mode
    _server_config["rate"] = args.rate
    _server_config["stress_duration"] = args.stress_duration
    _server_config["loop_stress"] = args.loop

    stop = asyncio.Event()
    loop = asyncio.get_running_loop()
    for sig_name in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(sig_name, stop.set)

    async with serve(
        handler, args.bind, args.port,
        ssl=ssl_ctx,
        ping_interval=20,
        ping_timeout=60,
        max_size=2**20,
        compression=None,
    ) as server:
        scheme = "wss" if args.tls else "ws"
        log.info(f"Mock server on {scheme}://{args.bind}:{args.port}")
        log.info(f"  Mode: {args.mode}")
        if args.mode == "stress":
            log.info(f"  Stress cycle: {args.stress_duration}s, loop={args.loop}")
            log.info(f"  Phases: calm(40%) → ramp(10%) → burst(6%) "
                     f"→ volatile(14%) → cooldown(30%)")
            burst_dur = int(args.stress_duration * 0.06)
            log.info(f"  Burst window: ~{burst_dur}s @ 5000-14000 msg/s/sym, TCP batched")
            log.info(f"  Calibrated to real Binance: avg ~1100 msg/s, ~3.4 frames/burst")
        else:
            log.info(f"  Constant rate: {args.rate} msg/s")
        log.info(f"  PID: {os.getpid()}")
        await stop.wait()
        log.info("Shutting down...")


def main():
    parser = argparse.ArgumentParser(
        description="Mock WebSocket server with 5-phase market stress simulation")
    parser.add_argument("--bind", default="0.0.0.0", help="Bind address")
    parser.add_argument("--port", type=int, default=9443, help="Listen port")
    parser.add_argument("--tls", action="store_true", help="Enable TLS")
    parser.add_argument("--no-tls", action="store_true", help="Disable TLS")
    parser.add_argument("--mode", choices=["stress", "constant"], default="stress",
                        help="Market data mode (default: stress)")
    parser.add_argument("--rate", type=int, default=200,
                        help="Constant mode: messages/sec (ignored in stress mode)")
    parser.add_argument("--stress-duration", type=int, default=210,
                        help="Stress mode: full cycle duration in seconds")
    parser.add_argument("--loop", action="store_true", default=True,
                        help="Loop stress cycles (default: true)")
    parser.add_argument("--no-loop", action="store_true",
                        help="Run one stress cycle then hold connection")
    args = parser.parse_args()

    if args.no_tls:
        args.tls = False
    elif not args.tls:
        args.tls = Path(__file__).parent.joinpath("cert.pem").exists()

    if args.no_loop:
        args.loop = False

    asyncio.run(run_server(args))


if __name__ == "__main__":
    main()
