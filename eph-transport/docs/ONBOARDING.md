# eph-transport — Developer Onboarding

This guide gets a new contributor from a clean clone to a working
`eph-transport` development loop in a few minutes. It covers the
environment, the build, the layout, the daily commands, and the code
conventions in use.

## What `eph-transport` is

`eph-transport` is a header-only C++23 WebSocket / raw-TCP transport
built on a generic `TcpTransport` concept. It unifies kernel-socket
(`eph-net`) and DPDK (`eph-dpdk`) backends behind a single typed
interface and offers three transport variants (`Transport`,
`DirectTxTransport`, `DirectTransport`) trading off threading
complexity for latency. See `summary.md` in this directory for the
architectural overview and `README.md` for the full API surface.

## Prerequisites

- **Compiler**: GCC 14+ or Clang 17+ (C++23: concepts, ranges,
  `std::expected`, `std::format`, structured bindings, CTAD).
- **Build system**: [xmake](https://xmake.io) 2.9+.
- **TLS backend**: aws-lc (installed via xmake's package manager when
  building the parent repo). OpenSSL is accepted as a drop-in for the
  subset used, but aws-lc is the tested default.
- **Linker**: `lld` or `mold` recommended on large builds for link
  speed.
- **Platform**: Linux x86_64 or Linux aarch64 (both tested by CI). The
  TSC helpers require an invariant TSC; `/sys/devices/system/clocksource/*`
  must list `tsc`.

### Installing xmake

```bash
curl -fsSL https://xmake.io/shget.text | bash
```

Or on Arch Linux:

```bash
sudo pacman -S xmake
```

## First build

`eph-transport` is a subproject of the `ephemeral_dev` monorepo — the
build is driven from the repo root, not from this directory.

```bash
# From the monorepo root
cd ~/ephemeral_dev
xmake config --mode=release           # or debug / releasedbg
xmake build eph-transport             # header-only target — validates deps
xmake build -g tests                  # build every test binary
xmake build -g benchmarks             # build every benchmark binary
```

Header-only means `xmake build eph-transport` does not emit an object
file, but it does validate that all transitive deps resolve. The real
compile work happens when you build a consumer — for eph-transport,
the tests and benchmarks in this directory.

### Useful flags

```bash
# Enable per-message TSC timestamps and HdrHistogram latency stats
xmake config --cxxflags="-DEPH_ENABLE_TIMESTAMPS=1"

# Change spdlog compile-time level filter (trace/debug/info/warn/err/crit/off)
xmake config --net_log_level=info     # top-level option

# Debug build with sanitisers
xmake config --mode=debug --sanitise=address,undefined

# Clean build
xmake clean -a
```

### Verifying the environment

```bash
xmake build test_transport_types
xmake run test_transport_types
xmake build test_websocket
xmake run test_websocket
```

All tests use GoogleTest through the `eph-test` rule. A green run on
`test_transport_types` and `test_websocket` means your toolchain,
aws-lc, spdlog, and the monorepo deps are all wired up correctly.

## Repository layout (this subproject)

```
eph-transport/
├── include/eph/transport/
│   ├── transport.hpp              # Transport<>        (threaded variant)
│   ├── direct_tx_transport.hpp    # DirectTxTransport<> (direct TX, RX thread)
│   ├── direct_transport.hpp       # DirectTransport<>   (threadless)
│   ├── presets.hpp                # Default/Small/Large/Evict/Raw aliases
│   ├── transport_types.hpp        # TransportConfig, TransportStats, enums
│   ├── reconnect_policy.hpp       # ReconnectPolicy
│   ├── ws_framer.hpp              # WebSocket MessageFramer adapter
│   ├── raw_framer.hpp             # Pass-through framer
│   └── detail/
│       ├── transport_core.hpp     # Shared state (TCP + TLS + config)
│       ├── tx_worker.hpp          # TX thread + queue + ping scheduling
│       ├── rx_worker.hpp          # RX thread + queue + stats
│       ├── frame_processor.hpp    # WS decode + fragmentation reassembly
│       ├── frame_filter.hpp       # FrameView + make_twophase_filter
│       ├── websocket.hpp          # RFC 6455 encode/decode
│       ├── http.hpp               # Minimal HTTP/1.1 for WS Upgrade
│       ├── tls_session.hpp        # TLS 1.3 handshake (aws-lc BIO)
│       ├── tls_record.hpp         # TlsRecordCrypto composition
│       ├── tls_encryptor.hpp      # AES-GCM write side
│       ├── tls_decryptor.hpp      # AES-GCM read side
│       ├── tls_constants.hpp      # Record layout, nonce construction
│       ├── message_types.hpp      # TxMessage / RxMessage (cache-aligned)
│       └── logger.hpp             # Shared spdlog logger factory
├── tests/                         # Unit and integration tests
├── benchmarks/                    # Micro-benchmarks
├── fuzzers/                       # libFuzzer harnesses
├── docs/
│   └── ONBOARDING.md              # This file
├── README.md                      # Full API reference and usage
├── CHANGELOG.md                   # Release notes
├── summary.md                     # Architecture + module map
├── xmake.lua                      # Build rules
└── .artifacts/                    # Historical bench / test reports (gitignored runtime, archived)
```

## Mental model: the three transports

All three variants share a common core (`TransportCore` + TLS +
WebSocket state machine + `ReconnectPolicy`) and differ only in how
they schedule TX and RX work:

1. **`Transport`** — default threaded variant. `send()` enqueues to a
   lock-free TX SPSC queue; a dedicated TX thread drains the queue,
   builds WS frames, encrypts, and writes to TCP. A dedicated RX
   thread polls TCP, decrypts, decodes, and pushes to an RX SPSC
   queue (or calls `on_message` directly). Use when the app thread
   should not block on network work.
2. **`DirectTxTransport`** — hybrid. `send()` runs synchronously on
   the calling thread (no TX queue, no TX thread). RX still runs on
   a background thread with a queue. Use when TX latency must be
   minimised but receive-side work is fine to offload.
3. **`DirectTransport`** — fully threadless. `send()` runs on the
   calling thread; `poll()` (or `feed_rx()` + `process_pending()`)
   drives RX from the calling thread too. Use inside single-threaded
   event loops (Reactor, io_uring, DPDK poll-mode).

Decide between them by thinking about where the latency budget goes
and who already owns the polling thread. The headers begin with a
compact description of the threading model; start there when reading
the code.

## Key entry points

- `Transport::create(factory, config)` — the factory performs the full
  handshake sequence (TCP → TLS 1.3 → WS Upgrade) and returns a
  `std::expected<std::unique_ptr<Transport>, ConnectionErrorInfo>`.
  This is synchronous; it blocks until the connection is ready.
- `TransportConfig::from_url("wss://host:port/path")` — parse a
  WebSocket URL into a config struct, then set callbacks and tuning
  fields as needed.
- `TransportConfig::validate()` / `warnings()` — call
  before `create()` to surface configuration errors and advisory
  warnings in a single place.
- `ReconnectPolicy::attempt(connect_fn)` — reusable backoff loop,
  templated on any callable returning
  `std::expected<void, ConnectionErrorInfo>`.
- `make_twophase_filter(extractor)` — build a batch frame filter for
  multi-symbol dedup.

## Daily development

### Running tests

```bash
# Build + run a specific test
xmake build test_websocket && xmake run test_websocket

# Build + run every test in the repo
xmake build -g tests
xmake run -g tests

# Filter tests within a binary (GoogleTest flags)
xmake run test_transport_config --gtest_filter=TransportConfig.FromUrl*
```

The user-level CLAUDE.md instructions require running every test that
covers the modified code before considering a change complete — get in
the habit of at least running the relevant test binary after every
meaningful edit.

### Running benchmarks

```bash
xmake build bench_transport_types
xmake run bench_transport_types
```

Benchmarks use [nanobench](https://nanobench.ankerl.com/). Results are
printed to stdout. Before modifying any code that has an associated
benchmark, capture a baseline first, then re-run after the change and
confirm no regression:

```bash
xmake run bench_transport_types > /tmp/baseline.txt
# ... make changes ...
xmake run bench_transport_types > /tmp/after.txt
diff /tmp/baseline.txt /tmp/after.txt
```

### Fuzzing (optional)

```bash
xmake config --fuzzer=libfuzzer
xmake build fuzz_ws_decode
xmake run fuzz_ws_decode -max_total_time=60
```

### Log level

Compile-time filtering through `SPDLOG_ACTIVE_LEVEL` is set via the
monorepo-level `net_log_level` option. Raise it to `trace` or `debug`
when chasing an issue:

```bash
xmake config --net_log_level=trace
xmake build test_websocket
xmake run test_websocket
```

Runtime level changes are not supported — log filtering is strictly
compile-time for performance.

## Common tasks

### Adding a new public API method

1. Add the declaration to the appropriate header under
   `include/eph/transport/`. Every public function must have a
   Doxygen-style `///` comment covering purpose, parameters, return,
   and any preconditions or thread-safety notes.
2. Mark it `[[nodiscard]]` when the return value should not be ignored
   (which is almost always for this codebase — see the many existing
   examples in `transport.hpp`).
3. Mark it `noexcept` unless it can genuinely throw (most hot-path
   methods are noexcept; constructors that build `std::string` are
   not).
4. Add `SPDLOG_LOGGER_*` log statements for error branches and entry
   points — this is non-optional per the project logging policy.
5. Add tests under `tests/` that cover happy path, boundary, and
   error cases. Use a descriptive test name
   (`test_send_with_queue_full_returns_queue_full_error`, not `test1`).
6. Re-run the relevant test binaries and confirm they pass.
7. If the method is on a hot path, add a benchmark to
   `benchmarks/bench_transport_types.cpp` and validate the cost.

### Adding a new TLS feature

All TLS work lives under `include/eph/transport/detail/tls_*.hpp`.
The split is important: `tls_session.hpp` handles the handshake via
aws-lc and exports session keys, after which it is idle.
`tls_encryptor.hpp` and `tls_decryptor.hpp` own the AEAD hot path and
never touch `SSL_*` functions. `tls_record.hpp` composes the two.

Handshake features belong in `tls_session.hpp`. Data-plane features
(record format, nonce construction, sequence limits) belong in
`tls_record.hpp` / `tls_constants.hpp`. Do not blur the line — the
whole point of the split is to keep libssl off the hot path.

### Adding a new frame filter

Implement the `FrameFilterFn` signature and wire it up through
`TransportConfig::on_frame_filter`. See `make_twophase_filter()` in
`detail/frame_filter.hpp` for the reference implementation: stack-only
hash table, O(n), bounded batch size, and exactly two passes over the
frames. New filters should match those constraints — allocation on
the RX thread is a hard no.

### Debugging a reconnect loop

1. Set `net_log_level=debug` and rebuild.
2. Run the test / application — `ReconnectPolicy::attempt()` logs
   every attempt with backoff duration, jitter, and failure reason.
3. If the loop never progresses, inspect
   `config.max_reconnect_attempts` (0 disables auto-reconnect),
   `on_reconnect_attempt` (returning `false` aborts), and whether
   `force_reconnect` is being set.
4. Watch for the `reconnecting` atomic flag on `TransportCore`: it
   gates `stop()`, and if it remains stuck, that is your bug.

## Code conventions

### Style

- C++23 throughout: prefer `std::expected`, `std::format`, concepts,
  ranges, structured bindings, `if constexpr`, `requires`-expressions.
- Header-only — put everything in `include/eph/transport/`; do not add
  `.cpp` files (except tests/benchmarks/fuzzers).
- `.hpp` extension for headers. Internal / implementation details go
  under `detail/` and are not part of the public API.
- 4-space indent, UNIX line endings, no tabs, trailing newline.
- Prefer `auto` when the type is obvious from the RHS; spell it out
  when it improves readability.
- Use `[[nodiscard]]` liberally on non-void returns, especially on
  error-returning APIs.

### Naming

- Types: `PascalCase` (`TransportConfig`, `ReconnectPolicy`).
- Functions / methods: `snake_case` (`send_close`, `try_recv_msg`).
- Constants and enumerators: `kPascalCase` (`kBinary`, `kMaxFrameHeaderLen`).
- Template parameters: descriptive `PascalCase` (`TcpImpl`, `Framer`,
  `MaxPayload`).
- Private / implementation: trailing underscore (`send_direct_`,
  `core_.config`).

### Error handling

- Return `std::expected<T, E>` from functions that can fail. E is an
  enum (see `eph/core/transport_errors.hpp` for
  `ConnectionError`, `SendError`, `FrameError`) plus an info struct
  (`ConnectionErrorInfo`) when a human-readable detail string is
  needed.
- Never throw from hot paths — construct the error and return it.
- Constructors that allocate may throw `std::bad_alloc`; document it
  in the comment.
- Callbacks invoked from worker threads: wrap in try/catch, log the
  exception, and continue. Do not let callback exceptions unwind
  worker threads.

### Logging

- Use the project's shared logger factory
  (`detail::transport_logger()`) — do not call `spdlog::get()` directly.
- Emit `SPDLOG_LOGGER_*` macros, not `SPDLOG_*`, so the compile-time
  level filter applies correctly.
- Log levels: `ERROR` for definite failures, `WARN` for degraded-but-
  working states, `INFO` for lifecycle events (connect / stop /
  reconnect), `DEBUG` for per-iteration state, `TRACE` for
  byte-level detail.
- Messages must be actionable: include the relevant variables. For
  example, log the error message, attempt number, and max attempts
  on a reconnect failure, not just `"reconnect failed"`.

### Testing

- Tests live under `tests/` with one file per unit or feature.
- GoogleTest is the framework of choice via the `eph-test` rule.
- Name tests by scenario, not by function: `Send.QueueFullReturnsQueueFull`.
- Cover boundary conditions (empty payload, max payload, exact
  queue capacity), error paths, and happy path.
- Integration tests that require a real network go under tests that
  depend on `eph-net` (see the top of `xmake.lua`).

### Commit conventions

Conventional Commits (enforced by pre-commit hook):

```
<type>(<scope>): <short summary>

<optional body>
```

Types: `feat`, `fix`, `refactor`, `perf`, `test`, `bench`, `docs`,
`style`, `chore`, `build`. Scopes typically: `transport`, `ws`,
`tls`, `http`, `bench`.

Examples from `git log`:

- `feat(transport): add TransportConfig operator== for value equality`
- `fix(transport): bound WS upgrade response buffer to prevent resource exhaustion`
- `perf(transport): zero-copy WS decode fast-path in DirectTransport::poll()`
- `refactor(transport): extract FrameProcessor, TxWorker, RxWorker components`

Commits should be atomic: one logical change, buildable on its own.

## Frequently asked questions

### Build fails with "cannot find -laws-lc"

Run `xmake require --install aws-lc` from the monorepo root, or just
run `xmake build` at the top level once to let the package manager
fetch all dependencies.

### Tests fail with "RTC not available" or TSC-related errors

The TSC helpers need an invariant TSC. On bare-metal this is always
fine; in VMs / containers it may not be. Disable timestamps for tests
with `-DEPH_ENABLE_TIMESTAMPS=0` (the default) or add a fake clock
provider.

### My new test compiles but segfaults on startup

Check that the test is listed under the `tests/` target pattern in
`xmake.lua`. The per-file rule in `xmake.lua` picks up
`tests/**.cpp` automatically — so a new file in the right location
is enough. A stale cache can confuse the link step: try
`xmake clean -a && xmake build -g tests`.

### The RX thread hangs on shutdown

`Transport::stop()` waits for any in-progress reconnect before joining
threads, up to 5 seconds, then resets the TCP connection to unblock.
If you see the 5 s log, your TCP backend's `reset()` is probably
blocking — check `SocketSession::reset` or the DPDK equivalent.

### How do I measure TX latency?

Enable `-DEPH_ENABLE_TIMESTAMPS=1` at compile time, then read
`TransportStats::tx_latency`, `tx_queue_wait`, and `tx_encode` — each
is an `RttStats` with min / max / mean / p50 / p99 / p999 in
nanoseconds. The breakdown isolates queue wait from encode time
from total.

## Further reading

- `README.md` — the authoritative API reference.
- `summary.md` — architecture overview, module map, and data flow
  diagrams.
- `include/eph/transport/*.hpp` — the actual code, with doc comments
  on every public entity. Start with `transport.hpp` for the
  overall mental model.
- `~/.claude/CLAUDE.md` in the parent repo — the broader
  observability / testing / benchmarking / style guide that all `eph`
  code follows.
