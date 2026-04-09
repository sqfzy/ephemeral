# eph-dpdk Onboarding Guide

New to the eph-dpdk subproject? This guide walks through the development
environment, the code layout, and the common workflows you will encounter
when hacking on the DPDK backend.

## Development Environment

### Prerequisites

- **Linux** (DPDK requires hugepages and VFIO/UIO drivers).
- **C++23 compiler** — GCC 14+ or Clang 18+. The codebase uses `std::expected`,
  `std::format`, concepts, `if constexpr`, and C++20 NTTP string literals.
- **[xmake](https://xmake.io)** — the monorepo's build system. `uv` is used
  elsewhere in the ecosystem for Python tooling but not by this subproject.
- **DPDK** — provided via vcpkg at the workspace root. The xmake target
  `eph-dpdk` pulls it through `add_packages("dpdk", { public = true })`.
- **aws-lc** for CSPRNG (`RAND_bytes`).
- **spdlog** for logging. `SPDLOG_ACTIVE_LEVEL` gates log output at compile
  time; set via `net_log_level` in the top-level `xmake.lua`.
- **Root privileges** only when running on a real NIC (for hugepage setup and
  driver binding). The unit tests do not need root.

### First build

From the repository root:

```bash
xmake f -m release          # release mode (default is debug)
xmake build eph-dpdk        # header-only target — verifies all headers compile
xmake build                 # build everything (tests, benchmarks, dependents)
```

`eph-dpdk` itself is a `headeronly` target, so building it is very fast — the
meaningful compile work happens inside the test and benchmark targets that
depend on it.

### Verifying the environment

Run the test suites that do not require a real NIC:

```bash
xmake run test_net_header    # core parse / checksum logic
xmake run test_tcp           # TcpSession state machine
xmake run test_reactor       # Reactor dispatch
xmake run test_multicast     # MulticastReceiver
xmake run test_udp           # UdpSender + templates
xmake run test_connector     # endpoint validation, DNS fallback
xmake run test_dns           # DNS codec + security checks
xmake run test_flow_steering
xmake run test_dpdk_platform
xmake run test_arp
xmake run test_eal
```

All tests use fake `rte_mbuf` structures and mock DPDK calls — no EAL init,
no hugepages, no NIC required. There are roughly 550 test cases across these
11 suites.

If you need to exercise a real NIC (latency benchmarks, integration work),
use `scripts/dpdk-setup.sh` to bind the interface and allocate hugepages, and
`scripts/dpdk-teardown.sh` to revert. Both require root.

## Project Architecture

eph-dpdk wraps DPDK behind a C++23-friendly API and organises its headers into
four strictly-downward layers. Reading from top to bottom gives you the
application interface first and the primitives last.

```
Layer 3 (application entry)
  reactor.hpp     — multiplexed RX for 16 TCP + 8 UDP per queue
  connector.hpp   — one-call connect() (Platform + ARP + DNS + TCP + Transport)
  flow_steering.hpp — NIC hardware RX dispatch (rte_flow, RSS)
  types.hpp       — DpdkTransport aliases over eph-transport

Layer 2 (protocol)
  tcp.hpp         — TcpSession<ReorderSlots> state machine
  udp.hpp         — UdpSender, UdpConfig, build_udp_packet()
  arp.hpp         — blocking ARP resolve()
  dns.hpp         — blocking DNS resolve() over DPDK
  multicast.hpp   — MulticastReceiver + MoldUDP64 adapter

Layer 1 (packet processing, net:: namespace)
  packet_core.hpp     — constants, byte order, checksum, ConnectionTuple
  packet_parse.hpp    — ParsedIpHeader, ParsedPacket, ParsedUdpPacket + parse fns
  packet_template.hpp — PacketTemplate (TCP) + UdpPacketTemplate
  net_header.hpp      — 13-line umbrella that includes the three above

Layer 0 (DPDK resource management)
  eal.hpp      — EalGuard RAII over rte_eal_init / rte_eal_cleanup
  platform.hpp — Platform per-port manager (mempool, queues, link-up poll)
```

Dependencies only flow downward. `detail/logger.hpp` is a leaf helper that
every layer can use. `dpdk.hpp` is a convenience umbrella that pulls in
`eal.hpp`, `platform.hpp`, `connector.hpp`, `udp.hpp`, and `types.hpp`.

### Directory layout

```
eph-dpdk/
  include/eph/dpdk/      public headers (everything is header-only)
  include/eph/dpdk/detail/   internal logger factory
  tests/                 unit tests (run without EAL)
  tests/dpdk_test_env.hpp    shared test fixtures and fake mbuf helpers
  benchmarks/            micro-benchmarks (nanosecond-level)
  fuzzers/               libFuzzer target (DNS reply parser)
  scripts/               dpdk-setup.sh / dpdk-teardown.sh
  xmake.lua              subproject build definition
  README.md              feature overview + quick start
  CHANGELOG.md           Keep-a-Changelog formatted history
  summary.md             architecture summary and module map
  docs/ONBOARDING.md     this file
```

### Key entry points

- `eph::dpdk::EalGuard::init(argc, argv)` — initialize DPDK EAL once per
  process. Returns an RAII guard.
- `eph::dpdk::Platform::create(config)` — bring up a port, create its mempool,
  configure queues, and wait for link-up.
- `eph::dpdk::connect(host, endpoint[, opts])` — the simplest client entry
  point. Seven overloads cover hostname / pre-resolved IP, custom
  `TransportConfig`, and reusing an existing Platform.
- `eph::dpdk::Reactor<bool EnableUdp>` — single-thread multiplexed RX for
  multiple connections on a shared queue.
- `eph::dpdk::TcpSession<ReorderSlots>` — low-level TCP session. Use directly
  only when `connect()` is insufficient (e.g. custom handshake, non-default
  transport).
- `eph::dpdk::UdpSender::create(cfg)` — fixed-peer UDP TX handle.
- `eph::dpdk::MulticastReceiver` — UDP multicast RX with RFC 1112 MAC
  management.

## Daily Development

### Building

```bash
xmake build eph-dpdk      # header verification only
xmake build               # everything (tests + benches + dependents)
xmake build test_reactor  # single test binary
```

Release, debug, asan, and tsan modes are defined at the top-level `xmake.lua`:

```bash
xmake f -m release
xmake f -m asan           # address + undefined behavior sanitizer
xmake f -m tsan           # thread sanitizer
```

### Running tests

```bash
xmake run test_tcp        # runs the TcpSession suite
```

After touching any code with coverage, run the affected suites — the user's
global convention is "after any code change, run all tests that cover the
modified code and ensure they pass before considering the task complete".

### Running benchmarks

Benchmarks live under `benchmarks/` and are built as part of the default
`xmake build` target:

```bash
xmake run bench_tcp_header
xmake run bench_udp
xmake run bench_multicast
xmake run bench_pipeline
xmake run bench_dns_codec
xmake run bench_memcpy_compare
xmake run bench_rte_ring_vs_bq
```

The user's convention is to capture a baseline before modifying code that has
benchmarks, then re-run after the change and verify no regression.

### Common tasks

#### Adding a new public API

1. Add the declaration or inline definition to the appropriate header in
   `include/eph/dpdk/`.
2. Add a Doxygen-style `///` or `/** */` comment block: one-line summary,
   parameter descriptions, return value, error conditions, any unsafe pointer
   or lifetime assumptions (critical for DPDK FFI boundaries).
3. Add unit tests under `tests/test_<module>.cpp`. Prefer boundary conditions
   and error paths over happy-path tests.
4. If the new API touches a performance-sensitive path, add a benchmark under
   `benchmarks/`.
5. Update `summary.md` and `CHANGELOG.md` if the change is user-visible.

#### Debugging a test failure

1. Run the failing test with `xmake run test_<suite>` and read the assertion.
2. Rebuild in debug mode: `xmake f -m debug && xmake build test_<suite>`.
3. For memory issues: `xmake f -m asan && xmake build && xmake run test_<suite>`.
4. Turn up spdlog verbosity by rebuilding with a lower `net_log_level` (set
   in the workspace root xmake.lua).

#### Touching DPDK FFI boundaries

- Zero-copy parse views (`ParsedPacket`, `ParsedUdpPacket`) reference memory
  inside the original `rte_mbuf`. They are valid only until the mbuf is freed.
  Document this in any new API that returns such a view.
- Packet templates (`PacketTemplate`, `UdpPacketTemplate`) are not thread-safe
  — `ip_id` is incremented without synchronization. Each TX thread needs its
  own template instance.
- Check NIC capability before enabling any hardware offload
  (`rx_offload_capa` / `tx_offload_capa` in `rte_eth_dev_info`). Setting
  `hw_cksum = true` on a NIC that does not support it produces silently
  corrupt packets.
- Always free mbufs on all error paths inside `tx_burst` / `rx_burst` loops.

#### Working with the Reactor

- `add_connection()` and `add_udp()` must be called **before** `start()`.
  The hot path has no locking by design.
- `mark_reconnected()` is safe to call while the reactor is running, using a
  four-step release/acquire protocol documented in-line in `reactor.hpp`.
- Prefer the layered parse API (`parse_ip_header` → protocol branch →
  `parse_tcp_from_ip` / `parse_udp_from_ip`) over the convenience wrappers
  when you are building new dispatch logic — it is zero-redundancy and the
  reactor already relies on it.

## Code Conventions

### Naming

- Namespaces are lowercase (`eph::dpdk`, `eph::dpdk::net`, `eph::dpdk::arp`,
  `eph::dpdk::dns`).
- Types are `PascalCase`; functions and variables are `snake_case`.
- Template parameters use `PascalCase` (`ReorderSlots`, `EnableUdp`).
- Constants are `kPascalCase` (`kEphemeralPortMin`, `kTcpAck`).
- Private members are trailing-underscore (`snd_nxt_`, `pkt_template_`).

### Error handling

- Fallible functions return `std::expected<T, std::string>` or
  `std::optional<T>`. Exceptions are not used in library code.
- Validation functions return `std::string_view` — empty on success, an error
  description otherwise. The returned views point to string literals and are
  safe to store.
- Every error branch logs with spdlog at `ERROR` or `WARN`. Log messages must
  be actionable — include the relevant parameters and system state, not just
  "error occurred".

### Logging

- Use spdlog via `SPDLOG_LOGGER_*` macros so compile-time log filtering works.
  Never call `spdlog::*` directly on the hot path.
- Each module has its own named logger via `get_logger<LoggerName{"dpdk.xxx"}>()`.
- Log levels: `ERROR` for unrecoverable, `WARN` for degraded, `INFO` for
  lifecycle events, `DEBUG` for normal flow, `TRACE` for per-packet diagnostic
  detail. The active level is gated at compile time via `SPDLOG_ACTIVE_LEVEL`.
- Log on entry/exit of non-trivial functions at `DEBUG`.

### Compile-time philosophy

- Prefer `constexpr` and `consteval` where possible. `internet_checksum`,
  `hton*/ntoh*`, `validate_config`, `config_ok`, `clamp_desc`, and
  `multicast_mac_from_ip` are all `constexpr`.
- Use `static_assert` for invariants that can be checked at compile time
  (see `tests/` for examples exercising constexpr config validation).
- `[[nodiscard]]` everywhere a return value must be checked — allocation
  results, `std::expected` returns, validation results.

### Commits

The repo follows **Conventional Commits** (`feat`, `fix`, `refactor`, `perf`,
`test`, `docs`, `chore`, `bench`, `build`) optionally scoped to a subproject
(`feat(dpdk): ...`). Each commit should be a single logical change that
builds independently.

## Troubleshooting

**"rte_eal_init failed"**
The DPDK EAL could not initialize. Most common causes are missing hugepages
(run `scripts/dpdk-setup.sh` as root), missing VFIO binding, or passing
conflicting `-l` / `-a` arguments.

**"No DPDK ports available"**
The NIC is not bound to a DPDK-compatible driver (igb_uio, uio_pci_generic,
vfio-pci). Use `scripts/dpdk-setup.sh`.

**"rte_pktmbuf_pool_create failed"**
Usually hugepage exhaustion. Check `/proc/meminfo` for `HugePages_Free` and
allocate more via `scripts/dpdk-setup.sh`.

**Tests fail with "CSPRNG not seeded"**
`aws-lc`'s entropy pool could not be initialized. On containers, ensure
`/dev/urandom` is available.

**Build fails on ARM64 with SSSE3 errors**
The `-mssse3` flag must only be set on x86. `xmake.lua` already guards this
via `is_arch("arm64", "arm64-v8a", "aarch64")`. If you see the error, check
that the guard is still present.

**Build fails with "fmt header conflict"**
vcpkg DPDK bundles fmt, which shadows spdlog's vendored copy. `xmake.lua`
works around this by explicitly linking `fmt` and controlling include order.

**Reactor start() returns false**
Either no connections have been registered or the reactor is already running.
Check the return value and the WARN log from `dpdk.reactor`.

## Where to Learn More

- `README.md` — feature list and usage examples.
- `summary.md` — architectural overview, module map, data flow diagrams.
- `CHANGELOG.md` — history of user-visible changes.
- `include/eph/dpdk/*.hpp` — every public type and function has inline
  documentation comments.
- `tests/` — examples of how to construct and exercise each API.
- `benchmarks/` — examples of hot-path usage patterns.
