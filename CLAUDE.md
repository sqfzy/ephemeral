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
module map, and `docs/architecture.md` for the concept model.

The frozen design spec is at `.artifacts/design-eph-v3.3-architecture-20260410.md` — refer
to it when reasoning about module boundaries or concept contracts.

The scope decisions for the current feature set are archived in
`.artifacts/phase-9-scope-decision.md`. Available public surface includes:

- `eph::net::parse_http_request` / `parse_http_response` / `build_http_request` —
  incremental zero-heap HTTP/1.1 parser subset. Explicitly rejects chunked /
  `Transfer-Encoding` / cookies / redirect / `Expect: 100-continue` — HFT exchanges
  do not use them and they are substantial attack surface.
- `eph::net::HmacSha256` with typed `Key` (RAII-clearing) and `Tag` wrappers.
- `eph::net::ProxyConfig` + kernel `StreamConfig::proxy` — HTTP CONNECT proxy.
  Kernel backend only; the field has been removed from the DPDK `StreamConfig`
  entirely (post-T3.19) — misuse on DPDK is a compile error.
- `eph::net::WsConfig` (`cfg.ws.path` / `host` / `extra_headers` / `timeout` /
  `permessage_deflate`) — backend-shared sub-config. Non-empty `ws.path`
  transparently performs the RFC 6455 client handshake inside `TcpStream::create()`
  on both backends.
- `eph::net::KeepaliveConfig` (`cfg.keepalive.interval` / `probes`) —
  backend-shared sub-config; default disabled. Kernel wires `setsockopt
  (SO_KEEPALIVE / TCP_KEEPIDLE / TCP_KEEPINTVL / TCP_KEEPCNT)`; DPDK lowers
  to `cfg.dpdk.wire.keepalive_*` for the PMD's `TcpSession::
  tick_keepalive`. Replaces the DPDK-only `cfg.legacy.keepalive_*` path.
- `eph::utils::KillSwitch` — single-fire, non-resettable compliance primitive.
- `eph::utils::TokenBucket` — thread-safe weighted rate limiter.
- `eph::utils::Backoff` concept + `ExponentialBackoff` / `ConstantBackoff` —
  generic retry-backoff strategies (`backoff.hpp`); `next_delay() ->
  std::optional<ms>`, `nullopt` = exhausted (absorbing). `ExponentialBackoff`
  is the math formerly in `eph::net::ReconnectPolicy` (moved here so
  `ReconnectOrchestrator` and `retry` share one source); `multiplier == 1.0`
  is now legal (constant backoff). `ExponentialBackoff` also exposes
  `reset()` / `attempts()` for the orchestrator's cross-cycle reuse.
- `eph::utils::retry(fn, backoff[, when][, sleeper])` — backon-style blocking
  retry driver (`retry.hpp`). Drives a `std::expected`-returning callable,
  sleeping between attempts per a `Backoff`, until success / non-retriable
  error (`when` predicate, default retry-all) / exhaustion. `sleeper` defaults
  to `ThreadSleeper` and is injectable so tests don't actually sleep. Blocking
  only — for non-blocking poll-loop reconnection use `ReconnectOrchestrator`.
- `eph::core::MetricsSink` concept + `NullSink` / `eph::utils::ConsoleSink` — the
  generic push sink for observability. Any user type with `push_counter` /
  `push_gauge` / `push_histogram` / `flush` satisfies it (duck-typed).
- `eph::net::StreamMetric` enum + `eph::net::publish_metrics<Stream, Sink>` — the
  two-layer observability for the 4 stream backends. Hot path: each stream owns an
  `alignas(64) std::atomic<uint64_t>` array, incremented via a private template
  `inc_<M>()` that compiles to a single `lock add` on x86 (verified by objdump).
  Reader: `metric(StreamMetric m)` direct read (bounds-checked — out-of-range
  values return 0), or `publish_metrics(*stream, sink, tags)` to forward every
  counter into any `MetricsSink`. See `docs/observability-guide.md` and
  `examples/observability_demo.cpp`. The 26 entries include 8 TCP/ICMP
  session-level metrics (`net.stream.tcp.*` + `net.stream.icmp.*`) exposed
  by `DpdkTcpStream::metric` via lazy-read from `TcpSession::Stats`.
- DPDK keepalive — public surface is now `cfg.keepalive.interval` /
  `probes` (post-T3.19, see above). The wire-level
  `eph::dpdk::TcpConfig::keepalive_interval / keepalive_probes` and the
  caller-driven `TcpSession::tick_keepalive(now_tsc)` are still the
  underlying mechanism; `DpdkTcpStream::create` lowers `cfg.keepalive`
  into the wire-level fields at factory time. In DPDK production
  (`DpdkPoller::poll`) the tick fires on every poll cycle via the
  `on_poll_tick_` hook; single-stream users driving `poll_once_`
  directly must tick themselves.
- `eph::dpdk::Platform::rss_using_probed_key()` — diagnostic getter
  reflecting which RSS bring-up path resolved. `Platform::create` for
  `enable_rss=true && nb_rx_queues>1` first tries `configure_rss`
  (installs eph's key); if the PMD rejects `rte_eth_dev_rss_hash_update`
  (notably ENA), it falls back to a probe via
  `rte_eth_dev_rss_hash_conf_get` and uses the NIC's actual key for
  `predict_rss_queue`. `rss_using_probed_key()` returns true on that
  fallback path. If the probe also fails, `Platform::create` hard-fails
  with a recovery hint; the previous silent-collapse-to-queue-0 path
  was removed (BREAKING CHANGE — see `eph-net-dpdk/CHANGELOG.md`). The
  hard-fail also fires for `enable_rss=false && nb_rx_queues>1`.
- `eph::dpdk::TcpSession::effective_mss()` / `peer_mss_negotiated()` —
  connection MSS is clamped to `min(local, peer SYN-ACK MSS)` and may shrink
  further on ICMP Frag Needed; exposed read-only for diagnostics.
- `eph::dpdk::TcpSession::on_icmp_frag_needed(mtu)` + `eph::dpdk::Platform::
  register_icmp_target` — path-MTU feedback path. `DpdkTcpStream::
  create_and_attach` wires the stream into Platform's ICMP registry so
  router-originated Type 3 Code 4 messages are dispatched to the owning
  stream regardless of which RX queue they land on (RSS-safe). The
  registry is `shared_ptr`-managed and internally mutex-locked: safe
  under any declaration / destruction order between Platform / Poller
  / Stream, and safe under concurrent register/unregister/dispatch
  (ASan + TSan verified). `DpdkPoller::set_icmp_callback` takes a
  `std::function<void(ParsedIcmp const&)>` that closes over the
  registry's shared_ptr; Stream's `IcmpTargetHandle` holds a weak_ptr
  and unregisters safely even if the registry predeceases it.
- `DpdkTcpStream::create_and_attach(cfg, platform)` — turnkey production
  factory. Handles queue selection (Software / RSS-pinned / FlowDirector),
  src_port allocation (rebinding to match RSS hash when pinning), TCP/TLS/
  WS handshakes, Poller attach, FlowDirector rule install, and ICMP
  registration. The older `create(cfg, poller)` overload was removed —
  its narrow subset is covered by `create_and_attach`.
- `eph::net::dpdk::DpdkPollable` concept grew `on_poll_tick_(uint64_t tsc)
  noexcept`. Invoked once per poll cycle by `DpdkPoller::poll()` for every
  registered entry — used by TCP keepalive; UDP implements as no-op.
- DPDK Platform — daemon-led model. Two configs and three factory
  entries cover every deployment shape:
  - `eph::dpdk::PlatformConfig` (lean, application-side): `pci` +
    `queues` + per-process EAL knobs (`pins` / `pin_policy` /
    `lcores` / `extra_eal_args` / `program_name`). `proc_type=Secondary`,
    `file_prefix=eph_<sanitize_bdf(pci)>`, and `allowed_devs={pci}` are
    derived internally — applications never set them. Consumed by
    `Platform::create(PlatformConfig)`, which attaches as a DPDK
    secondary to an already-running `eph-nicd` daemon.
  - `eph::dpdk::NicServiceConfig` (daemon-side): `pci` +
    `total_queues` + `rss_key` + descriptor / mempool / promiscuous
    fields + `daemon_lcore`. Consumed by
    `Platform::serve_nic(NicServiceConfig)` from the `eph-nicd` binary
    only; tenants must not call `serve_nic`.
  - `Platform::join()` blocks until SIGTERM/SIGINT — used by the
    daemon binary to keep the NIC primary alive while secondaries
    attach.
  Tenants share the daemon's hugepage segment via DPDK's standard
  `--proc-type=secondary` mechanism and own a disjoint queue
  sub-range. Hot path is unchanged: `inc_<M>` / `rr_counter` / `poll`
  are per-process and never cross the daemon boundary. Source-port
  partitioning across tenants is the **operator's** responsibility —
  `eph-net-dpdk` does not auto-allocate src_port and has no global
  view to enforce disjointness; coordinate via configuration files
  outside eph. Cross-tenant CPU pinning is also an OS-level concern
  (systemd Slice / cgroups / taskset) — eph sees only its own
  process's pin registry. `eph::dpdk::EalConfig` + `build_eal_argv`
  remain in `eal.hpp` as an internal helper for assembling argv;
  they are no longer accepted as public factory parameters
  (`Platform::create` / `serve_nic` derive everything from the lean
  configs above). The previous autojoin shape
  (`Platform::create_or_join` / `Platform::launch` /
  `CreateOrJoinConfig` / kitchen-sink `PlatformConfig` with
  `max_procs` / `queues_per_proc` / `file_prefix`) was removed in
  2026-05-02's reshape — see `eph-net-dpdk/CHANGELOG.md` BREAKING
  entry for the migration table, and
  `eph-net-dpdk/docs/dpdk-daemon-deployment.md` /
  `dpdk-multiprocess.md` / `dpdk-reconnect-pattern.md` for the
  ops / architecture / reconnect stories.

Deliberately **not** included: `Gateway`, `CircuitBreaker`,
chunked HTTP, SOCKS5 proxy. See `.artifacts/phase-9-scope-decision.md` for rationale
and recovery guidance if a future need surfaces.

Observability scope deferred for now: histogram integration (delay distributions
would reuse the existing `eph::utils::Recorder` / HdrHistogram), gauge-type metrics
(e.g. `reasm_readable_bytes`), tracing context, and OpenTelemetry SDK adapters.
See `.artifacts/discuss-20260418-181343-metrics-sink-architecture.md` for the
full architectural discussion.

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

Optional dependencies: system **libdpdk** via pkg-config (for `eph-net-dpdk` and
`*_dpdk` targets — `sudo pacman -S dpdk` on Arch, `sudo apt install libdpdk-dev` on
Ubuntu, or build from source with `meson setup -Ddisable_drivers=crypto/openssl`),
`aws-lc` (TLS / HMAC / CSPRNG — required by `eph-net` for its TLS path), `numactl`,
`tabulate`. `gtest` and `benchmark` are auto-fetched. The previous vcpkg DPDK path
leaked vcpkg's bundled libssl headers into the DPDK TU and collided with aws-lc's
(ASN1_NULL / CRYPTO_THREADID typedefs); using system libdpdk isolates the headers
under `/usr/include/dpdk/` and eliminates the conflict — the `/tmp/gcc14-wrap/g++`
include-order workaround is retired.

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

libFuzzer harnesses live under `eph-net-dpdk/fuzzers/` (currently `fuzz_dns_reply`,
`fuzz_arp_reply`, `fuzz_icmp_reply`, `fuzz_udp_packet`). They are **intentionally outside the xmake graph** because the
default toolchain is GCC 14 and libFuzzer needs Clang ≥ 17 — build them with the
`clang++ -fsanitize=fuzzer,address,undefined ...` command in
`eph-net-dpdk/fuzzers/README.md`. Do not assume `xmake build -g tests` covers them.

## Benchmarks

Two distinct benchmark systems:

1. **Microbenchmarks** (Google Benchmark) — per-module `<module>/benchmarks/**.cpp`,
   auto-globbed via the `eph-bench` rule. Run with `xmake run bench_fix_parse`,
   `xmake run bench_array_book`, etc. **Per CLAUDE rules**: when modifying code that has
   an associated benchmark, run the benchmark first to capture a baseline, then re-run
   after the change and verify no regression.

2. **End-to-end latency benchmarks** — `benchmarks/latency/`, one
   `lat_<scenario>[_dpdk]` binary per scenario, all served by the single C++23
   `benchmarks/mockex/mockex` binary (`mockex --scenario <name>` dispatches to
   the handler for `[lat_<name>]`). The mock is **always kernel**, only the
   client side differs between kernel and DPDK, which is what makes the
   comparison fair. Each scenario's run-loop (in
   `benchmarks/latency/scenarios/lat_<name>_loop.hpp`) writes one (1-leg push:
   `lat_ex_market`, `lat_ex_market_2p`) or three (echo-RTT scenarios — `rtt`,
   `tx`, `rx` legs) uniquely-prefixed `Recorder::export_json` files to
   `benchmarks/latency/outputs/`.

   The seven scenarios:

   | Scenario          | Client                                             | mockex handler                         |
   |-------------------|----------------------------------------------------|----------------------------------------|
   | `lat_tcp`         | `KernelTcpStream<RawStreamCodec>` echo RTT         | `tcp_echo_run` (echo + ts stamp)       |
   | `lat_udp`         | `KernelUdpSocket<RawDatagramCodec>` echo RTT       | `udp_echo_run`                         |
   | `lat_ws`          | `KernelTcpStream<WsCodec>` echo RTT (RFC 6455)     | `ws_echo_run`                          |
   | `lat_ex_market`   | `WsCodec` + `core/json_scan.hpp` 1-leg oneway      | `ex_market_push_run` (MMPP-2 + pool)   |
   | `lat_ex_market_2p`| 2-phase multi-symbol + burst                       | `ex_market_2p_push_run`                |
   | `lat_ex_order`    | `WsCodec` + N-inflight JSON order RTT              | `ex_order_echo_run` (JSON ts splice)   |
   | `lat_ex_md_udp`   | `RawDatagramCodec` Mold64-style RTT                | `ex_md_udp_echo_run`                   |

   Drive everything via the `lat` dispatcher — it handles NIC-B state transitions
   (host kernel ↔ `bench_ns` ↔ vfio-pci) idempotently and execs the right binary:

```bash
sudo ./benchmarks/latency/lat tcp           # raw TCP RTT, kernel client
sudo ./benchmarks/latency/lat udp --dpdk    # raw UDP RTT, DPDK client
sudo ./benchmarks/latency/lat ex_market     # exchange bookTicker push
```

Configuration is a single INI-style `benchmarks/latency/bench.conf`: lowercase global
keys (NIC/IP/CPU layout, `warmup_samples`) before the first section header, then one
`[lat_<scenario>]` section per binary (`port`, `payload_size`, `duration_seconds`,
push-scenario `mockex_params` / `mockex_payload` / `mockex_seed`, optional
`endpoint = wss://...` for real-server mode). Both the C++ client and mockex read the
same file via `bench::ScenarioConfig`.

Exception to the canonical TSC rule: bench client and mockex both read
`clock_gettime(CLOCK_MONOTONIC_RAW)` via `bench::monotonic_raw_ns()` so they share a
time base on one-way scenarios. Samples feed
`eph::utils::Recorder::record_ns()` directly (no cycle→ns conversion). Shared bench
infrastructure is a header-only library under `benchmarks/latency/core/` with its own
unit tests in `tests/unit/bench/`. See `benchmarks/latency/README.md` for the scenario
list, config schema, and standalone mock debugging.

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
      Codec,        Pollable /         keep the legacy
      Mold64-       Poller concepts,   framer API; they still
      Codec, …)     SocketAddr,        satisfy the Codec
                    Reconnect-         concept so they plug into
                    Orchestrator,      eph-net-kernel / -dpdk)
                    test mocks, TLS
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

### The three core concepts

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
  hot path. Constrain templates with the core concepts (`Stream`, `Datagram`, `Pollable`,
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
- **`std::expected<T, ErrorInfo>` for fallible APIs.** The error type is
  `eph::core::Error` (enum) + `eph::core::ErrorInfo` (enum + const char* detail). Legacy
  per-module error enums (`SendError`, `ConnectionError`, …) have been retired; the
  parser modules that still expose domain-specific enums (`FrameError`, `FixError`, …)
  remain unchanged. Do not throw across module boundaries (`SPDLOG_NO_EXCEPTIONS` is set
  in tests anyway).
- **TSC, not steady_clock, for measurement.** `eph::utils::TSC` (in `eph-utils/time.hpp`)
  is the canonical timer for nanosecond measurements; benchmarks and the latency
  framework all use it.
- **Per-module README/CHANGELOG/summary/ONBOARDING.** These are regenerated on major
  refactors — prefer editing the source code and re-running the doc pass over tweaking
  docs by hand.

## Useful docs in this repo

- `summary.md` — full architecture, module map, data flow (deepest single document)
- `docs/architecture.md` — concept model / module graph / PacketView contract
- `docs/poller-guide.md` — the `Poller` concept with kernel and DPDK examples, single and
  multi-connection patterns, heterogeneous `TcpStream + UdpSocket` on one poller
- `docs/custom-codec.md` — writing a new `StreamCodec` or `DatagramCodec`
- `docs/dpdk-setup.md` — DPDK hugepages, vfio-pci binding, `eph-net-dpdk` environment
- `eph-net-dpdk/docs/dpdk-daemon-deployment.md` — `eph-nicd` daemon model: toml schema, systemd, upgrades, security notes
- `eph-net-dpdk/docs/dpdk-multiprocess.md` — daemon-led multi-process architecture
- `eph-net-dpdk/docs/dpdk-reconnect-pattern.md` — tenant reconnect template for daemon restarts
- `docs/latency-benchmark-fairness.md` — why the kernel-vs-DPDK comparison is structured
  the way it is
- `docs/operations-runbook.md`, `docs/troubleshooting.md`, `docs/production-config.md`
