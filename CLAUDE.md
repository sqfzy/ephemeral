# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

ephemeral (`eph`) is a header-only C++23 ultra-low-latency networking library for HFT.
Eleven independent modules share a concept-driven, zero-virtual-dispatch design. Two
networking backends — **eph-net-kernel** (epoll / POSIX sockets) and **eph-net-dpdk**
(DPDK kernel-bypass) — both provide `TcpStream<Codec>` / `UdpSocket<Codec>` types that
satisfy the common `eph::net::Stream` / `eph::net::Datagram` concepts and plug into the
shared `eph::net::Poller` concept.

See `README.md` for the public-facing overview, `summary.md` for the full architecture /
module map, and `docs/architecture.md` for the v3.3 concept model.

The v3.3 architecture is the result of a large refactor that finished in April 2026. The
frozen design spec is at `.artifacts/design-eph-v3.3-architecture-20260410.md` — refer to
it when reasoning about module boundaries or concept contracts.

## Build

Build system is **xmake**. Compiler must be GCC ≥ 13 or Clang ≥ 17 (uses `std::expected`,
`std::format`).

```bash
xmake -m release          # release build (set_optimize fastest)
xmake -m debug            # debug build (SPDLOG_LEVEL_TRACE)
xmake f -m asan && xmake  # AddressSanitizer + UBSan
xmake f -m tsan && xmake  # ThreadSanitizer
```

Optional config flags: `xmake f --use_numa=y`, `xmake f --native_arch=y` (enables
`-march=native` for benchmark targets).

Optional dependencies: `vcpkg::dpdk` (for `eph-net-dpdk` and `*_dpdk` targets), `aws-lc`
(TLS / HMAC / CSPRNG — required by `eph-net` for its TLS path), `numactl`, `tabulate`.
`gtest` and `benchmark` are auto-fetched. DPDK builds use the GCC wrapper at
`/tmp/gcc14-wrap/g++` which reorders `-isystem` / `-L` so aws-lc resolves before
vcpkg's bundled libssl — see Phase 7's commit message (`c2a0ca4`) for the rationale.

The **release build excludes ALL tests/benchmarks/examples by default** (every such target
sets `set_default(false)`). Build them explicitly:

```bash
xmake build -g tests       # build all test targets
xmake build -g benchmarks  # build all microbenchmark targets
xmake build -g examples    # build all example targets
xmake build <target_name>  # build a single target
```

## Tests

Tests use **gtest** with the shared `eph-test` rule defined in the root `xmake.lua` (sets
`kind=binary`, `group=tests`, `default=false`, links gtest, defines `SPDLOG_NO_EXCEPTIONS`).

Each module owns its tests under `<module>/tests/**.cpp`; the module's `xmake.lua`
auto-globs them into per-file targets named after the file basename. So
`eph-net-kernel/tests/test_kernel_tcp_stream.cpp` → target `test_kernel_tcp_stream`.

```bash
xmake build -g tests          # build all tests
xmake run test_ws_codec       # run a single test (use the file basename)
xmake run -g tests            # run every test in the group
```

Cross-module integration tests live in `tests/integration/` (e.g. `test_transport_e2e`,
`test_binance_adapter`, `test_kernel_udp`). Per CLAUDE rules: after modifying any code,
run the tests that cover it before declaring done.

DPDK end-to-end integration tests live in
`eph-net-dpdk/tests/integration/test_dpdk_e2e.cpp` (single binary, all P0+P1 cases). They
drive a real `DpdkTcpStream` / `DpdkUdpSocket` over NIC_B against kernel echo mocks bound
to NIC_A, and require NIC_B bound to vfio-pci at run time. Use the wrapper for the
friendly path:

```bash
sudo benchmarks/latency/lat tcp --dpdk     # transition NIC_B to vfio-pci once
sudo tests/integration/dpdk_e2e            # run the full E2E suite
```

When NIC_B is on the kernel driver, the tests SKIP cleanly with a diagnostic — safe to
run on any host.

## Benchmarks

Two distinct benchmark systems:

1. **Microbenchmarks** (Google Benchmark) — per-module `<module>/benchmarks/**.cpp`,
   auto-globbed via the `eph-bench` rule. Run with `xmake run bench_fix_parse`,
   `xmake run bench_array_book`, etc. **Per CLAUDE rules**: when modifying code that has
   an associated benchmark, run the benchmark first to capture a baseline, then re-run
   after the change and verify no regression.

2. **End-to-end latency benchmarks** — `benchmarks/latency/`, one
   `lat_<scenario>[_dpdk]` binary per scenario (`lat_tcp`, `lat_udp`, `lat_ws`,
   `lat_ex_market`, `lat_ex_order`, `lat_ex_md_udp`, plus `_dpdk` variants). Each binary
   forks its own kernel mock echo server and runs the bench client; the mock is
   **always kernel** (only the client side differs between kernel and DPDK), which is
   what makes the comparison fair. The bench writes no files. Drive everything via the
   wrapper script — it handles NIC-B state transitions
   (host kernel ↔ bench_ns ↔ vfio-pci) idempotently and execs the right binary:

```bash
sudo ./benchmarks/latency/lat tcp           # raw TCP RTT, kernel client
sudo ./benchmarks/latency/lat udp --dpdk    # raw UDP RTT, DPDK client
sudo ./benchmarks/latency/lat ex_market     # exchange bookTicker push
```

`benchmarks/latency/bench.conf` holds the NIC/IP/CPU layout. Shared bench infrastructure
is a header-only library under `benchmarks/latency/core/` with its own unit tests in
`tests/unit/bench/`.

## Architecture

### Module dependency order

```
eph-utils  ←  eph-containers
                   ↑
             eph-core  (Error / ErrorInfo / StreamCodec / DatagramCodec / OutputBuffer)
                   ↑
             ┌─────┴──────┬─────────────────┐
             ↓            ↓                 ↓
        eph-codec     eph-net          eph-fix / eph-itch /
     (WsCodec,      (Stream /          eph-json / eph-book
      RawStream-    Datagram /         (all parser modules
      Codec,        Pollable /         keep the pre-v3.3
      Mold64-       Poller concepts,   framer API; they still
      Codec, …)     SocketAddr,        satisfy the new Codec
                    ReconnectPolicy,   concept so they plug into
                    test mocks, TLS    eph-net-kernel / -dpdk)
                    session detail)
                         ↑
                ┌────────┴────────┐
                ↓                 ↓
        eph-net-kernel    eph-net-dpdk
        (epoll +          (lcore burst +
         KernelTcpStream  DpdkTcpStream
         KernelUdpSocket  DpdkUdpSocket
         KernelPoller)    DpdkPoller + Eal + …)
```

Key rules:

- `eph-net-kernel` and `eph-net-dpdk` are **sibling backends** that never depend on each
  other. Pick one (or both) in your application target.
- `eph-net` does NOT depend on `eph-codec`: codecs are template parameters. A consumer
  links `eph-net-kernel` (or `-dpdk`) plus whichever codec modules it needs.
- `eph-fix`, `eph-itch`, `eph-json`, `eph-book` still depend only on `eph-core` and
  `eph-utils`. They never transitively pull in networking.
- All DPDK build weight (vfio-pci, hugepages, `apply_dpdk_pmd_linkgroups()`) is
  confined to `eph-net-dpdk`. Kernel-only users are fully immune.

### The three v3.3 concepts

The type system pivots on three narrow concepts defined in `eph-core` and `eph-net`:

- **`eph::core::StreamCodec<T>` / `eph::core::DatagramCodec<T>`** — stateful decoders
  that consume a `PacketView` and emit zero or more `Frame`s. `StreamCodec` supports
  streaming reassembly (`decode` returns `expected<optional<Frame>>`). `DatagramCodec`
  processes one complete datagram per call and emits 0/1/N frames through a sink
  (`MoldUDP64` is the canonical example). Both accept a writable `OutputBuffer&` so the
  codec can inject auto-responses (WS pong, close ack).

- **`eph::net::Stream<T>` / `eph::net::Datagram<T>`** — the per-connection user-facing
  type. Combines a byte socket, a codec, an optional TLS state, and a Poller attachment.
  Implementations:
  - `eph::net::kernel::KernelTcpStream<C, EnableTls>` / `KernelUdpSocket<C>`
  - `eph::net::dpdk::DpdkTcpStream<C, EnableTls>` / `DpdkUdpSocket<C>`
  - `eph::net::test::FakeStream` / `FakeDatagram` (in-memory mocks with no syscalls)

- **`eph::net::Poller<T>`** — the single I/O driver. Hosts heterogeneous Pollables via
  P2 function-pointer type erase. Implementations:
  - `eph::net::kernel::KernelPoller` (epoll, supports `poll(timeout)`)
  - `eph::net::dpdk::DpdkPoller<>` (lcore burst poll, non-blocking only)
  - `eph::net::test::TestPoller<P>` (drives FakeStream/FakeDatagram synchronously)

Every user program follows the same shape: `auto poller = …; auto stream = …;
poller->add(stream.get()); while (running) poller->poll();`. Single-connection and
multi-connection programs use exactly the same API.

### Per-module layout convention

Every `eph-*` module follows the same shape:

```
eph-<name>/
  include/eph/<name>/*.hpp     ← public headers
  tests/test_*.cpp             ← gtest unit tests (auto-globbed)
  benchmarks/bench_*.cpp       ← Google Benchmark microbenchmarks (auto-globbed)
  xmake.lua                    ← header-only target + test/bench glob loops
  README.md, CHANGELOG.md, summary.md, docs/ONBOARDING.md  ← per-module docs
```

When adding a new test or benchmark, just drop the `.cpp` into the module's `tests/` or
`benchmarks/` directory — the glob loop in the module xmake.lua picks it up automatically.

### Logging is compile-time filtered

All modules use spdlog with `SPDLOG_ACTIVE_LEVEL` set per build mode
(`SPDLOG_LEVEL_TRACE` in debug, `SPDLOG_LEVEL_INFO` in release, controlled by the
`net_log_level` global in the root xmake.lua). Use the
`SPDLOG_TRACE / DEBUG / INFO / WARN / ERROR` macros (not the runtime spdlog::trace APIs)
so suppressed levels compile out entirely. Tests inherit the SPDLOG_NO_EXCEPTIONS define
from the `eph-test` rule.

### DPDK pieces have special link requirements

DPDK PMDs need whole-archive linking. The root xmake.lua exposes a helper
`apply_dpdk_pmd_linkgroups()` — call it on any target that links `eph-net-dpdk` (see the
`*_dpdk` example targets at the bottom of the root xmake.lua for the pattern).

DPDK environment setup is via `eph-net-dpdk/scripts/dpdk-setup.sh` and
`dpdk-teardown.sh`; see `docs/dpdk-setup.md`.

## Conventions specific to this codebase

- **Header-only everywhere.** Do not introduce `.cpp` files in module `include/`
  directories. New functionality lives in headers; if a function is non-trivial it
  should still be `inline` or a template.
- **Prefer concepts over inheritance.** This codebase has zero virtual dispatch in the
  hot path. Constrain templates with the v3.3 concepts (`Stream`, `Datagram`, `Pollable`,
  `Poller`, `StreamCodec`, `DatagramCodec`, `MetricsSink`); do not add abstract base
  classes.
- **Zero-copy parsers.** `eph-fix`, `eph-itch`, `eph-json` parsers operate directly on
  receive buffers and return view types. On the DPDK path, `DpdkTcpStream` exposes an
  `MbufView` that lets TLS decrypt in-place via aws-lc so the codec sees the plaintext
  inside the original mbuf without any memcpy. Do not add owning string conversions on
  the hot path.
- **`PacketView` is the zero-copy contract.** Every `Stream` / `Datagram` implementation
  exposes a `using PacketView = …;` associated type that provides
  `writable_data() / data() / length() / trim_front(n) / trim_back(n) / arrival_tsc()`.
  Codecs are templated on `PacketView` so the same `WsCodec` works against both the
  contiguous `SpanView` (kernel) and the mbuf-backed `MbufView` (DPDK) with no runtime
  branching.
- **`std::expected<T, ErrorInfo>` for fallible APIs.** The v3.3 error type is
  `eph::core::Error` (enum) + `eph::core::ErrorInfo` (enum + const char* detail). Pre-v3.3
  per-module error enums (`SendError`, `ConnectionError`, …) have been retired; the
  parser modules that still expose domain-specific enums (`FrameError`, `FixError`, …)
  remain unchanged. Do not throw across module boundaries (`SPDLOG_NO_EXCEPTIONS` is set
  in tests anyway).
- **TSC, not steady_clock, for measurement.** `eph::utils::TSC` (in `eph-utils/time.hpp`)
  is the canonical timer for nanosecond measurements; benchmarks and the latency
  framework all use it.
- **Per-module README/CHANGELOG/summary/ONBOARDING.** These are regenerated on major
  refactors — prefer editing the source code and re-running the doc pass over tweaking
  docs by hand. The v3.3 refactor regenerated them wholesale.

## Useful docs in this repo

- `summary.md` — full architecture, module map, data flow (deepest single document)
- `docs/architecture.md` — v3.3 concept model / module graph / PacketView contract
- `docs/poller-guide.md` — the `Poller` concept with kernel and DPDK examples, single and
  multi-connection patterns, heterogeneous `TcpStream + UdpSocket` on one poller
- `docs/custom-codec.md` — writing a new `StreamCodec` or `DatagramCodec`
- `docs/dpdk-setup.md` — DPDK hugepages, vfio-pci binding, `eph-net-dpdk` environment
- `docs/latency-benchmark-fairness.md` — why the kernel-vs-DPDK comparison is structured
  the way it is
- `docs/operations-runbook.md`, `docs/troubleshooting.md`, `docs/production-config.md`
