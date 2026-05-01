# eph latency benchmarks

End-to-end latency measurements for the `Stream` / `Poller` API across seven
scenarios, served by a single C++23 mock binary (`benchmarks/mockex/mockex`)
and one `lat_<scenario>[_dpdk]` client binary per scenario. See
`benchmarks/mockex/README.md` for the mock internals, refit workflow, and
real-vs-mock validation loop.

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

Every parameter lives in [`config.toml`](./config.toml). Both the C++ client
and the C++ mockex binary read this single file. Layout:

```toml
# Machine-level layout (shared between all scenarios + mockex)
[networking]
nic_a      = "ens34"
nic_b      = "ens35"
server_ip  = "172.31.47.238"
client_ip  = "172.31.38.174"

[measurement]
warmup_samples = 1000

# Per-scenario subtable
[scenarios.lat_tcp]
port             = 20000
payload_size     = 256
duration_seconds = 300
```

Every scenario binary reads `[scenarios.lat_<name>]` plus the globals.
Override the config file path per-run with `BENCH_CONFIG=/path/to/other.toml`
or `--config /path/to/other.toml`, or edit `config.toml` in place.

To change TCP payload size to 1024 bytes:

```toml
[scenarios.lat_tcp]
payload_size = 1024
```

## Mock design

The mock is the C++23 binary `benchmarks/mockex/mockex`, header-only
per-scenario handlers plus a single `src/main.cpp`. Every scenario reads
`[scenarios.lat_<name>]` from `config.toml` (same parser as the client) and exposes
itself through `mockex --scenario <name>`. See
`benchmarks/mockex/README.md` for the full design — the short version:

- **TCP / UDP / WebSocket echo** — `eph::net::posix` helpers bind + accept,
  then a recv/stamp/send inner loop overwrites `[8:16]` / `[16:24]` with
  the mock's `t_mock_recv` / `t_mock_send` before echoing.
- **Exchange order echo** — same WS framing, splices `"t_mock_recv"` /
  `"t_mock_send"` fields into the client's JSON.
- **Exchange market push** — two-state MMPP-2 arrival sampler (fitted
  offline from a real Binance capture) + rotating pool of real `bookTicker`
  JSON frames with the `"T"` field patched in place per send.
- **Shared clock** — both mock and client read `bench::monotonic_raw_ns()`
  (CLOCK_MONOTONIC_RAW). Same epoch, same toolchain, no cross-language jitter.
- **Shared config parser** — `bench::ScenarioConfig` reads the same
  `config.toml` the client reads.

Rate is now governed by the MMPP-2 parameters in each scenario's
`mockex_params` INI — see `benchmarks/mockex/README.md` for the refit
workflow and K-S validation loop against real exchange captures.

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

```bash
./build/linux/arm64/release/mockex \
    --scenario tcp \
    --config benchmarks/latency/config.toml
```

The mock logs progress to stderr via spdlog and can be killed with
`Ctrl-C`. `mockex --help` prints the full scenario list.

## Dependencies

- GCC ≥ 13 or Clang ≥ 17 (C++23 / `std::expected` / `std::format`)
- `aws-lc` (already required by `eph-net` for TLS)
- For `--dpdk` variants: `vfio-pci` module loaded, hugepages reserved, NIC-B
  available for unbind — see [`../../docs/dpdk-setup.md`](../../docs/dpdk-setup.md)
- For the offline mockex refit tools only (`benchmarks/mockex/tools/`):
  Python 3.9+ with `websockets` (for `capture_binance.py`). `fit_mmpp.py` and
  `ks_validate.py` are stdlib-only.

## Project layout

```
benchmarks/latency/
├── config.toml          single source of tuning knobs
├── lat                  dispatcher script (NIC state + exec binary)
├── xmake.lua            auto-globs lat_*.cpp into kernel + _dpdk targets
├── core/
│   ├── config.hpp       BenchConfig + ScenarioConfig TOML parser
│   ├── measurement.hpp  monotonic_raw_ns, signal handler, print_report
│   └── json_scan.hpp    minimal JSON field scanner (no eph-json dep)
├── (see ../mockex/ for the mock binary and its fixtures/tools)
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
