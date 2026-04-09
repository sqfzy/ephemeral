# benchmarks/latency — Developer Onboarding

A 10-minute guide to running, editing, and extending the latency
benchmark suite.

## 1. What this subproject is

Six self-contained latency benchmarks, each compiled twice (once with
POSIX sockets, once with a DPDK PMD), measuring the transport cost of
echoing a message through a kernel server. The kernel server is
deliberately shared across both builds so the server-side TSC stamp is
identical; only the client transport varies.

Scenarios live under `tcp/`, `udp/`, `ws/`, and `exchange/`. Each file
`lat_<name>.cpp` contains **everything** for that scenario: main, fork,
mock, and client. Shared infrastructure (config loading, runner,
concepts, framing, DPDK bootstrap) is header-only under `core/`.

See [`../summary.md`](../summary.md) for architecture details and
[`../README.md`](../README.md) for user-facing docs.

## 2. Environment setup

### Host requirements

- Linux with an invariant TSC (`constant_tsc`, `nonstop_tsc` in
  `/proc/cpuinfo`). The binaries calibrate at startup; without
  invariance `eph::utils::TSC::init()` fails and `main` exits.
- Two NICs on distinct PCI slots. `NIC_A` stays in the host network
  namespace (mock side); `NIC_B` is moved into `bench_ns` for kernel
  runs or bound to `vfio-pci` for DPDK runs.
- Root privileges — `setns(2)`, `rte_eal_init`, and `vfio-pci` binding
  all need `CAP_SYS_ADMIN`.
- Ideally `isolcpus=` for `CLIENT_CPU` and `MOCK_CPU`. If your host has
  no isolated cores, set `ALLOW_NON_ISOLATED=true` in `bench.conf` and
  the binaries relax their pinning policy.

### Toolchain

- `xmake` (build tool for the whole monorepo)
- GCC 14+ or Clang 18+ with C++23 (concepts, `std::expected`,
  `std::format`, structured bindings)
- `spdlog` (vcpkg / system package)
- For DPDK builds: `vcpkg::dpdk`, `aws-lc`, and the `eph-dpdk` sibling
  library

### First build

From the repo root:

```bash
xmake f --mode=release          # once
xmake build lat_tcp             # single kernel target
xmake build lat_tcp_dpdk        # corresponding DPDK target
```

The `benchmarks/latency/xmake.lua` sub-project auto-discovers every
`**/lat_*.cpp` under this directory and emits one `lat_<name>` +
`lat_<name>_dpdk` pair per source file. To rebuild everything:

```bash
for t in lat_tcp lat_udp lat_ws lat_ex_market lat_ex_order lat_ex_md_udp; do
  xmake build $t $t\_dpdk
done
```

### First run

Edit [`bench.conf`](../bench.conf) with your NIC names, IPs, gateway,
and CPU pins. Then:

```bash
sudo ./benchmarks/latency/lat tcp
```

The first run will transition NIC-B from the host ns into `bench_ns`,
start the mock in the child, exec the client in the parent, and print
latency histograms at the end of each payload window.

## 3. Daily workflow

### Editing core/ headers

`core/` is header-only and `#include`d by every scenario, so every
change triggers a full rebuild of all 12 targets. Build scope is
per-target: stick to the one scenario you care about while iterating.

### Editing a scenario

Each scenario file is independent and compiles in isolation:

```bash
xmake build lat_ws              # only rebuilds ws/lat_ws.cpp
```

The same source file also builds as `lat_ws_dpdk` with `EPH_USE_DPDK=1`
defined. The DPDK variant pulls in `core/dpdk_env.hpp` and the
`eph-dpdk` library; the kernel variant does not.

### Running one bench

```bash
# Kernel TCP
sudo ./benchmarks/latency/lat tcp

# DPDK TCP (first call also runs dpdk-setup.sh to bind NIC-B to vfio-pci)
sudo ./benchmarks/latency/lat tcp --dpdk

# Switching modes automatically transitions NIC-B state
sudo ./benchmarks/latency/lat udp        # tears DPDK down, sets up bench_ns
```

Repeat runs in the **same mode** take the fast path (no NIC transition).

### Overriding config

```bash
BENCH_CONFIG=/tmp/my.conf sudo -E ./benchmarks/latency/lat tcp
```

The runner script and all binaries honour `$BENCH_CONFIG` (absolute
path).

## 4. Common tasks

### Add a new scenario

1. Create `<dir>/lat_<name>.cpp` implementing the scenario. It must:
   - Call `bench::install_signal_handlers()` and
     `eph::utils::TSC::init()` in `main`.
   - Load config via `bench::load_bench_conf()`.
   - `fork()`; child runs the mock; parent either enters `bench_ns` or
     brings up `DpdkBenchEnv` (guarded on `EPH_USE_DPDK`).
   - Define a scenario class satisfying `bench::RttScenario` or
     `bench::OneWayScenario` (from `core/scenario_concept.hpp`).
   - Drive it through `bench::BenchRunner::run_rtt_sweep` /
     `run_rtt_inflight_sweep` / `run_oneway`.
2. Add the scenario name to the case statement and help text in
   [`lat`](../lat).
3. `xmake build lat_<name>` will pick it up automatically via the
   glob in [`xmake.lua`](../xmake.lua).

### Tweak a sweep axis

Edit the comma-separated list in `bench.conf`:

```
TCP_PAYLOADS=64,128,256,512,1024,1460,4096,16384
INFLIGHTS=1,4,16,64
```

Empty list = scenario falls back to its compiled-in
`kDefault*Payloads` constant.

### Change the measurement window

```
WARMUP=2
DURATION=10
SERVER_WORK_NS=200          # spin ns inside mock per request
```

`WARMUP`/`DURATION` are seconds. `SERVER_WORK_NS` models business
work; it widens the SRV leg without touching the TX/RX legs.

## 5. Code conventions

- **Observability**: non-trivial functions use `spdlog` with
  ERROR/WARN/INFO/DEBUG/TRACE levels. Bench startup logs at INFO; hot
  loops log nothing.
- **Error handling**: `std::expected<T, std::string>` for anything that
  can fail at startup (config load, socket bind, EAL init, ARP). Hot
  paths return `bool` or a size.
- **Comments**: every `core/` header has a `@file` comment at the top
  describing intent. Scenarios add `@file` comments too. Inline comments
  explain *why*, not *what*.
- **C++ style**: concepts over type-erasure, `std::expected` over
  exceptions, `std::span` for buffer views, `noexcept` on hot-path
  functions.
- **No files written**: bench binaries log to stderr only. Capture with
  shell redirection when you want to keep output.

## 6. Troubleshooting

| Symptom                                | Likely cause                                  | Fix                                                               |
|----------------------------------------|-----------------------------------------------|-------------------------------------------------------------------|
| `TSC calibration failed`               | Host TSC not invariant                        | Check `/proc/cpuinfo`; the bench cannot measure without it        |
| `enter_netns: open /var/run/netns/...` | `bench_ns` missing                            | Run via `scripts/lat` which creates it, not the binary directly   |
| `EAL init` errors on DPDK build        | Hugepages / `vfio-pci` missing                | `eph-dpdk/scripts/dpdk-setup.sh --check-only` to diagnose         |
| `ARP resolve gateway` timeout          | Gateway wrong or unreachable                  | Verify `GATEWAY_IP` in `bench.conf`; try `ip neigh` on host       |
| NIC-B stuck after crash                | `.dpdk_state` inconsistent with current bind  | `sudo eph-dpdk/scripts/dpdk-teardown.sh` then retry               |
| `pin failed: not isolcpus`             | `CLIENT_CPU`/`MOCK_CPU` not on isolated cores | Set `ALLOW_NON_ISOLATED=true` for dev hosts                       |
| "binary not found"                     | Target not built                              | `xmake build lat_<scenario>[_dpdk]` — `lat` tells you the command |

## 7. Where to look next

- [`../README.md`](../README.md) — 30-second overview
- [`../summary.md`](../summary.md) — architecture, module map, data flow
- [`../CHANGELOG.md`](../CHANGELOG.md) — what changed across the simplify plan
- [`../bench.conf`](../bench.conf) — annotated config template
- [`../core/runner.hpp`](../core/runner.hpp) — how a measurement window
  actually works
- [`../core/tsc_protocol.hpp`](../core/tsc_protocol.hpp) — wire format
  for TSC stamps
