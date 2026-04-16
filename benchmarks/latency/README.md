# eph latency benchmarks

End-to-end latency measurements for the `Stream` / `Poller` API across six
scenarios, using Python stdlib mocks and a single `lat_<scenario>[_dpdk]` binary
per scenario.

The mock always runs in kernel. Only the client side differs between the
`kernel` and `dpdk` builds, which is what makes the kernel-vs-DPDK comparison
fair — any latency delta is due to the client-side protocol stack, not the mock
implementation.

## Quick start

```bash
# Kernel client (default)
sudo ./benchmarks/latency/lat tcp            # raw TCP echo RTT
sudo ./benchmarks/latency/lat udp            # raw UDP echo RTT
sudo ./benchmarks/latency/lat ws             # plain WebSocket echo RTT
sudo ./benchmarks/latency/lat ex_market      # exchange bookTicker push (1-leg)
sudo ./benchmarks/latency/lat ex_order       # pipelined order RTT
sudo ./benchmarks/latency/lat ex_md_udp      # Mold64-style UDP feed (1-leg)

# DPDK client (requires vfio-pci + hugepages + NIC pair configured)
sudo ./benchmarks/latency/lat tcp --dpdk
```

The dispatcher handles NIC-B state transitions (host kernel ↔ `bench_ns` ↔
`vfio-pci`) idempotently. First `--dpdk` run binds NIC-B to `vfio-pci`; the next
non-DPDK run hands it back. Same-mode reruns take a fast path with no
transition.

## Scenarios

| Scenario        | Measures                                   | Mock                       | Wire                 |
| --------------- | ------------------------------------------ | -------------------------- | -------------------- |
| `lat_tcp`       | TCP echo RTT                               | `tcp_echo.py` (stdlib)     | raw bytes over TCP   |
| `lat_udp`       | UDP echo RTT                               | `udp_echo.py` (stdlib)     | raw datagrams        |
| `lat_ws`        | WebSocket echo RTT                         | `ws_echo.py` (RFC 6455)    | WS binary frames     |
| `lat_ex_market` | exchange bookTicker one-way push           | `ex_market_push.py`        | JSON over WS         |
| `lat_ex_order`  | pipelined order RTT (N-inflight)           | `ex_order_echo.py`         | JSON over WS         |
| `lat_ex_md_udp` | Mold64-style market-data feed one-way      | `ex_md_udp_push.py`        | binary Mold64 / UDP  |

Each `lat_<scenario>.cpp` source compiles into two targets: `lat_<scenario>`
(kernel client) and `lat_<scenario>_dpdk` (DPDK client, same source with
`EPH_USE_DPDK=1`). Build all six via `xmake build -g benchmarks` or a single
one via `xmake build lat_tcp`.

## Configuration

Every parameter lives in [`bench.conf`](./bench.conf). Both the C++ client and
the Python mock read this single file. Layout:

```ini
# Lowercase globals (shared between all scenarios + mocks)
mock_nic       = ens34
mock_ip        = 10.0.0.1
client_nic     = ens35
client_ip      = 10.0.0.2
warmup_samples = 1000

# Per-scenario INI section
[lat_tcp]
port             = 20000
payload_size     = 256
duration_seconds = 10
```

Every scenario binary reads `[lat_<scenario>]` plus the globals. Override the
config file path per-run with `BENCH_CONFIG=/path/to/other.conf`, or edit
`bench.conf` in place.

To change TCP payload size to 1024 bytes:

```ini
[lat_tcp]
payload_size = 1024
```

## Mock design

All mocks are Python 3.8+ stdlib only — no `pip install` required.

- **TCP / UDP echo** — plain `socket`, `SO_REUSEADDR`, `TCP_NODELAY`.
- **WebSocket echo** — `mocks/_ws.py` implements the RFC 6455 handshake
  (SHA-1 + base64 via `hashlib` / `base64`) and binary frame read/write.
- **Rate-limited push** — `mocks/_rate.py` busy-loops on
  `clock_gettime(CLOCK_MONOTONIC_RAW)` (via `ctypes`) because `time.sleep` is
  too coarse at bench rates.
- **Shared clock** — `mocks/_clock.py` exposes `monotonic_raw_ns()` for Python
  mocks; the C++ client uses `bench::monotonic_raw_ns()` in
  `core/measurement.hpp`. Same clock source, same epoch — required for
  one-way scenarios (`lat_ex_market`, `lat_ex_md_udp`) where the mock stamps
  send time and the client subtracts it from receive time.
- **Shared config parser** — `mocks/_conf.py` reads the same `bench.conf`
  layout as the C++ `bench::ScenarioConfig`.

Mock push rate is capped at roughly 100-200 kHz sustained by Python stdlib
overhead. For higher rates, replace the specific push mock with a C
implementation — the `bench.conf` contract stays the same, just swap the
executable the `lat` wrapper invokes. See `.artifacts/phase-10-scope-decision.md`.

## Fairness

The mock is identical between kernel and DPDK runs. The client is the only
thing that changes. Any delta between
`sudo ./lat tcp` and `sudo ./lat tcp --dpdk` is therefore attributable to the
client-side protocol stack: kernel epoll + `sendmsg/recvmsg` vs. DPDK lcore
burst poll + `rte_eth_tx_burst/rx_burst`. Mock-side work (Python interpreter,
socket syscalls, scheduling) is a shared baseline.

See [`../../docs/latency-benchmark-fairness.md`](../../docs/latency-benchmark-fairness.md)
for the full rationale.

## Performance

The latest measured numbers are archived under
`.artifacts/phase-10-perf-results-20260411.md`. Re-run on your host with

```bash
sudo ./benchmarks/latency/lat tcp
```

and compare p50/p99 against the archived report. Use `p50` / `p99` for
comparisons — `p999` and `max` are dominated by OS jitter on non-isolated
hosts.

## Debugging a mock standalone

Every Python mock accepts `--config <path>`:

```bash
python3 benchmarks/latency/mocks/tcp_echo.py --config benchmarks/latency/bench.conf
```

The mock logs progress to stderr and can be killed with `Ctrl-C`.

## Dependencies

- Python 3.8+ (stdlib only)
- GCC ≥ 13 or Clang ≥ 17 (C++23 / `std::expected` / `std::format`)
- `aws-lc` (already required by `eph-net` for TLS)
- For `--dpdk` variants: `vfio-pci` module loaded, hugepages reserved, NIC-B
  available for unbind — see [`../../docs/dpdk-setup.md`](../../docs/dpdk-setup.md)

## Project layout

```
benchmarks/latency/
├── bench.conf           single source of tuning knobs
├── lat                  dispatcher script (NIC state + exec binary)
├── xmake.lua            auto-globs lat_*.cpp into kernel + _dpdk targets
├── core/
│   ├── config.hpp       BenchConfig + ScenarioConfig INI parser
│   ├── measurement.hpp  monotonic_raw_ns, signal handler, print_report
│   └── json_scan.hpp    minimal JSON field scanner (no eph-json dep)
├── mocks/
│   ├── _clock.py        ctypes clock_gettime(CLOCK_MONOTONIC_RAW)
│   ├── _conf.py         bench.conf INI parser
│   ├── _rate.py         busy-loop rate limiter
│   ├── _ws.py           RFC 6455 handshake + frame IO
│   ├── tcp_echo.py, udp_echo.py, ws_echo.py
│   └── ex_market_push.py, ex_order_echo.py, ex_md_udp_push.py
├── tcp/lat_tcp.cpp
├── udp/lat_udp.cpp
├── ws/lat_ws.cpp
└── exchange/
    ├── lat_ex_market.cpp
    ├── lat_ex_order.cpp
    └── lat_ex_md_udp.cpp
```

Shared bench unit tests live in `tests/unit/bench/`.

## See also

- [`../../docs/latency-benchmark-fairness.md`](../../docs/latency-benchmark-fairness.md)
  — why the kernel-vs-DPDK comparison is structured this way
- [`../../docs/dpdk-setup.md`](../../docs/dpdk-setup.md) — NIC-B / hugepages /
  vfio-pci environment
- [`../../.artifacts/phase-10-scope-decision.md`](../../.artifacts/phase-10-scope-decision.md)
  — scope decisions for the latency benchmark suite
- [`../../.artifacts/phase-10-perf-results-20260411.md`](../../.artifacts/phase-10-perf-results-20260411.md)
  — latest kernel performance numbers
