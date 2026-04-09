# eph-net Onboarding Guide

Welcome to `eph-net`, the kernel-side (POSIX socket) networking subproject of
the `ephemeral_dev` monorepo. This guide assumes you're already checked out
at the monorepo root and walks through the things a new contributor needs to
be productive inside `eph-net` specifically.

## Development environment

### Prerequisites

- **xmake** — build system for the whole monorepo (Lua-driven, generates
  compile_commands.json into `build/`).
- **A C++23 compiler** — GCC 14+ or Clang 17+. The top-level `xmake.lua`
  already adds the `libstdc++` link/rpath directories for GCC 14 on Amazon
  Linux 2023 (`/usr/lib/gcc/aarch64-amazon-linux/14`). Other distros with a
  system GCC 14 work out of the box.
- **aws-lc** — OpenSSL-compatible TLS + crypto backend. Declared as an
  optional package; the top-level `xmake.lua` has
  `add_requires("aws-lc", { optional = true })` so xmake can fetch it.
  Required for `eph-net` (HttpClient TLS and hmac.hpp).
- **spdlog** — logging; optional package, but effectively required.
- **google/benchmark** — optional, needed only if you run `bench_*` targets.
- **GoogleTest** — fetched by xmake (`add_requires("gtest", ...)`).
- **libFuzzer** (ships with clang) — only needed if you build `fuzz_http_parse`.

### First build

From the monorepo root:

```bash
# Debug build (SPDLOG_LEVEL_TRACE, no optimization):
xmake f -m debug
xmake build eph-net

# Or build everything that depends on eph-net transitively:
xmake build
```

Because `eph-net` itself is `set_kind("headeronly")`, `xmake build eph-net`
only validates metadata and installs headers — actual code generation
happens when a dependent test / benchmark / example pulls the headers in.
If you want to be sure nothing is syntactically broken, build one of the
per-file test targets:

```bash
xmake build test_socket_transport
xmake build test_http_client
xmake build test_rate_limiter
```

### Verify your environment

```bash
# Unit tests for eph-net (each file becomes a target):
xmake run test_tcp_concept
xmake run test_socket_transport
xmake run test_http
xmake run test_http_client
xmake run test_hmac
xmake run test_circuit_breaker
xmake run test_rate_limiter
xmake run test_gateway
xmake run test_kill_switch
xmake run test_proxy
xmake run test_websocket
xmake run test_transport
xmake run test_transport_types
xmake run test_tls_record
xmake run test_framer

# Or run whatever xmake picks up via GoogleTest discovery:
xmake test
```

Sanitizer modes (from top-level `xmake.lua`):

```bash
xmake f -m asan    # AddressSanitizer + UBSan
xmake f -m tsan    # ThreadSanitizer
xmake build
xmake run test_gateway   # exercise multi-threaded code under tsan
```

## Project architecture (the 5-minute tour)

`eph-net` sits in the middle of a dependency chain:

```
eph-core  ->  eph-utils / eph-containers  ->  eph-transport  ->  eph-net
                                                               |
                                                               v
                                                           eph-dpdk (sibling)
```

`eph-transport` defines the protocol-agnostic `Transport<Tcp, Framer, ...>`
engine. `eph-net` provides **one** of the two backends for its `TcpTransport`
concept: a POSIX-sockets implementation called `SocketTransport`.
`eph-dpdk` provides the other (kernel-bypass). Anything above the concept
— WS framing, TLS, reconnect logic, stats — is shared.

On top of that raw transport, `eph-net` also hosts the exchange-facing
"operational" components that every real trading client wants but aren't
actually part of the transport: `HttpClient` (sync REST), `hmac.hpp`
(request signing), `CircuitBreaker`, `RateLimiter`, `Gateway`
(multi-connection lifecycle), `KillSwitch` (coordinated shutdown), and
`proxy.hpp` (SOCKS5 / HTTP CONNECT).

### Directory layout

```
eph-net/
├── include/eph/
│   ├── net.hpp                     # umbrella convenience header
│   └── net/
│       ├── socket_config.hpp       # SocketConfig
│       ├── socket_transport.hpp    # SocketTransport (TcpTransport impl)
│       ├── socket_connect.hpp      # connect(url, ...) + preset aliases
│       ├── http_message.hpp        # pure HTTP types (no I/O)
│       ├── http_client.hpp         # sync HTTP/1.1 + TLS
│       ├── hmac.hpp                # HMAC-SHA256 (hex + base64 + verify)
│       ├── circuit_breaker.hpp
│       ├── rate_limiter.hpp
│       ├── gateway.hpp
│       ├── kill_switch.hpp
│       └── proxy.hpp
├── tests/                          # GoogleTest files, one target per file
├── benchmarks/                     # google/benchmark files, one target per file
├── fuzzers/                        # libFuzzer harnesses
├── xmake.lua                       # headeronly + per-file test/bench targets
├── README.md
├── CHANGELOG.md
├── summary.md
└── docs/ONBOARDING.md              # you are here
```

### Key entry points

- **`eph::net::connect(url, modifier?, sock_cfg?)`** — the most common
  starting point. Parses a `ws://` / `wss://` URL, runs a user-supplied
  modifier on the `TransportConfig`, validates, and returns a connected
  `Transport<SocketTransport, WsFramer, ...>`.
- **`eph::net::SocketTransport`** — use directly when you want a raw
  kernel TCP connection without the `Transport<>` engine on top.
- **`eph::net::HttpClient`** — synchronous HTTP/1.1 + TLS client for
  REST calls. Not for hot paths — typical latency is 1–10 ms per
  request.
- **`eph::net::hmac_sha256*`** — signing + constant-time verification for
  Binance / Bybit (hex) and OKX (base64).
- **`eph::net::{CircuitBreaker, RateLimiter, Gateway, KillSwitch}`** —
  operational primitives that wrap whatever transport you use.
- **`eph::net::proxy::make_proxied_factory(...)`** — inject SOCKS5 or HTTP
  CONNECT tunneling into `Transport<>::create(...)`.

## Daily development

### Build

```bash
xmake build eph-net         # header validation only
xmake build                 # everything
xmake build test_gateway    # single test
xmake build bench_hmac      # single benchmark
```

### Test

```bash
xmake run test_gateway      # single test
xmake test                  # discovered tests
```

After any code change, run every test that covers the modified code and
ensure they pass before calling the task complete — this is the
project-wide rule in `~/.claude/CLAUDE.md`, and benchmarked hot paths
additionally require a before/after benchmark comparison.

### Benchmark

```bash
# Establish a baseline BEFORE modifying benchmarked code:
xmake run bench_rate_limiter --benchmark_format=json > /tmp/rl.baseline.json

# After the change:
xmake run bench_rate_limiter --benchmark_format=json > /tmp/rl.after.json

# Then eyeball or diff the two JSON files.
```

Available benches: `bench_socket_config`, `bench_hmac`,
`bench_circuit_breaker`, `bench_rate_limiter`, `bench_gateway`,
`bench_kill_switch`, `bench_http_client`, `bench_proxy`, `bench_ws`,
`bench_tls`, `bench_rx_pipeline`, `bench_transport_pipeline`,
`bench_control_plane`.

### Common tasks

#### Adding a public function to a header

1. Write a `/// @brief` doc comment with `@param` / `@return` / `@note`.
2. Cover both the happy path and at least one error/boundary case in
   `tests/test_<module>.cpp`.
3. If the function sits on a hot path (`send`, `poll_rx`, `try_acquire`,
   etc.) add a matching benchmark and compare against baseline.
4. Run `xmake run test_<module>` and make sure the build is clean with
   both `-m debug` and `-m release`.
5. Run `xmake run test_<module>` under `-m asan` (or `-m tsan` for
   thread-sensitive code) if you touched anything involving shared state.

#### Adding a new operational component

The existing components follow a template you should mirror:

- **`struct Config` inside the class**, with fields that are defaulted to
  safe production values.
- **`constexpr std::string_view validate() const noexcept`** returning
  an empty `string_view` on success or a human-readable error on failure
  (so the caller can use it in `std::unexpected(err)` directly).
- **`std::vector<std::string> warnings() const`** for non-fatal
  misconfigurations (too-small timeouts, obviously-wrong burst sizes).
- **`std::string dump() const`** and **`std::string to_json() const`** for
  logging and monitoring.
- **Defaulted `operator==`** so Configs can be compared in tests.
- **`std::formatter`** specialization for `spdlog`-friendly output.
- **A lazily-initialized spdlog logger** in `detail::<component>_logger()`.
- **`SPDLOG_LOGGER_DEBUG`** on entry / exit of non-trivial methods,
  `SPDLOG_LOGGER_ERROR` on every error branch, `SPDLOG_LOGGER_WARN` for
  advisory conditions, `SPDLOG_LOGGER_TRACE` on the hot path only when
  absolutely necessary.

#### Debugging a failing test

1. `xmake f -m debug && xmake build test_<name>` — make sure you're on
   the debug build (optimization off, log level trace).
2. Run it standalone: `xmake run test_<name> --gtest_filter='*<scenario>*'`.
3. If it smells like a data race or use-after-free, rebuild under
   `-m asan` or `-m tsan` and re-run.
4. Check the commit log for the affected module (`git log -- include/eph/net/<file>.hpp`)
   — several of the current tests are regression tests locked to a
   specific commit (e.g. `test_gateway.cpp` has tests guarding the
   `dump()` deadlock fix from commit `fa9bbf9`).

#### Adding a benchmark

1. Drop a new `bench_<component>.cpp` into `benchmarks/`. The top of the
   file should pull `#include <benchmark/benchmark.h>` and include the
   header under test.
2. `xmake build` will pick it up automatically via the
   `os.files("benchmarks/**.cpp")` loop in `xmake.lua`.
3. `xmake run bench_<component>` to run it.
4. Commit in a `bench(net): ...` prefixed commit per the project's
   convention.

## Code conventions

### Naming

- **Types**: `PascalCase` (`SocketTransport`, `CircuitBreaker`).
- **Functions / methods**: `lower_snake_case` (`hmac_sha256_hex`,
  `poll_rx_for`).
- **Member variables**: `trailing_underscore_` (`config_`, `fd_`,
  `state_`).
- **Constants**: `kCamelCase` with a `k` prefix (`kKillSwitchMaxTransports`,
  `kEnableTimestamps`).
- **Enums**: scoped, `PascalCase` enumerators (`CircuitState::HalfOpen`,
  `ConnHealth::Disconnected`).
- **Concepts**: `PascalCase` (`Stoppable`, `GatewayManageable`,
  `TcpTransport`).
- **Detail namespace**: `namespace detail { ... }` for internal helpers
  that should not be part of the public API but can't be hidden in a
  `.cpp` (this is a header-only library).

### Error handling

- **`std::expected<T, std::string>`** is the default error channel for
  anything that can fail at runtime (`connect`, `send`, `poll_rx`,
  `hmac_sha256`, `parse_http_response`, …). Error strings should be
  actionable — include context like parameter values and syscall errno
  text (`strerror(errno)`).
- **`std::expected<T, ConnectionErrorInfo>`** on the `connect*` family —
  `ConnectionErrorInfo` from `eph-transport` carries a structured
  `ConnectionError` enum plus a detail string.
- **No exceptions** on the data plane. Configurator paths may throw, but
  the `send` / `poll_rx` / `try_acquire` hot path is noexcept and
  allocation-free.
- **`std::unexpected(std::format(...))`** is the idiomatic way to return
  an error.
- **`[[nodiscard]]`** on every function whose return value carries status.

### Logging

- Use the project-wide spdlog conventions from `~/.claude/CLAUDE.md`:
  leveled logging on all non-trivial functions, DEBUG at entry/exit,
  actionable messages that include variable values, ERROR on every error
  branch, `SPDLOG_ACTIVE_LEVEL` for compile-time filtering.
- Each subsystem has its own logger (`net.socket`, `net.http_client`,
  `net.hmac`, `net.circuit_breaker`, `net.rate_limiter`, `net.gateway`,
  `net.kill_switch`, `net.proxy`) created lazily by a
  `detail::<subsys>_logger()` function. Do not create loggers at static
  init time — spdlog needs the main thread running first.
- Prefer the `SPDLOG_LOGGER_*` macros (not `spdlog::*` directly) so each
  subsystem's logger is targeted and the level filter applies.

### Testing

- Integration-style tests for I/O code; pure unit tests for pure logic.
- One GoogleTest file per header, one xmake target per file.
- Test names describe the scenario: `TEST(CircuitBreakerTest,
  TripsAfterThresholdFailures)`, not `TEST(CircuitBreaker, Test1)`.
- Always cover at least one boundary and one error path per new function.
- When fixing a bug, add a regression test that fails **without** the fix
  and passes **with** it.

### Commits

- Conventional Commits with a module scope — the convention visible in
  `git log --oneline`:
  - `feat(net): add Gateway::is_all_healthy()`
  - `fix(net): prevent deadlock in Gateway::dump() by calling callbacks outside lock`
  - `test(net): add const-correctness tests for RateLimiter query methods`
  - `bench(net): add const-ref benchmarks for RateLimiter mutable refactoring`
  - `refactor(net): templatize Gateway::for_each to avoid std::function overhead`
  - `docs(net): ...` / `chore(net): ...` / `perf(net): ...`
- Each commit should represent one logical change and be individually
  buildable.

## FAQ / Troubleshooting

### "`TcpTransport<SocketTransport>` concept is not satisfied"

If you modify `SocketTransport`'s public interface, the `static_assert`
at the bottom of `socket_transport.hpp` will catch it immediately:

```cpp
static_assert(TcpTransport<SocketTransport>,
    "SocketTransport must satisfy TcpTransport concept");
```

Run `xmake build eph-net` (or any dependent test) and GCC / Clang will
tell you which requirement is unsatisfied. The concept lives in
`eph-core::tcp_concept.hpp`.

### "`SO_BINDTODEVICE(...)` failed: Operation not permitted"

`SO_BINDTODEVICE` requires `CAP_NET_RAW` or root. Either run the test
binary under `sudo`, grant the capability
(`sudo setcap cap_net_raw+ep <binary>`), or leave `bind_device` empty.

### DNS hangs / takes 30+ seconds

`SocketTransport::connect()` already bounds DNS at the user-supplied
timeout via `std::async` + `wait_for()`, so if a test is hanging it's
probably waiting on the async thread's lingering `getaddrinfo()` call on
process exit. Prefer the loopback / IP-literal path in tests
(`127.0.0.1:<port>`) to skip DNS entirely.

### `xmake run bench_*` fails with missing benchmark package

The top-level `xmake.lua` declares `add_requires("benchmark", { optional = true })`;
if it's not installed, xmake will refuse to build the bench targets. Install
it via your system package manager or let xmake fetch it.

### TLS handshake fails on a clean checkout

`HttpClient` uses `SSL_CTX_set_default_verify_paths()` when no
`ca_cert_path` is set. On some minimal containers there are no default CA
paths; either point `HttpClient::Config::ca_cert_path` at a CA bundle
(e.g. `/etc/ssl/certs/ca-bundle.crt`) or pass `use_tls = false` if you
only need to talk to plain HTTP test servers.

### New feature doesn't appear in the example binaries

The example targets (`ws_echo_client`, `minimal_ws_client`,
`production_client`, etc.) live at the monorepo root's `xmake.lua`. If
you add a new public API you want to showcase, either extend an existing
example or add a fresh target there — nothing in `eph-net/xmake.lua`
builds top-level examples.
