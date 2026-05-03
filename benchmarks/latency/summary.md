# Project: benchmarks/latency

> Latency benchmark suite for the ephemeral_dev networking stack —
> measures kernel-vs-DPDK client transport cost against a shared kernel
> mock, across TCP / UDP / WebSocket / exchange scenarios.

**Language**: C++23 | **Build**: xmake (sub-project)

---

## Table of Contents
1. [Overview](#overview)
2. [Architecture](#architecture)
3. [Module Map](#module-map)
4. [Data Flow](#data-flow)
5. [Key Components](#key-components)
6. [Entry Points & APIs](#entry-points--apis)
7. [Dependencies](#dependencies)
8. [Testing](#testing)

---

## Overview

`benchmarks/latency` is a micro-benchmark suite that measures the
end-to-end latency cost of the `eph` networking libraries across six
transport scenarios. Each scenario answers the same question — "how
long does it take this client to bounce a message off an equivalent
kernel server?" — for two distinct transports: the host kernel POSIX
socket stack and the in-house DPDK PMD (`eph-dpdk`).

The suite's key design decision is that the mock server is **always**
kernel. A scenario's DPDK variant only swaps the *client* transport;
the mock remains a POSIX socket server in the host network namespace.
This keeps the kernel-vs-DPDK comparison fair: because the server
stamps `T_recv` and `T_send` using the same `eph::utils::TSC` clock in
both cases, the SRV leg (server-side work) is a shared baseline, and
any measurable difference between runs concentrates in the TX/RX legs
where the client transport actually differs.

Each scenario is a single self-contained `lat_<name>.cpp` translation
unit. `xmake.lua` discovers them all with a glob and emits two targets
per file — `lat_<name>` (kernel client) and `lat_<name>_dpdk` (same
source, `EPH_USE_DPDK=1`). The runner script `scripts/lat` drives
NIC-B between `bench_ns` (kernel) and `vfio-pci` (DPDK) states
idempotently and then execs the right binary. The binary forks, runs
the mock in the child, and drives the client in the parent. Nothing
is written to disk; all output is spdlog to stderr.

---

## Architecture

The design is a two-process, three-namespace layout driven by a shared
`BenchRunner`:

- **Host ns**: mock server (always POSIX sockets on NIC-A).
- **bench_ns** (kernel runs): client socket lives here on NIC-B.
- **host ns + DPDK PMD** (DPDK runs): client stays in the host ns but
  NIC-B is bound to `vfio-pci` so only the DPDK application can touch
  it.

The `core/` layer is header-only so every scenario binary pulls in only
the code it actually uses. All scenario glue — `main`, `fork`, `setns`,
mock loop, client scenario class, `BenchRunner` invocation — lives in
the single `lat_<name>.cpp` file.

### Component Diagram

```
 +--------------------------------------------------------------+
 |                       scripts/lat                            |
 |  reads bench.conf, drives NIC-B state (host<->ns<->vfio),    |
 |  execs lat_<scenario>[_dpdk]                                 |
 +----------------------------+---------------------------------+
                              | exec()
                 +------------v-----------+
                 |  lat_<scenario>[_dpdk] |
                 |   load_bench_conf()    |
                 |   TSC::init()          |
                 |   install_signal...    |
                 |          fork()        |
                 +----+--------------+----+
                      |              |
              +-------v------+  +----v---------+
              | child: mock  |  | parent: bench|
              | (host ns,    |  | client (ns or|
              |  POSIX)      |  |  DPDK PMD)   |
              +------+-------+  +--+-----------+
                     |             |
                     |   network   |
                     | <---------> |
                     |   NIC-A/B   |
                     |             |
                     |         +---v--------------+
                     |         |   BenchRunner    |
                     |         |  warmup          |
                     |         |  measurement     |
                     |         |  4 legs Recorder |
                     |         |  report          |
                     |         +------------------+
```

---

## Module Map

| Module / File                         | Responsibility                                                  | Key Types                                | Depends On                   |
|---------------------------------------|-----------------------------------------------------------------|------------------------------------------|------------------------------|
| `xmake.lua`                           | Discover `**/lat_*.cpp`, emit kernel + DPDK targets             | -                                        | `eph-utils`, `eph-net-dpdk`  |
| `bench.conf`                          | Single source of tuning (NIC, CPU, sweeps)                      | -                                        | -                            |
| `lat` (bash)                          | NIC-B state machine + exec binary                               | -                                        | `eph-net-dpdk/scripts/dpdk-*.sh` |
| `core/config.hpp`                     | Parse `bench.conf`; also legacy `CommonConfig` CLI              | `BenchConfig`, `load_bench_conf`         | `<expected>`                 |
| `core/runner.hpp`                     | Warmup -> measurement -> report orchestration                   | `BenchRunner`                            | `eph::utils::{Recorder,PhasedTimer}` |
| `core/sample.hpp`                     | Latency sample structs                                          | `RttSample`, `OneWaySample`              | -                            |
| `core/scenario_concept.hpp`           | Compile-time contracts for scenario classes                     | `RttScenario`, `OneWayScenario`          | `<concepts>`                 |
| `core/signal.hpp`                     | SIGINT/SIGTERM handlers + shared shutdown flag                  | `g_running`, `install_signal_handlers`   | -                            |
| `core/netns.hpp`                      | `setns(2)` wrapper for `bench_ns` entry                         | `enter_netns`                            | `<sched.h>`                  |
| `core/socket_bind.hpp`                | POSIX bind/listen/accept helpers for mocks                      | `tcp_bind_listen`, `udp_bind`, `accept_one` | `<sys/socket.h>`           |
| `core/tsc_protocol.hpp`               | 24-B binary header + JSON `T`/`T_recv`/`T_send`                 | `stamp_binary`, `parse_T*`               | `eph::utils::TSC`            |
| `core/dpdk_env.hpp`                   | DPDK bootstrap (EAL + Platform + ARP)                           | `DpdkBenchEnv`                           | `eph::dpdk::*` (guarded)     |
| `core/stream_scheduler.hpp`           | Priority-queue multi-stream dispatch                            | `StreamScheduler`, `StreamEntry`         | `eph::utils::TSC`            |
| `core/ws_client.hpp`                  | Client-side WS handshake + frame reader (POSIX + DPDK)          | `connect_ws`, `recv_one_frame`, `dpdk_ws_handshake` | `ws_framing.hpp`, `eph::dpdk::tcp` |
| `core/ws_framing.hpp`                 | RFC 6455 masked/unmasked frame build/parse                      | `build_masked_text_frame`, `parse_client_frame_inplace` | - |
| `core/ws_handshake.hpp`               | Server-side WS Upgrade + bundled SHA-1                          | `ws_server_handshake`                    | `eph::core::detail::base64`  |
| `tcp/lat_tcp.cpp`                     | Raw TCP RTT scenario + POSIX mock                               | `TcpRttScenario`, `TcpDpdkRttScenario`   | core/*                       |
| `udp/lat_udp.cpp`                     | Raw UDP RTT scenario + POSIX mock                               | `UdpRttScenario`, `UdpDpdkRttScenario`   | core/*                       |
| `ws/lat_ws.cpp`                       | Plain WS RTT scenario + POSIX mock                              | `WsRttScenario` (+ DPDK variant)         | core/*                       |
| `exchange/lat_ex_market.cpp`          | Bench WS bookTicker push (1-leg oneway)                         | `MarketRxScenario`                       | core/*, `mock_ws.hpp`        |
| `exchange/lat_ex_order.cpp`           | Exchange order RTT, N-inflight pipeline                         | `OrderRttScenario`                       | core/*, `mock_ws.hpp`        |
| `exchange/lat_ex_md_udp.cpp`          | Exchange UDP market-data echo RTT                               | `MdUdpRttScenario`                       | core/*, `mock_md_udp.hpp`    |
| `exchange/mock_ws.hpp`                | Shared exchange WS mock (4 streams per symbol)                  | `run_exchange_ws_mock`                   | `stream_scheduler.hpp`       |
| `exchange/mock_md_udp.hpp`            | Shared exchange UDP market-data echo mock                       | `run_exchange_md_udp_mock`               | `socket_bind.hpp`            |

---

## Data Flow

A measurement window for `lat_tcp` (kernel) looks like this:

1. Client opens TCP to server, sends a 4-byte little-endian payload
   length. The mock reads the header and enters its echo loop.
2. Client stamps `client_send_tsc` into bytes 0..8 of the send buffer,
   sends `payload` bytes.
3. Mock `recv_exact`s, stamps `server_recv_tsc` into bytes 8..16,
   spins for `SERVER_WORK_NS` ns, stamps `server_send_tsc` into bytes
   16..24, sends the same buffer back.
4. Client `recv_exact`s, stamps `client_recv_tsc`. Now the `RttSample`
   has all four TSCs.
5. The scenario loop in
   `benchmarks/latency/scenarios/lat_<name>_loop.hpp` computes
   per-leg deltas via `bench::compute_legs()` and feeds them into
   the three `eph::utils::Recorder` instances (`rec_rtt`, `rec_tx`,
   `rec_rx`).
6. After `duration_seconds` elapses, the loop calls
   `bench::print_leg_report()` (per-leg p50/p99/p999/max via spdlog)
   and `bench::export_legs()` (one `_rtt`/`_tx`/`_rx` JSON each).

UDP, WS, and exchange scenarios are variations on this theme with
different wire formats (JSON vs binary header) and different sweep
axes (payload size vs inflight count vs 1-leg).

### Flow Diagram

```
 client                                      mock
 ------                                      ----
 stamp client_send_tsc             +------->  recv
 write hdr[0:8] = client_send_tsc  |          stamp server_recv_tsc
 send(buf, msg_size)    -----------+          write hdr[8:16]
                                              spin_for_ns(WORK_NS)
                                              stamp server_send_tsc
                                              write hdr[16:24]
 recv(buf, msg_size)    <-------------------- send(buf, msg_size)
 stamp client_recv_tsc
 read server_recv_tsc, server_send_tsc
        |
        v
  BenchRunner::record_rtt
        |
        +-> rtt_.record(client_recv - client_send)
        +-> tx_ .record(server_recv - client_send)
        +-> rx_ .record(client_recv - server_send)
        +-> srv_.record(server_send - server_recv)
```

---

## Key Components

### `BenchRunner`

**File**: `core/runner.hpp`
**Purpose**: Drive every scenario through the same warmup -> measurement
-> report skeleton. Owns five `eph::utils::Recorder` instances (4 RTT
legs + 1 oneway).
**Interface**:
```cpp
BenchRunner(const BenchConfig& cfg, string_view scenario, string_view transport);
template <RttScenario S>    void run_rtt_sweep(S&, span<const size_t>);
template <RttScenario S>    void run_rtt_inflight_sweep(S&, span<const int>);
template <OneWayScenario S> void run_oneway(S&);
```
**Notes**: dispatch is templated on the scenario concept, so the hot
loop inlines `do_one_rtt` / `do_one_recv` with no virtual call. The
runner polls `g_running` with relaxed ordering; that's safe because
there's no other shared state to synchronize against the flag.

### `BenchConfig` + `load_bench_conf()`

**File**: `core/config.hpp`
**Purpose**: Read `bench.conf` (bash KEY=VALUE) into a single struct.
Replaces the legacy `CommonConfig` + `parse_common` CLI path.
**Interface**:
```cpp
struct BenchConfig { /* NIC, CPUs, sweeps, exchange mock tuning */ };
[[nodiscard]] std::expected<BenchConfig, std::string> load_bench_conf();
```
**Notes**: lookup order is `$BENCH_CONFIG` -> `./bench.conf` -> walk up
from `/proc/self/exe` to `<project>/benchmarks/latency/bench.conf`.
Required fields: `NIC_B`, `SERVER_IP`, `LOCAL_IP`, `GATEWAY_IP`,
`CLIENT_CPU`, `MOCK_CPU`. Unknown keys are silently ignored so older
binaries tolerate newer config files.

### `DpdkBenchEnv`

**File**: `core/dpdk_env.hpp`
**Purpose**: One-shot bootstrap for every DPDK client. Runs EAL init,
`Platform::create` (port + mempool + queues), parses IPs, grabs the
local MAC via `rte_eth_macaddr_get`, and ARP-resolves the gateway MAC.
**Notes**: move-only; guards everything with `#ifdef EPH_USE_DPDK`, so
kernel binaries pay no compile-time cost.

### `StreamScheduler`

**File**: `core/stream_scheduler.hpp`
**Purpose**: Priority-queue multi-stream dispatch for the exchange WS
mock. Each stream has its own period (or Poisson mean) and an `emit`
callback; the scheduler fires at most one stream per `tick()`.
**Notes**: periods are converted to TSC cycles at registration time so
the hot path does no division. Not thread-safe - mocks are
single-threaded.

### `ws_framing` helpers

**File**: `core/ws_framing.hpp`
**Purpose**: RFC 6455 client/server frame build + parse. Client frames
are masked (bench side); server frames are unmasked (mock side).
`parse_client_frame_inplace` unmasks the payload in place to keep the
hot path allocation-free.
**Notes**: the exchange mock's receive loop uses the streaming
`parse_client_frame_inplace` + a persistent `rx` buffer so partial
frames and batched client sends both work.

### `tsc_protocol`

**File**: `core/tsc_protocol.hpp`
**Purpose**: Define the two on-wire TSC encodings the bench uses. Raw
TCP/UDP scenarios use a 24-byte binary header at the start of every
message; WebSocket scenarios embed `T`, `T_recv`, `T_send` JSON fields.
The helpers `parse_T/T_recv/T_send` are designed for hot-path use
(linear scan, no allocation).

### scenario files (`lat_*.cpp`)

**Files**: `tcp/lat_tcp.cpp`, `udp/lat_udp.cpp`, `ws/lat_ws.cpp`,
`exchange/lat_ex_*.cpp`.
**Purpose**: One self-contained binary per scenario. Each contains
`main` + `mock_fn::run` (child) + `client_fn::run` (parent) + one or
two scenario classes (`RttScenario` / `OneWayScenario`). The kernel
variant uses POSIX sockets and enters `bench_ns`; the DPDK variant
(same file, `EPH_USE_DPDK=1`) calls `DpdkBenchEnv::create_full` and
runs in the host namespace.

### `scripts/lat`

**File**: `lat` (bash, at the subproject root)
**Purpose**: Single-command runner. Parses args, loads `bench.conf`,
detects NIC-B current state (`host` / `bench_ns` / `dpdk`), drives it
toward the desired state via idempotent transitions (deleting
`bench_ns` and unbinding from `vfio-pci` as necessary), locates the
right binary under `build/linux/*/release/`, and `exec`s it.
**Notes**: deliberately fails loudly on `dpdk-setup.sh` /
`dpdk-teardown.sh` errors - silently continuing leaves NIC-B in an
inconsistent state that the next run won't recover from.

---

## Entry Points & APIs

| Entrypoint            | Type        | Description                                                |
|-----------------------|-------------|------------------------------------------------------------|
| `scripts/lat`         | bash CLI    | User-facing runner; argument is the scenario name          |
| `lat_tcp[_dpdk]`      | binary      | Raw TCP echo RTT sweep                                     |
| `lat_udp[_dpdk]`      | binary      | Raw UDP echo RTT sweep                                     |
| `lat_ws[_dpdk]`       | binary      | Plain WebSocket echo RTT sweep                             |
| `lat_ex_market[_dpdk]`| binary      | Exchange bookTicker oneway                                 |
| `lat_ex_order[_dpdk]` | binary      | Exchange order RTT, N-inflight pipeline                    |
| `lat_ex_md_udp[_dpdk]`| binary      | Exchange UDP market-data RTT                               |
| `bench::load_bench_conf()` | C++ API | Read bench.conf into a `BenchConfig`                       |
| `bench::BenchCtx`     | C++ API     | Per-scenario bring-up context (config + Poller + DPDK env) |
| `bench::monotonic_raw_ns()` | C++ API | Shared time base for client + mockex (CLOCK_MONOTONIC_RAW) |
| `bench::export_legs()` | C++ API     | Write `_rtt`/`_tx`/`_rx` JSON files from a `Recorder`      |
| `mockex --scenario <name>` | binary | Unified kernel-side mock; dispatches to a handler in `mockex/include/mockex/scenarios/<name>.hpp` per `kScenarioTable` |

All public APIs are header-only under `core/`.

---

## Dependencies

### Internal (module graph)

```
              +---- tcp/lat_tcp.cpp -----+
              |                          |
              +---- udp/lat_udp.cpp -----+
              |                          |
              +---- ws/lat_ws.cpp -------+
              |                          |      +-- eph-utils (Recorder,
              +-- exchange/lat_ex_*.cpp -+      |    PhasedTimer, TSC,
              |                          |      |    cpu_pin, spin_for_ns)
              |                          v      |
              |                        core/ ---+
              |                                 |
              |                                 +-- eph-net-dpdk
              |                                     (EalGuard, Platform,
              |                                      UdpSender, TcpSession,
              |                                      arp::resolve)
              |                                        [only for _dpdk builds]
              |
              +-- scripts/lat --> eph-net-dpdk/scripts/dpdk-setup.sh
                                  eph-net-dpdk/scripts/dpdk-teardown.sh
```

### External

| Package          | Purpose                                                             |
|------------------|---------------------------------------------------------------------|
| `spdlog`         | Structured logging for every bench binary                           |
| `libdpdk` (sys)  | Underlying DPDK runtime (EAL, ethdev, mempool) for `_dpdk` variants |
| `aws-lc`         | TLS crypto (pulled in transitively by sibling libs, not this suite) |

---

## Testing

The latency suite has unit tests for its core building blocks under
`tests/unit/bench/`, plus end-to-end validation by running the
binaries themselves. Coverage:

| Component                     | Coverage                                                        |
|-------------------------------|-----------------------------------------------------------------|
| `core/config.hpp` (load_bench_conf) | `tests/unit/bench/test_load_bench_conf.cpp` (215 LOC)     |
| `core/stream_scheduler.hpp`   | `tests/unit/bench/test_stream_scheduler.cpp` (88 LOC)           |
| `core/tsc_protocol.hpp`       | `tests/unit/bench/test_tsc_protocol.cpp` (93 LOC)               |
| `core/ws_framing.hpp`         | `tests/unit/bench/test_ws_frame.cpp` (131 LOC)                  |
| Scenario concepts             | `requires` clauses in `scenario_concept.hpp` (compile-time)     |
| TSC calibration               | `eph::utils::TSC::init()` fail-fast at bench startup            |
| End-to-end mock + client      | Manually via `./benchmarks/latency/lat <scenario>`              |
| `lat` script state machine    | Verified manually; auto-recovery from wedged NIC tested live    |

**Untested core/** (no unit tests, only e2e validation):

- `core/runner.hpp` (BenchRunner sweep loops)
- `core/socket_bind.hpp` (poll/accept retry)
- `core/dpdk_env.hpp` (EAL + Platform + ARP bootstrap)
- `core/netns.hpp` (`setns(2)` wrapper)
- `core/signal.hpp` (SIGINT/SIGTERM handlers — trivial)
- `core/sample.hpp` (POD types — no logic to test)
- `core/ws_handshake.hpp` (server-side Sec-WebSocket-Accept)
- `core/ws_client.hpp` (client-side WS handshake)

Key sanity scenarios when running by hand:

- A kernel run followed immediately by a DPDK run in the same shell
  should produce one NIC-B transition (bench_ns → vfio-pci) and an
  announced transition log line.
- All six scenarios should print non-empty per-leg histograms — if any
  leg has `n=0`, the scenario's TSC stamping is broken.
- Swapping `SERVER_WORK_NS=200 → 1000` should widen the SRV leg by
  ~800 ns while leaving TX / RX roughly unchanged — sanity check that
  server-side timing is isolated from transport timing.
- A re-run in the same mode (`./lat tcp` twice in a row) should print
  `bench_ns,bench_ns` and skip every transition step (idempotency).

The suite deliberately writes no files; capture spdlog output with
shell redirection when persistence is wanted.
