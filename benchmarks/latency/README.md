# benchmarks/latency

Latency benchmark suite for the ephemeral_dev networking stack. Measures
RTT (and 1-leg oneway) for TCP, UDP, WebSocket, and exchange-flavoured
scenarios across two transports:

- **kernel** — POSIX sockets, client lives inside `bench_ns`
- **dpdk**   — `eph-dpdk` user-space PMD bound to `vfio-pci` on NIC-B

The mock server is **always kernel** and always runs on NIC-A. Only the
client transport differs between the two builds, so the comparison is
symmetric: the server-side TSC stamp that dominates the measurement is
identical across kernel and DPDK runs.

## Scenarios

| Binary             | Scenario                                 | Wire         | Shape       |
|--------------------|------------------------------------------|--------------|-------------|
| `lat_tcp`          | Raw TCP echo RTT                         | TCP          | RTT sweep   |
| `lat_udp`          | Raw UDP echo RTT                         | UDP          | RTT sweep   |
| `lat_ws`           | Plain WebSocket echo RTT                 | WS over TCP  | RTT sweep   |
| `lat_ex_market`    | Exchange bookTicker push (1-leg)         | WS           | oneway      |
| `lat_ex_order`     | Exchange order RTT, N-inflight pipeline  | WS           | inflight    |
| `lat_ex_md_udp`    | Exchange market-data UDP echo            | UDP          | RTT sweep   |

Each scenario source file `lat_<name>.cpp` produces two targets:
`lat_<name>` (kernel client) and `lat_<name>_dpdk` (DPDK client, same
source compiled with `EPH_USE_DPDK=1`).

## Quick start

Every binary needs root and a two-NIC host configured via `bench.conf`.
The one-command runner handles NIC-B state transitions (host kernel ↔
`bench_ns` ↔ `vfio-pci`) automatically:

```bash
# Build
xmake build lat_tcp                          # single kernel target
xmake build lat_tcp_dpdk                     # DPDK variant

# Run (from repo root or this directory — `lat` finds both)
sudo ./benchmarks/latency/lat tcp            # kernel TCP RTT
sudo ./benchmarks/latency/lat udp --dpdk     # DPDK UDP RTT
sudo ./benchmarks/latency/lat ex_market      # WS bookTicker push (kernel)
```

First `--dpdk` run calls `eph-dpdk/scripts/dpdk-setup.sh` to bind NIC-B
to `vfio-pci`; the next non-DPDK run calls `dpdk-teardown.sh` to hand
the NIC back to the kernel. Same-mode reruns take a fast path with no
state transition.

### Prerequisites

- Linux with `CAP_SYS_ADMIN` (run as root)
- `xmake` + C++23 toolchain (GCC 14 / Clang 18+)
- Two NICs on distinct PCI devices, one designated for the mock
  (`NIC_A`) and one for the client (`NIC_B`)
- For DPDK: `vfio-pci` loaded, hugepages configured, `dpdk-devbind.py`
  available via `eph-dpdk/scripts/`
- TSC must be invariant (`constant_tsc`, `nonstop_tsc`); the binaries
  calibrate on startup and refuse to run on an un-calibrated host

## Configuration

All tuning lives in [`bench.conf`](./bench.conf). Each binary reads it
directly via `bench::load_bench_conf()`; there are no command-line flags
other than the `--dpdk` toggle on the runner script. Required keys:

- `NIC_A`, `NIC_B`, `SERVER_IP`, `LOCAL_IP`, `GATEWAY_IP`
- `CLIENT_CPU`, `MOCK_CPU` (CPU pinning, typically `isolcpus`)
- Payload sweep lists: `TCP_PAYLOADS`, `UDP_PAYLOADS`, `WS_PAYLOADS`,
  `MD_UDP_PAYLOADS`, `INFLIGHTS`
- Exchange mock tuning: `SYMBOLS`, `BOOKTICKER_US`, `DEPTH_MS`,
  `TRADE_MEAN_MS`, `KLINE_S`, `DEPTH_BYTES`

Override the config file path per-run with `BENCH_CONFIG=/path/to/other.conf`.

## Project layout

```
benchmarks/latency/
├── bench.conf             single source of tuning knobs
├── lat                    runner script (NIC state machine + exec binary)
├── xmake.lua              generates lat_<name>[_dpdk] per scenario source
├── core/                  header-only shared library
│   ├── config.hpp         BenchConfig + load_bench_conf
│   ├── runner.hpp         BenchRunner (warmup → measurement → report)
│   ├── sample.hpp         RttSample / OneWaySample
│   ├── scenario_concept.hpp  RttScenario / OneWayScenario concepts
│   ├── signal.hpp         SIGINT/SIGTERM handlers + g_running
│   ├── socket_bind.hpp    POSIX bind/listen/accept helpers
│   ├── netns.hpp          setns(2) wrapper for bench_ns entry
│   ├── tsc_protocol.hpp   24-byte binary header + JSON T/T_recv/T_send
│   ├── dpdk_env.hpp       DpdkBenchEnv (EAL + Platform + ARP)
│   ├── stream_scheduler.hpp   priority-queue for multi-stream mocks
│   ├── udp_client.hpp     POSIX UDP client helper
│   ├── ws_client.hpp      client-side WS handshake + frame reader
│   ├── ws_framing.hpp     RFC 6455 masked/unmasked frame build/parse
│   └── ws_handshake.hpp   server-side WS Upgrade (bundled SHA-1)
├── tcp/lat_tcp.cpp        raw TCP RTT scenario + mock
├── udp/lat_udp.cpp        raw UDP RTT scenario + mock
├── ws/lat_ws.cpp          plain WS RTT scenario + mock
└── exchange/
    ├── lat_ex_market.cpp  bookTicker push 1-leg scenario
    ├── lat_ex_order.cpp   order RTT N-inflight scenario
    ├── lat_ex_md_udp.cpp  UDP market-data RTT scenario
    ├── mock_ws.hpp        shared exchange WS mock (4 streams × symbols)
    └── mock_md_udp.hpp    shared exchange UDP mock
```

## How a run works

1. `lat <scenario>` reads `bench.conf`, detects the current NIC-B state,
   and drives it to `bench_ns` (kernel) or `vfio-pci` (DPDK).
2. It execs `lat_<scenario>[_dpdk]` which:
   - Installs signal handlers and calibrates TSC
   - Reloads `bench.conf`, calls `fork()`
   - **Child**: stays in the host ns, runs the kernel mock pinned to
     `MOCK_CPU`, binds `SERVER_IP` on NIC-A
   - **Parent**: kernel build enters `bench_ns` via `setns(2)`; DPDK
     build stays in the host ns and brings up `DpdkBenchEnv` (EAL →
     Platform → MAC lookup → ARP gateway resolve)
3. The parent drives `BenchRunner::run_rtt_sweep` /
   `run_rtt_inflight_sweep` / `run_oneway`. Each window is warmup then
   measurement, feeding samples into four `eph::utils::Recorder`
   instances (RTT, TX, RX, SRV legs).
4. At end of window the runner logs `== BENCH <label> payload=NB ==`
   and four rows of min/p50/p99/p999/max.
5. On exit, the parent `SIGTERM`s the child and `waitpid`s.

The bench intentionally **writes no files**. All output is spdlog to
stderr; capture with shell redirection if you want to keep it.

## Fairness note

Because the mock is identical between kernel and DPDK runs, and the
server stamps `T_recv` / `T_send` inside the mock, the SRV leg is a
baseline that both transports share. TX and RX leg deltas (`c_send →
s_recv` and `s_send → c_recv`) are what actually change when you flip
to DPDK, and they isolate the client-side transport cost.

## See also

- [`docs/ONBOARDING.md`](./docs/ONBOARDING.md) — first-run walkthrough
- [`summary.md`](./summary.md)                 — architecture / module map
- [`CHANGELOG.md`](./CHANGELOG.md)             — history since the
  simplify plan started
- [`../../eph-dpdk/scripts/dpdk-setup.sh`](../../eph-dpdk/scripts/dpdk-setup.sh)
  — NIC binding script the runner delegates to
