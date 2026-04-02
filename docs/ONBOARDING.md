# Developer Onboarding Guide

This guide covers everything needed to build, test, and contribute to **ephemeral** -- a header-only C++23 ultra-low-latency WebSocket/TLS client library for high-frequency trading.

Current version: **1.0.0** (see `eph/version.hpp`).

---

## 1. Prerequisites

### Required tools

| Tool | Minimum version | Purpose |
|------|----------------|---------|
| GCC | 13+ | C++23 compiler (`std::expected`, `std::format`) |
| xmake | 2.8+ | Build system and package manager |
| git | any recent | Version control |

### Installation on Arch Linux (WSL)

```bash
# Compiler
sudo pacman -S gcc

# Verify C++23 support
g++ --version   # Must be >= 13

# xmake
sudo pacman -S xmake
# Or via the official installer:
# curl -fsSL https://xmake.io/shget.text | bash

# Google Test and Google Benchmark are fetched automatically by xmake.
# No manual package installation needed for core dependencies.
```

### Optional dependencies

| Package | Purpose | Install |
|---------|---------|---------|
| aws-lc | TLS 1.3 (required for real network connections) | Auto-fetched by xmake |
| DPDK | Kernel-bypass networking backend | `vcpkg::dpdk` (see `docs/dpdk-setup.md`) |
| numactl | NUMA-aware memory allocation | `sudo pacman -S numactl` |
| tabulate | Pretty-printed benchmark tables | Auto-fetched by xmake |
| spdlog | Structured logging | Auto-fetched by xmake |

All packages listed in `xmake.lua` with `optional = true` are fetched automatically when available. The core library compiles without them.

---

## 2. Clone and first build

```bash
# Clone the repository
git clone https://github.com/sqfzy/ephemeral.git
cd ephemeral

# Configure and build (release mode -- optimized with -Ofast)
xmake -m release

# Or debug mode (SPDLOG_ACTIVE_LEVEL set to TRACE, debug symbols enabled)
xmake -m debug
```

On first run, xmake downloads and builds dependencies (gtest, spdlog, benchmark, aws-lc). This may take a few minutes.

### Verify the build

```bash
# Build and run a single test to confirm everything works
xmake build -g tests
xmake run test_version
```

### Compile commands for editor integration

The build automatically generates `build/compile_commands.json` (via the `plugin.compile_commands.autoupdate` rule). Point your editor/LSP to this file.

For Neovim with clangd, add to your project-local config:
```
--compile-commands-dir=build
```

---

## 3. Running tests

All test targets belong to the `tests` group. Each test file produces one binary named after the file (e.g., `tests/fix/test_fix.cpp` becomes target `test_fix`).

```bash
# Build all tests
xmake build -g tests

# Run a specific test
xmake run test_fix
xmake run test_websocket
xmake run test_bounded_queue

# Run all non-DPDK tests (DPDK tests require hardware and hugepages)
for t in test_alignment test_audit_log test_bounded_queue \
         test_bounded_queue_bytes test_circuit_breaker test_cpu \
         test_ema test_evicting_queue test_evicting_queue_bytes \
         test_execution_report test_fix test_fix_orders \
         test_fix_session test_framer test_gateway test_hdr_histogram \
         test_hmac test_http test_http_client test_hugepage \
         test_itch_adapter test_kill_switch test_metrics_concept \
         test_order_manager test_position test_proxy test_rate_limiter \
         test_record test_recorder test_ring_buffer test_risk_check \
         test_socket_transport test_system_stats test_tcp_concept \
         test_time test_timestamp test_tls_record test_transport \
         test_transport_types test_version test_websocket \
         test_binance_adapter; do
    xmake run "$t" 2>/dev/null && echo "PASS: $t" || echo "FAIL: $t"
done
```

Test files live alongside the module they cover:

| Directory | Module tested |
|-----------|--------------|
| `tests/containers/` | eph-containers (queues, ring buffer) |
| `tests/core/` | eph-core (metrics concept) |
| `tests/fix/` | eph-fix (FIX parser, builder, session, orders, risk) |
| `tests/itch/` | eph-itch (ITCH parser) |
| `tests/json/` | eph-json (JSON parser) |
| `tests/net/` | eph-net (transport, TLS, WebSocket, HTTP, framers) |
| `tests/utils/` | eph-utils (CPU, time, histogram, alignment, hugepage) |
| `tests/book/` | eph-book (ArrayBook, ITCH adapter, Binance adapter) |
| `tests/dpdk/` | eph-dpdk (requires DPDK hardware) |

---

## 4. Architecture overview

### Module responsibilities

```
eph-core          Shared abstractions: TcpTransport concept, error types,
                  framer concept, metrics concept. Zero external dependencies
                  beyond spdlog. Every other module depends on this.

eph-utils         System utilities: CPU pinning, NUMA, hugepages, timestamps,
                  HDR histogram, audit log, EMA, performance recording.

eph-containers    Lock-free data structures: BoundedQueue (SPSC fixed-size),
                  EvictingQueue (SPSC with overwrite), RingBuffer.
                  Used internally by Transport for TX/RX queues.

eph-net           Networking layer: SocketTransport (POSIX TCP backend),
                  TLS 1.3 (via aws-lc), WebSocket client, HTTP client,
                  pluggable framers, Gateway (multi-connection manager),
                  KillSwitch, CircuitBreaker, RateLimiter, proxy support.

eph-dpdk          DPDK kernel-bypass TCP backend: userspace TCP/IP stack,
                  ARP, DNS, EAL init, flow steering, Reactor (multi-conn).
                  Drop-in replacement for SocketTransport via the
                  TcpTransport concept.

eph-fix           FIX protocol: zero-copy parser, message builder,
                  session management, order types, position tracking,
                  risk checks, execution reports.

eph-itch          ITCH/OUCH protocol: parser (1.4 ns/msg), MoldUDP64
                  transport, SoupBinTCP framing.

eph-json          JSON parser: zero-allocation streaming parser optimized
                  for market data (Binance adapters included).

eph-book          Order book: ArrayBook (fixed-size sorted array, cache-
                  friendly for shallow books), MapBook (arbitrary depth),
                  exchange adapters (Binance JSON, ITCH).
```

### Dependency graph

```
                    eph-core
                   /    |    \
             eph-utils  |   eph-fix
                |       |   eph-itch
          eph-containers|   eph-json
                 \      |   eph-book (standalone, uses spdlog only)
                  \     |
                  eph-net
                    |
                  eph-dpdk
```

Key relationships from `xmake.lua`:
- `eph-utils` depends on `eph-core`
- `eph-containers` depends on `eph-utils`
- `eph-net` depends on `eph-core`, `eph-utils`, `eph-containers`
- `eph-dpdk` depends on `eph-core`, `eph-utils`, `eph-containers` (includes `eph-net` headers directly to avoid aws-lc conflicts)
- `eph-fix` depends on `eph-core` only (not `eph-net`, to avoid aws-lc/OpenSSL header conflicts with DPDK)
- `eph-itch` depends on `eph-core` only
- `eph-json` depends on `eph-core` only
- `eph-book` is standalone (spdlog only, no eph-core dep)

### Core design pattern: concept-based polymorphism

The library uses C++20/23 concepts instead of virtual dispatch for zero-overhead abstraction:

- **`TcpTransport` concept** (`eph/core/tcp_concept.hpp`): Any TCP backend (POSIX sockets, DPDK, loopback) must satisfy `connect()`, `send()`, `poll_rx()`, `close()`, etc. `Transport<TcpImpl>` is monomorphized at compile time.

- **`MessageFramer` concept** (`eph/core/framer_concept.hpp`): Pluggable wire format (WebSocket, raw TCP, length-prefix, FIX, ITCH). Transport decodes frames through whichever framer you pick at compile time.

- **`ErrorEnum` concept** (`eph/core/error_traits.hpp`): All error enums provide ADL `error_name()` for `std::format` integration.

### Transport variants

The library provides three transport classes, each with a different threading model. All compose the same internal building blocks: `TransportCore` (connection + framing), `TxWorker` (send loop), `RxWorker` (receive loop), and `ReconnectPolicy`.

| Class | Threads | Use case |
|-------|---------|----------|
| `Transport` | TX thread + RX thread (default) | General-purpose; app calls `send()`/`recv()` non-blocking |
| `DirectTxTransport` | RX thread only | App sends directly on its thread; lower latency for TX-heavy paths |
| `DirectTransport` | None (app polls) | Full control; app drives both TX and RX in its own event loop |

```
Transport (default):
  Application thread:  send() / recv()  (non-blocking, via SPSC queues)
  TX thread:           busy-poll SPSC queue -> WS frame -> TLS encrypt -> TCP send
  RX thread:           busy-poll TCP rx -> TLS decrypt -> WS decode -> SPSC queue

DirectTxTransport:
  Application thread:  send() goes directly to wire (no TX queue)
  RX thread:           busy-poll TCP rx -> TLS decrypt -> WS decode -> SPSC queue

DirectTransport:
  Application thread:  send() and poll_rx() both execute inline (no threads)
```

Communication between threads (where applicable) uses the lock-free `BoundedQueue` from eph-containers.

Headers: `eph/transport/transport.hpp`, `eph/transport/direct_tx_transport.hpp`, `eph/transport/direct_transport.hpp`. Internal composition: `transport_core.hpp`, `tx_worker.hpp`, `rx_worker.hpp`, `reconnect_policy.hpp`.

---

## 5. Key entry points

### For reading the codebase

| What you want to understand | Start here |
|-----------------------------|-----------|
| How connections are established | `eph/transport/transport.hpp` -- `Transport::create()` (also `DirectTxTransport`, `DirectTransport`) |
| The TCP abstraction all backends implement | `eph/core/tcp_concept.hpp` -- `TcpTransport` concept |
| POSIX socket backend | `eph/net/socket_transport.hpp` |
| DPDK backend | `eph/dpdk/tcp.hpp` |
| WebSocket framing | `eph/transport/ws_framer.hpp` and `eph/transport/websocket.hpp` |
| TLS 1.3 layer | `eph/transport/tls_session.hpp` and `eph/transport/tls_record.hpp` |
| Multi-connection management | `eph/net/gateway.hpp` -- `Gateway` class |
| FIX protocol | `eph/fix/parser.hpp` (read) and `eph/fix/builder.hpp` (write) |
| ITCH protocol | `eph/itch/parser.hpp` and `eph/itch/messages.hpp` |
| JSON market data parsing | `eph/json/parser.hpp` |
| Order book | `eph/book/array_book.hpp` (fixed-size) and `eph/book/map_book.hpp` (dynamic) |
| Error handling pattern | `eph/core/error_traits.hpp` -- `ErrorEnum` concept |
| Lock-free queues | `eph/containers/bounded_queue.hpp` |

### Example programs

Located in `examples/`. Build with `xmake build <target>`:

| Target | Description |
|--------|------------|
| `minimal_ws_client` | Simplest WebSocket connection |
| `production_client` | Production-ready client with reconnect and error handling |
| `ws_echo_client` | WebSocket echo with latency measurement |
| `ws_via_proxy` | Connecting through HTTP/SOCKS proxy |
| `framer_showcase` | Demonstrates pluggable framer system |
| `fix_trading_demo` | FIX protocol order entry |
| `itch_feed_demo` | ITCH market data feed handler |
| `binance_book` | Live Binance order book via WebSocket + JSON + ArrayBook |
| `simple_hft` | HFT-style client with timestamp instrumentation |
| `spsc_queue_demo` | Lock-free queue usage |
| `perf_tuning_basics` | CPU pinning, NUMA, hugepage utilities |
| `dpdk_quickstart` | DPDK backend setup (requires DPDK) |

---

## 6. Daily development tasks

### Adding a new feature

1. Identify which module the feature belongs to.
2. Add header files under `<module>/include/eph/<module>/`.
3. All modules are header-only -- there are no `.cpp` source files to compile for the library itself.
4. Write tests under `tests/<module>/test_<feature>.cpp`.
5. Register the test target in `xmake.lua` (auto-generated if it follows the `tests/<module>/` directory convention, or add a manual target for cross-module dependencies).
6. Build and run:
   ```bash
   xmake build test_<feature>
   xmake run test_<feature>
   ```

### Running benchmarks

Benchmark targets belong to the `benchmarks` group. They use Google Benchmark.

```bash
# Build all benchmarks
xmake build -g benchmarks

# Build and run a specific benchmark
xmake build bench_fix_parse
xmake run bench_fix_parse

# Available micro-benchmarks:
#   containers: bench_bq_pushpop, bench_bq_pingpong, bench_bq_batch,
#               bench_bq_throughput, bench_eq_pushpop, bench_eq_pingpong,
#               bench_eq_throughput
#   fix:        bench_fix_parse
#   itch:       bench_itch_parse
#   json:       bench_json_parse
#   net:        bench_rx_pipeline, bench_tls, bench_transport_pipeline, bench_ws
#   utils:      bench_time
#   book:       bench_array_book
#
# End-to-end latency benchmarks (benchmarks/latency/):
#   bench_market       -- mock WS server -> market data parse latency
#   bench_order_rtt    -- mock WS server -> order round-trip latency
#   (DPDK variants: bench_market_dpdk, bench_order_rtt_dpdk)
#
# These use a self-contained mock WS server (benchmarks/latency/mock/)
# and have no external dependencies (no live exchange connection needed).
#
# For dual-NIC fair comparison (POSIX vs DPDK on same machine):
#   scripts/bench_latency.sh
```

**Important**: Before modifying performance-critical code, run the relevant benchmarks to establish a baseline. After your change, re-run and compare. See `benchmarks/METRICS.md` for baseline data.

### Debugging

**Log levels**: In debug mode, `SPDLOG_ACTIVE_LEVEL` is set to `SPDLOG_LEVEL_TRACE`, enabling all log output. In release mode, it is set to `SPDLOG_LEVEL_INFO`. This is a compile-time filter.

At runtime, set the minimum displayed level:
```cpp
spdlog::set_level(spdlog::level::debug);  // Show debug and above
spdlog::set_level(spdlog::level::trace);  // Show everything
```

**Build in debug mode**:
```bash
xmake -m debug
xmake build test_transport
xmake run test_transport
```

**GDB/LLDB**: Examples are built with `-fno-omit-frame-pointer` and debug symbols for accurate stack traces:
```bash
xmake build ws_echo_client
gdb --args build/linux/x86_64/debug/ws_echo_client
```

**Perf profiling** (Linux):
```bash
xmake build -m release bench_fix_parse
perf record -g xmake run bench_fix_parse
perf report
```

---

## 7. Code conventions

### Error handling: `std::expected`

All fallible operations return `std::expected<T, E>` instead of throwing exceptions. Error types are enums satisfying the `ErrorEnum` concept (ADL `error_name()` function).

```cpp
auto result = Transport<SocketTransport>::create(factory, cfg);
if (!result) {
    SPDLOG_ERROR("Connection failed: {}", result.error());
    return;
}
auto& transport = *result;
```

Test targets define `SPDLOG_NO_EXCEPTIONS` to catch accidental throws.

### Logging with spdlog

Use compile-time filtered macros, not runtime `spdlog::info(...)`:

```cpp
SPDLOG_TRACE("Entering poll loop, fd={}", fd);          // Hot path tracing
SPDLOG_DEBUG("TCP connected to {}:{}", host, port);     // Function entry/exit
SPDLOG_INFO("Transport started, {} connections", n);     // Operational events
SPDLOG_WARN("TX queue 90% full, depth={}", depth);      // Approaching limits
SPDLOG_ERROR("TLS handshake failed: {}", err.detail);   // All error branches
```

Rules:
- All non-trivial functions must include leveled logging.
- Log all error branches, external I/O, and key function entry/exit (DEBUG level).
- Log messages must be actionable: include variable values, function arguments, system state.
- `SPDLOG_ACTIVE_LEVEL` controls compile-time filtering (TRACE in debug, INFO in release).

### Modern C++23 features

The project requires and uses:
- **`std::expected`** for error handling (no exceptions)
- **`std::format`** for string formatting
- **Concepts** (`TcpTransport`, `MessageFramer`, `ErrorEnum`) for zero-overhead polymorphism
- **Structured bindings** where appropriate
- **`constexpr` / `consteval`** for compile-time computation (see `version.hpp`)
- **`std::span`** for non-owning buffer views
- **Designated initializers** for config structs

### Header-only design

Every module is `set_kind("headeronly")` in xmake. All implementation lives in `.hpp` files. This ensures full inlining and monomorphization at the call site -- critical for the zero-overhead abstraction goal.

### Comments

- Explain **why**, not what. The code shows what; comments reveal intent.
- Every header file starts with a `/// @file` doc comment describing the file's purpose.
- Document non-obvious design decisions (e.g., why `eph-fix` depends on `eph-core` instead of `eph-net`).

### Testing

- Test boundary conditions and error paths, not just the happy path.
- Test names describe the scenario: `test_parse_empty_input_returns_error`, not `test1`.
- After any code change, run all tests covering the modified module.

---

## 8. Common issues and troubleshooting

### Build fails: "C++23 not supported by current compiler"

The `eph-utils` target runs a compile-time check for `std::expected` and `std::format`. If it fails:

```bash
# Check your GCC version
g++ --version

# On Arch, ensure you have gcc >= 13
sudo pacman -S gcc

# If using an alternative compiler, set it explicitly
xmake f --toolchain=gcc-13
xmake -m release
```

### xmake cannot find packages

```bash
# Clean package cache and re-fetch
xmake require --force
xmake -m release
```

### aws-lc build fails

aws-lc is optional. If it fails to build (common on some ARM/WSL configurations), you can still build and test modules that do not require TLS:

```bash
# Build only non-TLS targets
xmake build test_fix
xmake build test_itch
xmake build test_bounded_queue
```

### Tests fail with "address already in use"

Some networking tests (`test_socket_transport`, `test_transport`) bind to local ports. If a previous test run crashed, the port may still be in TIME_WAIT:

```bash
# Wait 60 seconds and retry, or pick a different port
# Check what's bound:
ss -tlnp | grep <port>
```

### DPDK tests fail or won't compile

DPDK is optional and requires specific hardware setup. See `docs/dpdk-setup.md` for:
- Hugepage allocation
- NIC binding to DPDK-compatible driver
- EAL initialization

DPDK tests are skipped by default (they are not part of the standard test loop). Only run them on machines with DPDK-compatible NICs.

### OpenSSL/aws-lc header conflicts

If you see conflicting `openssl/*.h` includes when building targets that combine `eph-net` (aws-lc) and `eph-dpdk` (vcpkg DPDK, which pulls in OpenSSL):

This is a known issue. The architecture deliberately keeps `eph-fix` and `eph-itch` dependent on `eph-core` (not `eph-net`) to avoid this. For DPDK targets, `eph-dpdk` includes `eph-net` headers directly without inheriting the aws-lc package dependency.

If you hit this in a new target, check that your `add_deps()` does not transitively pull in both aws-lc and vcpkg OpenSSL.

### Log output is missing in release builds

Release mode sets `SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_INFO`. TRACE and DEBUG macros are compiled out entirely. Build in debug mode to see full log output:

```bash
xmake -m debug
```

---

## Further reading

| Document | Location |
|----------|----------|
| Troubleshooting (error codes) | `docs/troubleshooting.md` |
| DPDK setup | `docs/dpdk-setup.md` |
| Production configuration | `docs/production-config.md` |
| Operations runbook | `docs/operations-runbook.md` |
| Multi-connection patterns | `docs/multi-connection.md` |
| Custom framer guide | `docs/custom-framer.md` |
| Binance protocol notes | `docs/binance-protocols.md` |
| Benchmark metrics baseline | `benchmarks/METRICS.md` |
