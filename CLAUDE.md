# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

ephemeral (`eph`) is a header-only C++23 ultra-low-latency networking library for HFT. Ten independent modules share a concept-driven, zero-virtual-dispatch design. Two networking backends — POSIX sockets (`eph-net`) and DPDK kernel-bypass (`eph-dpdk`) — both satisfy the same `TcpTransport` concept and plug into the same `eph-transport` variants.

See `README.md` for the public-facing overview and `summary.md` for the full architecture/module map.

## Build

Build system is **xmake**. Compiler must be GCC ≥ 13 or Clang ≥ 17 (uses `std::expected`, `std::format`).

```bash
xmake -m release          # release build (set_optimize fastest)
xmake -m debug            # debug build (SPDLOG_LEVEL_TRACE)
xmake f -m asan && xmake  # AddressSanitizer + UBSan
xmake f -m tsan && xmake  # ThreadSanitizer
```

Optional config flags: `xmake f --use_numa=y`, `xmake f --native_arch=y` (enables `-march=native` for benchmark targets).

Optional dependencies: `vcpkg::dpdk` (for `eph-dpdk` and `*_dpdk` targets), `aws-lc` (TLS / HMAC / CSPRNG — required by `eph-transport` and `eph-net`), `numactl`, `tabulate`. `gtest` and `benchmark` are auto-fetched.

The **release build excludes ALL tests/benchmarks/examples by default** (every such target sets `set_default(false)`). Build them explicitly:

```bash
xmake build -g tests       # build all test targets
xmake build -g benchmarks  # build all microbenchmark targets
xmake build -g examples    # build all example targets
xmake build <target_name>  # build a single target
```

## Tests

Tests use **gtest** with the shared `eph-test` rule defined in the root `xmake.lua` (sets `kind=binary`, `group=tests`, `default=false`, links gtest, defines `SPDLOG_NO_EXCEPTIONS`).

Each module owns its tests under `<module>/tests/**.cpp`; the module's `xmake.lua` auto-globs them into per-file targets named after the file basename. So `eph-net/tests/test_gateway.cpp` → target `test_gateway`.

```bash
xmake build -g tests          # build all tests
xmake run test_gateway        # run a single test (use the file basename)
xmake run -g tests            # run every test in the group
```

Cross-module integration tests live in `tests/integration/` (e.g. `test_transport_e2e`, `test_binance_adapter`). Per CLAUDE rules: after modifying any code, run the tests that cover it before declaring done.

DPDK end-to-end integration tests live in `eph-dpdk/tests/integration/test_dpdk_e2e.cpp` (single binary, all 7 P0+P1 cases). They drive a real `TcpSession`/`UdpSender` over NIC_B against kernel echo mocks bound to NIC_A, and require NIC_B bound to vfio-pci at run time. Use the wrapper for the friendly path:

```bash
sudo benchmarks/latency/lat tcp --dpdk     # transition NIC_B to vfio-pci once
sudo tests/integration/dpdk_e2e            # run all 7 E2E tests
```

When NIC_B is on the kernel driver, all 7 tests SKIP cleanly with a diagnostic — safe to run on any host.

## Benchmarks

Two distinct benchmark systems:

1. **Microbenchmarks** (Google Benchmark) — per-module `<module>/benchmarks/**.cpp`, auto-globbed via the `eph-bench` rule. Run with `xmake run bench_fix_parse`, `xmake run bench_array_book`, etc. **Per CLAUDE rules**: when modifying code that has an associated benchmark, run the benchmark first to capture a baseline, then re-run after the change and verify no regression.

2. **End-to-end latency benchmarks** — `benchmarks/latency/`, one `lat_<scenario>[_dpdk]` binary per scenario (`lat_tcp`, `lat_udp`, `lat_ws`, `lat_ex_market`, `lat_ex_order`, `lat_ex_md_udp`, plus `_dpdk` variants). Each binary forks its own kernel mock echo server and runs the bench client; the mock is **always kernel** (only the client side differs between kernel and DPDK), which is what makes the comparison fair. The bench writes no files. Drive everything via the wrapper script — it handles NIC-B state transitions (host kernel ↔ bench_ns ↔ vfio-pci) idempotently and execs the right binary:

```bash
sudo ./benchmarks/latency/lat tcp           # raw TCP RTT, kernel client
sudo ./benchmarks/latency/lat udp --dpdk    # raw UDP RTT, DPDK client
sudo ./benchmarks/latency/lat ex_market     # exchange bookTicker push
```

`benchmarks/latency/bench.conf` holds the NIC/IP/CPU layout. Shared bench infrastructure is a header-only library under `benchmarks/latency/core/` with its own unit tests in `tests/unit/bench/`.

## Architecture

### Module dependency order

```
eph-core ← eph-utils ← eph-containers ← eph-transport ← {eph-net, eph-dpdk}
eph-fix, eph-itch, eph-json, eph-book — depend only on eph-core (+ eph-utils)
eph-book additionally bridges to eph-json and eph-itch via adapters
```

`eph-net` and `eph-dpdk` are sibling backends — both provide types satisfying the `TcpTransport` concept that `eph-transport` consumes as a template parameter. Pick one (or both) at the application layer; the rest of the stack does not change.

### Three transport variants (eph-transport)

All three compose from the same building blocks (`TransportCore`, `ReconnectPolicy`, `FrameProcessor`) and present an identical `create() / send() / recv() / close_gracefully()` API. Pick by threading model:

| Variant | Threads | Use case |
|---|---|---|
| `Transport<TcpImpl, Framer, ...>` | TX thread + RX thread + SPSC queues | General-purpose, app decoupled from I/O |
| `DirectTxTransport<...>` | RX thread only | Low-latency TX (app calls send directly), background RX |
| `DirectTransport<...>` | None | Single-threaded event loops, DPDK poll-mode, Reactor |

`presets.hpp` provides canonical aliases (`DefaultTransport<T>`, `DirectDefaultTransport<T>`, …) — prefer these over hand-rolled template instantiations.

### Concepts that gate the type system

- **`TcpTransport`** (`eph-core/include/eph/core/tcp_concept.hpp`) — backend interface (`connect`, `send`, `poll_rx`, `close`, `reset`, `mss`, `state`). Implemented by `eph::net::SocketTransport` and `eph::dpdk::TcpSession`.
- **`MessageFramer`** (`framer_concept.hpp`) — pluggable framing (`encode`, `decode`, `max_overhead`). Implementations: `WsFramer`, `RawFramer`, `LengthPrefixFramer`, `FixFramer`, `JsonFramer`, `ItchFramer`, `SoupBinTcpFramer`. See `docs/custom-framer.md` for writing new ones.
- **`MetricsSink`** (`metrics_concept.hpp`) — Prometheus-style counters/gauges/histograms. `NullSink` is zero-cost; `ConsoleSink` (in eph-utils) logs via spdlog.

### Per-module layout convention

Every `eph-*` module follows the same shape:

```
eph-<name>/
  include/eph/<name>/*.hpp     ← public headers
  tests/test_*.cpp             ← gtest unit tests (auto-globbed)
  benchmarks/bench_*.cpp       ← Google Benchmark microbenchmarks (auto-globbed)
  xmake.lua                    ← header-only target + test/bench glob loops
  README.md, CHANGELOG.md, summary.md, ONBOARDING.md  ← per-module docs
```

When adding a new test or benchmark, just drop the `.cpp` into the module's `tests/` or `benchmarks/` directory — the glob loop in the module xmake.lua picks it up automatically; no manual target wiring needed.

### Logging is compile-time filtered

All modules use spdlog with `SPDLOG_ACTIVE_LEVEL` set per build mode (`SPDLOG_LEVEL_TRACE` in debug, `SPDLOG_LEVEL_INFO` in release, controlled by the `net_log_level` global in the root xmake.lua). Use the `SPDLOG_TRACE / DEBUG / INFO / WARN / ERROR` macros (not the runtime spdlog::trace APIs) so suppressed levels compile out entirely. Tests inherit the SPDLOG_NO_EXCEPTIONS define from the `eph-test` rule.

### DPDK pieces have special link requirements

DPDK PMDs need whole-archive linking. The root xmake.lua exposes a helper `apply_dpdk_pmd_linkgroups()` — call it on any target that links `eph-dpdk` (see the `*_dpdk` example targets at the bottom of the root xmake.lua for the pattern).

DPDK environment setup is via `eph-dpdk/scripts/dpdk-setup.sh` and `dpdk-teardown.sh`; see `docs/dpdk-setup.md`.

## Conventions specific to this codebase

- **Header-only everywhere.** Do not introduce `.cpp` files in module `include/` directories. New functionality lives in headers; if a function is non-trivial it should still be `inline` or a template.
- **Prefer concepts over inheritance.** This codebase has zero virtual dispatch in the hot path. Constrain templates with the existing concepts (`TcpTransport`, `MessageFramer`, `MetricsSink`); do not add abstract base classes.
- **Zero-copy parsers.** `eph-fix`, `eph-itch`, `eph-json` parsers operate directly on receive buffers and return view types — do not add owning string conversions on the hot path.
- **`std::expected` for fallible APIs.** Transport and parser errors return `std::expected<T, ErrorEnum>` using the typed enums in `eph-core/transport_errors.hpp` and per-module error types. Do not throw across module boundaries (and `SPDLOG_NO_EXCEPTIONS` is set in tests anyway).
- **TSC, not steady_clock, for measurement.** `eph::utils::TSC` (in `eph-utils/time.hpp`) is the canonical timer for nanosecond measurements; benchmarks and the latency framework all use it.
- **Per-module README/CHANGELOG/summary/ONBOARDING are regenerated, not hand-edited.** They are produced by the doc workflow — see the most recent commit `afaceba docs: regenerate README/CHANGELOG/ONBOARDING/summary for all subprojects`. Edit source code and let the generator update them rather than tweaking docs by hand.

## Useful docs in this repo

- `summary.md` — full architecture, module map, data flow (this is the deepest single document)
- `docs/dpdk-setup.md` — DPDK hugepages, vfio-pci binding
- `docs/latency-benchmark-fairness.md` — why the kernel-vs-DPDK comparison is structured the way it is
- `docs/custom-framer.md` — adding a new `MessageFramer` implementation
- `docs/operations-runbook.md`, `docs/troubleshooting.md`, `docs/production-config.md`
