# benchmarks/latency/mocks

Python mock servers for the latency benchmark scenarios. Each
`lat_<scenario>` binary forks its own mock at startup; these scripts
are what get forked.

## Why Python (stdlib only)

Per plan D-3 the mocks are written in Python 3.8+ using only the
standard library — no `pip install`, no `websockets`, no `aiohttp`,
no `asyncio`. A blocking socket loop is simpler, more portable, and
easier to reason about for the rates the bench actually uses.

Deliberate consequence: the push-scenario mocks
(`ex_market_push.py`, `ex_md_udp_push.py`) top out around **~200 kHz
sustained** on modern x86_64. If you need higher push rates, replace
the mock with a C equivalent — the bench client stays the same.

## Shared helpers (underscore-prefixed)

| File        | Purpose                                                      |
|-------------|--------------------------------------------------------------|
| `_conf.py`  | Minimal INI parser: globals + `[section]` blocks.            |
| `_clock.py` | `clock_gettime(CLOCK_MONOTONIC_RAW)` via ctypes → ns int.    |
| `_rate.py`  | Busy-spin `RateLimiter(hz)` for sub-ms slot scheduling.      |
| `_ws.py`    | RFC 6455 server handshake + binary frame encode/decode.      |

The clock helper matches C++ `bench::monotonic_raw_ns()`, so any
timestamp the mock stamps into a push payload is in the *same* clock
domain as the bench client's `TSC → monotonic_raw` reading.

## Scenario mocks

| Script                 | Scenario         | Section           | Role                               |
|------------------------|------------------|-------------------|------------------------------------|
| `tcp_echo.py`          | `lat_tcp`        | `[lat_tcp]`       | TCP byte-echo                      |
| `udp_echo.py`          | `lat_udp`        | `[lat_udp]`       | UDP datagram echo                  |
| `ws_echo.py`           | `lat_ws`         | `[lat_ws]`        | WS binary frame echo               |
| `ex_order_echo.py`     | `lat_ex_order`   | `[lat_ex_order]`  | WS JSON passthrough echo           |
| `ex_market_push.py`    | `lat_ex_market`  | `[lat_ex_market]` | WS `bookTicker` JSON push with `T` |
| `ex_md_udp_push.py`    | `lat_ex_md_udp`  | `[lat_ex_md_udp]` | UDP Mold64-format push             |

All scripts share the same command-line convention:

```
python3 <script>.py --config benchmarks/latency/bench.conf
```

Optional overrides: `--host`/`--port` (echo scripts), `--dest-ip` /
`--dest-port` (`ex_md_udp_push`). These exist mainly to make
standalone smoke-testing ergonomic — in the bench harness the
scripts just read `bench.conf`.

## Launch standalone (debugging)

```bash
# TCP echo
python3 benchmarks/latency/mocks/tcp_echo.py \
    --config benchmarks/latency/bench.conf

# WS market-data push (stamps "T":<monotonic_raw_ns> at send time)
python3 benchmarks/latency/mocks/ex_market_push.py \
    --config benchmarks/latency/bench.conf

# UDP market-data push (destination = client_ip:port from bench.conf)
python3 benchmarks/latency/mocks/ex_md_udp_push.py \
    --config benchmarks/latency/bench.conf
```

If the configured `mock_ip` is not bound on the current host
(typical on a dev box without the 2-NIC layout), use `--host
127.0.0.1` + the matching bench client override for smoke tests.

## Rate limits at a glance

| Mock                  | Practical ceiling (stdlib Python) |
|-----------------------|------------------------------------|
| `tcp_echo.py`         | bound by RTT, not by Python        |
| `udp_echo.py`         | bound by RTT, not by Python        |
| `ws_echo.py`          | ~250 kHz                           |
| `ex_order_echo.py`    | ~250 kHz                           |
| `ex_market_push.py`   | **~200 kHz sustained**             |
| `ex_md_udp_push.py`   | **~200 kHz sustained**             |

Per plan D-7 this is why `push_rate_hz` in `[lat_ex_market]` /
`[lat_ex_md_udp]` defaults to 100 kHz — comfortably inside the
safe envelope. For higher sustained rates, swap the mock for a C
implementation (the bench client does not need to change).

## Signal handling

Each script has `try/except KeyboardInterrupt` around `main()`, so
both SIGINT (Ctrl-C) and SIGTERM from the parent harness terminate
the mock cleanly and release the listening socket (SO_REUSEADDR is
set anyway, so immediate restart is always safe).

## Timestamp semantics for push scenarios

Both push mocks call `monotonic_raw_ns()` **at the moment of send**,
not at the intended rate-limiter slot. Per D-6, stamping at the
actual send instant makes rate-limiter jitter appear as
measurement noise rather than a systematic bias in one-way latency
measurements — which is what lets a Python mock produce meaningful
numbers at all.
