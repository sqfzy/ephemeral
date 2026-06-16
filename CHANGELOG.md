# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

> **Note** — this top-level CHANGELOG carries two distinct sections:
> the current `[Unreleased]` (post-v3.3 surface, kept summary-only —
> the per-module `eph-<name>/CHANGELOG.md` files remain authoritative
> and structurally consistent for that module's history), and a
> historical `[v2.x pre-v3.3 archive]` section preserved verbatim
> below it so the API-evolution record stays auditable.
>
> The post-v3.3 module reshape
> (`.artifacts/design-eph-v3.3-architecture-20260410.md`) retired
> `eph-transport`, `Gateway`, `CircuitBreaker`, `SocketTransport`,
> and the per-domain error enums (`SendError`, `ConnectionError`,
> `TcpError`, `WsError`, …) in favour of the eleven-module concept-driven
> architecture documented in `summary.md` and `docs/architecture.md`.
> Below the archive banner are the entries from before that reshape —
> they describe APIs that no longer exist in the public surface.

## [Unreleased]

### Added

#### Architecture
- Eleven-module concept-driven architecture (`eph-utils`, `eph-containers`,
  `eph-core`, `eph-codec`, `eph-net`, `eph-net-kernel`, `eph-net-dpdk`,
  `eph-fix`, `eph-itch`, `eph-json`, `eph-book`); see `summary.md` and
  `docs/architecture.md`. Header-only, zero virtual dispatch in the hot
  path, GCC ≥ 13 / Clang ≥ 17 (uses `std::expected`, `std::format`).
- Three core concepts replacing the retired class hierarchy:
  `eph::core::StreamCodec<T>` / `DatagramCodec<T>`, `eph::net::Stream<T>`
  / `Datagram<T>`, `eph::net::Poller<T>`. Two networking backends share
  the same concepts: `eph::net::kernel::*` (epoll) and
  `eph::net::dpdk::*` (DPDK kernel-bypass).
- Unified `eph::core::Error` enum + `eph::core::ErrorInfo` (allocation-free
  `const char* detail`) returned via `std::expected<T, ErrorInfo>` from
  every fallible API. Replaces the per-module legacy enums. See
  `eph-core/include/eph/core/error.hpp`.

#### Net / DPDK
- `DpdkTcpStream::create_and_attach(cfg, platform)` turnkey factory:
  queue selection (Software / RSS-pinned / FlowDirector), src_port
  allocation with RSS hash rebinding, TCP / TLS / WS handshakes, Poller
  attach, FlowDirector rule install, ICMP registration.
- Path-MTU feedback path: `Platform::register_icmp_target` +
  `TcpSession::on_icmp_frag_needed(mtu)`; router-originated ICMP Type 3
  Code 4 routes to the owning stream regardless of which RX queue it
  lands on (RSS-safe, shared_ptr-managed registry).
- Caller-driven TCP keepalive: `TcpConfig::keepalive_interval` /
  `keepalive_probes` + `TcpSession::tick_keepalive(now_tsc)`. Production
  poller invokes via `on_poll_tick_` hook automatically.
- DPDK multi-process (primary + secondary): `eph::dpdk::ProcType` +
  `PlatformConfig::proc_type` / `file_prefix` / `rx_queue_range` +
  `Platform::create_primary` / `create_secondary`. See
  `eph-net-dpdk/docs/dpdk-multiprocess.md`.
- RSS bring-up hardening: `Platform::create` no longer silently falls back
  to queue 0 when RSS can't be enabled — it hard-fails with a recovery hint.
  **BREAKING CHANGE** vs pre-v3.3 silent collapse. (The later 2026-06-02
  reshape removed the trusted-key prediction surface entirely and made RSS
  queue landing fully empirical — see `eph-net-dpdk/CHANGELOG.md`.)

#### Net / Kernel
- `eph::net::HttpConnectConfig` + `StreamConfig::proxy` — HTTP CONNECT
  proxy, kernel backend only (DPDK rejects with `Error::InvalidConfig`).
- `eph::net::parse_http_request` / `parse_http_response` /
  `build_http_request` — incremental zero-heap HTTP/1.1 parser subset.
  Explicitly rejects chunked / `Transfer-Encoding` / cookies / redirect /
  `Expect: 100-continue` (unused by HFT venues, substantial attack surface).

#### Codec / WebSocket
- `StreamConfig::ws_path` / `ws_extra_headers` / `ws_timeout` — non-empty
  `ws_path` transparently performs RFC 6455 client handshake inside
  `TcpStream::create()` on both backends.
- `WsCodecConfig::permessage_deflate` (RFC 7692).

#### Observability
- `eph::core::MetricsSink` concept + `NullSink` / `eph::utils::ConsoleSink` —
  generic push sink. Any user type with `push_counter` / `push_gauge` /
  `push_histogram` / `flush` satisfies it (duck-typed).
- `eph::net::StreamMetric` enum + `eph::net::publish_metrics<Stream, Sink>` —
  two-layer observability for the four stream backends. Hot path:
  `alignas(64) std::atomic<uint64_t>` array, `lock add` on x86. Reader:
  `metric(StreamMetric m)` direct read, or `publish_metrics` to forward
  every counter into any `MetricsSink`.

#### Compliance / Utilities
- `eph::utils::KillSwitch` — single-fire, non-resettable compliance primitive.
- `eph::utils::TokenBucket` — thread-safe weighted rate limiter.
- `eph::net::HmacSha256` with typed `Key` (RAII-clearing) and `Tag` wrappers.

### Changed (BREAKING)

- **Logging is now SILENT by default and compile-time gated on `EPH_ENABLE_LOG`.**
  The whole library logs through `eph/core/log.hpp` (`EPH_LOG_*` macros +
  `eph::log::get`). With `EPH_ENABLE_LOG` undefined (the default) every log site
  compiles to a no-op — the library no longer writes to the host's stdout or its
  spdlog default logger, and pays zero hot-path cost. Previously release builds
  defaulted to `SPDLOG_LEVEL_INFO` and emitted to stdout/the default logger.
  Migration: build with `-DEPH_ENABLE_LOG` (or `xmake f --eph_log=y`) to restore
  output; tune with `-DSPDLOG_ACTIVE_LEVEL=…`. Logger names are unified under
  `eph.<module>.<component>` (the dpdk `dpdk.X` / `net.dpdk.X` double-prefix is
  fixed to `net.dpdk.X`). The per-module `net_log_level` / `SPDLOG_ACTIVE_LEVEL`
  build defines, `eph::core::detail::make_logger`, and the dpdk
  `get_logger<LoggerName>` NTTP helper are removed. See `docs/logging-guide.md`.

### Removed (BREAKING)

- `eph::net::Transport` / `SocketTransport` / `DirectTransport` /
  `DirectTxTransport` and the entire `eph-transport` module — replaced
  by the `Stream<Codec>` / `Datagram<Codec>` concept hierarchy across
  `eph-net-kernel` and `eph-net-dpdk`.
- `eph::net::Gateway`, `eph::net::CircuitBreaker` — out of scope, see
  `.artifacts/phase-9-scope-decision.md`.
- Per-module error enums (`SendError`, `ConnectionError`, `TcpError`,
  `WsError`, …) — replaced by unified `eph::core::Error`. Parser modules
  retain domain-specific enums (`FrameError`, `FixError`, …) where the
  granularity is genuinely needed.
- DPDK silent fallback to queue 0 on RSS bring-up failure —
  `Platform::create` now hard-fails with a recovery hint.
- `DpdkTcpStream::create(cfg, poller)` overload — narrow subset covered by
  `create_and_attach(cfg, platform)`.

### Notes

- For **per-module** detailed history (every backfill, fix, refactor),
  consult `eph-<name>/CHANGELOG.md`. Those are the source of truth and
  are continuously updated during development; this top-level changelog
  summarises only.
- For deferred-out-of-scope items see
  `.artifacts/phase-9-scope-decision.md` (Gateway, CircuitBreaker,
  chunked HTTP, SOCKS5 proxy) and the deferred-observability list in
  CLAUDE.md (histogram integration, gauge metrics, tracing, OpenTelemetry).

---

## [v2.x pre-v3.3 archive]

The entries below describe APIs that were retired during the post-v3.3
reshape. Preserved verbatim for historical record.

### Added

#### Transport (`transport`)
- Three-class transport hierarchy: `Transport` (full-featured), `DirectTxTransport` (TX-only direct), `DirectTransport` (full direct-mode)
- Five independent composable components: `TransportCore`, `ReconnectPolicy`, `FrameProcessor`, `TxWorker`, `RxWorker`
- `feed_rx()`/`process_pending()` API for reactor-driven direct-mode receive
- Reactor `on_burst_complete` callback for batched RX processing

#### DPDK (`dpdk`)
- Reactor feed integration for direct-mode transport

### Changed

#### DPDK (`dpdk`)
- Extract logger factory (`LoggerName`, `get_logger`) from `net_header.hpp` into `detail/logger.hpp` to reduce header coupling
- Fix unqualified `get_logger`/`LoggerName` calls in `dns.hpp` and `arp.hpp` (wrong namespace scope)

#### Benchmarks
- Mock WebSocket server for deterministic latency measurement (no live exchange dependency)
- Socket and DPDK benchmark variants using `DirectTransport` for fair comparison
- DPDK benchmark setup script with network namespace isolation
- Standalone mock server mode (`--no-mock`) for namespace-based benchmarking
- Built-in mock with `SO_BINDTODEVICE` for single-process benchmarking

### Changed

#### Transport (`transport`)
- Extracted `eph-transport` module from `eph-net` as a standalone package
- Eliminated `TransportMode` enum in favor of distinct transport classes
- Replaced monolithic transport with composition of independent worker components
- Unified benchmark design: all variants use `DirectTransport` + external mock server

#### Benchmarks
- Renamed `bench_market_single` to `bench_market`
- Moved latency benchmarks into `benchmarks/latency/` directory
- Renamed `bench_setup.sh` to `bench_latency.sh`
- Removed 9 legacy Binance-dependent benchmarks in favor of mock-based approach
- Mandatory CPU pinning with configurable defaults for reproducible results

### Fixed

#### Network Transport (`net`)
- Corrected RFC 6455 WebSocket Accept GUID in `validate_ws_accept` and `mock_ws_handshake`
- Fixed `on_message` callback delivery not triggering in push mode
- Resolved GCC 14 linker errors in transport module
- Made `SocketTransport::state_` atomic to prevent data race on concurrent access
- Deferred `close_fd()` in `SocketTransport::close()` for graceful shutdown
- DNS answer count cap to prevent buffer overread
- TLS hostname verification enforcement
- Proxy response buffer bounds checking
- Reactor data race in RX dispatch loop (local session pointer fix)
- HMAC key truncation when key exceeds block size
- CloseWait data delivery and DNS truncation guard in DPDK stack
- SOCKS5 buffer overflow, poll error handling, and TLS session hardening
- Flushed pending ACK using local session pointer in Reactor RX loop

#### Transport (`transport`)
- Guarded direct-mode members and APIs with `constexpr`/`requires` to prevent misuse
- `ReconnectPolicy` unit tests and correctness fixes

#### Benchmarks
- Validated all required benchmark arguments with fail-fast on missing values
- Sequential mock server startup to prevent port conflicts
- Used sysfs for PCI device detection in `bench_latency.sh` (portable across distros)
- Fixed mock server kill errors on cleanup

### Added

#### Network Transport (`net`)
- Gateway for multi-transport lifecycle management
- KillSwitch for coordinated emergency shutdown
- Circuit breaker for exchange connection resilience
- SOCKS5 and HTTP CONNECT proxy support
- mTLS client certificate support
- Plain WebSocket (`ws://`) support via `use_tls` config flag
- Dual-stack IPv4/IPv6 support in SocketTransport
- URL-based `connect()` convenience factory and `TransportConfig::from_url()`/`to_url()`
- Exponential backoff with jitter for reconnection
- RFC 6455 compliant graceful close handshake
- WebSocket fragmentation reassembly across TLS records
- Per-connection RTT histogram from ping/pong measurements
- Per-phase connection latency breakdown and handshake duration metric
- TX/RX latency breakdown with per-opcode text/binary counters
- Push-mode `on_message` callback and configurable send timeout
- `on_reconnected` callback for subscription replay
- `on_reconnect_attempt` callback for non-transient error abort
- `on_ping`/`on_pong` and connection state change callbacks
- `on_rx_drop` callback for pre-allocated WS fragment buffer
- `SendError` enum, `send_for()` timed send, `send_close()`, `send_binary()`, `send_ping()`
- `wait_recv()` with timeout, `try_recv()`, batch `recv`, `poll_rx_for()`
- `is_connected()`, queue occupancy API for backpressure monitoring
- TCP keepalive support
- Token bucket rate limiter for exchange API compliance
- HMAC-SHA256 signing and encoding utilities
- Minimal HTTP/1.1 client for REST API calls
- Structured logging for Transport state transitions
- `TransportStats::dump()`, `to_json()`, and `std::formatter` specializations
- `ConnectionInfo` struct and `Transport::connection_info()`
- TLS connection metadata exposure and sequence exhaustion recovery
- Symbol-aware dedup for multi-symbol WebSocket streams
- Generic `on_frame_filter` callback replacing symbol dedup
- Deferred thread start for per-symbol multi-connection support
- WsFramer and pluggable `MessageFramer` concept as Transport template parameter
- DNS resolution timeout for `SocketTransport::connect()`
- UTF-8 validation for WebSocket text frames (RFC 6455)
- Close frame delivery to RX queue with `close_code()`/`close_reason()` accessors
- Payload length bounds checking in WebSocket frame encoder
- RFC 6455 compliance validation in `ws::decode_frame`
- Config validation (`SocketConfig::validate()`, `TransportConfig` interval checks)

#### DPDK (`dpdk`)
- User-space DNS resolution over DPDK data plane
- RSS and `rte_flow` steering for hardware RX dispatch
- Reactor -- epoll-style zero-ring multiplexed RX
- SharedRxDispatcher for per-symbol multi-connection support
- Gateway MAC option to skip ARP
- `TcpConfig::validate()` and `std::formatter` support
- TCP reorder buffer for out-of-order segments
- SYN options and retransmit support
- Safety guards to prevent binding sole/SSH NIC
- Observability operators across all Stats and Config types

#### FIX Protocol (`fix`)
- FIX 4.4 session layer (Logon, Logout, Heartbeat, SeqNum management)
- Pre-trade risk checker with configurable limits
- Position tracker for pre-trade risk management
- Order lifecycle manager (OMS core)
- Typed `ExecutionReport` view and `try_parse_execution_report`
- Typed order entry helpers for HFT
- `set_decimal()` and `set_price()` for exact decimal encoding
- `set_timestamp()` / `get_timestamp()` for UTCTimestamp fields
- `set_raw()` for binary field support, `set_unique()` family for duplicate tag prevention
- `get_bool()` / `set_bool()` for FIX Y/N boolean fields
- `has_tag()`, `has_overflow()`, `begin()`/`end()` iterators for range-for
- Type-safe `dispatch()` visitor for zero-overhead MsgType routing
- `dump()` and `to_json()` on MessageView for diagnostics
- `parse_all()` batch parser for FIX message streams
- `msg_type_name()` for human-readable MsgType names
- Repeating group support with typed `GroupEntry` accessors
- Sub-second precision, multi-char MsgType dispatch, and builder ergonomics
- `ParserStats` for production observability
- BodyLength validation and parse error offsets
- `MessageBuilder` constructor precondition validation
- `std::formatter` for `ParseError`
- `std::span` overloads for `parse()` and `parse_all()`

#### ITCH Protocol (`itch`)
- Nasdaq ITCH 5.0 parser with field accessors for all 22 message types
- Nasdaq OUCH 5.0 order entry protocol
- SoupBinTCP framer for Nasdaq ITCH TCP transport
- MoldUDP64 multicast transport parser
- Type-safe `dispatch()` visitor and `dispatch_all()` combining parse + dispatch
- `trim()` utility for right-padded string fields
- Message classification helpers
- `ParserStats` for production observability
- `std::span` overloads for `parse()` and `parse_all()`
- `std::formatter` for `ParseError`

#### JSON Parser (`json`)
- Zero-copy JSON parser and `JsonFramer` for crypto HFT
- Binance bookTicker adapter and `symbol_hash` extractor
- Binance WebSocket subscription helpers
- Binance REST client for depth snapshots and server time
- OKX exchange adapter for bbo-tbt and subscriptions
- Bybit exchange adapter for tickers and subscriptions

#### Order Book (`book`)
- L2 `ArrayBook` with zero-allocation design
- `MapBook` for deep L3 order books (1000+ levels)
- Binance bookTicker to ArrayBook adapter
- ITCH to ArrayBook adapter for L2 order book building
- Depth snapshot loading for reconnection recovery
- Market microstructure signal calculators
- Utility methods, NaN guard, and expanded tests

#### Containers (`containers`)
- `BoundedQueue` and `BoundedQueueBytes` with timed operations and batch push/pop
- `EvictingQueue` and `EvictingQueueBytes` with timed operations and batch push/pop
- `RingBuffer` for tick history and lookback signals
- `try_peek()`, `try_peek_latest()` for non-consuming reads
- `clear()`, `size_approx()`, `empty()`, `full()`, `overwrite_count_approx()`
- `try_consume_all()` drain helpers
- `try_produce_n` for zero-alloc batch enqueue
- Stats structs with `dump()`, `to_json()`, `std::formatter`, and diff operators

#### Utilities (`utils`)
- `HdrHistogram` with percentile iteration, inverse CDF, windowed measurement, linear buckets
- `ConcurrentRecorder` with bulk API, export methods, and `reset()`
- EMA and crossover detector for signal smoothing
- Timestamp conversion utilities for HFT
- `SystemResourceStats` with memory and thread metrics, `to_json()`, `std::formatter`
- `set_thread_affinity()` returning `std::expected` for error detection
- `set_thread_realtime()` for SCHED_FIFO/SCHED_RR
- Compile-time version header (`eph/version.hpp`) with `version_at_least()`
- AuditLog for regulatory compliance audit trail

#### Core (`core`)
- `eph-core` module extracted with unified error patterns
- `MetricsSink` concept with `NullSink` and `ConsoleSink`
- Instance-method framer decode

#### Benchmarks
- Market data + ping/pong DPDK benchmark
- RX pipeline micro-benchmark (decrypt, decode, filter)
- Mock Binance WebSocket server for latency benchmarking with flash-crash simulation
- ArrayBook benchmark (3ns update, 163ns full cycle)
- JSON parse and Binance adapter benchmarks
- FIX and LengthPrefix framer pipeline benchmarks
- Per-second windowed latency tracking
- Unified bench runner with baseline data collection
- Nanosecond-precision feed latency measurement

#### Examples
- Binance bookTicker to orderbook end-to-end example
- Socket-based WSS client example
- 6 examples covering full learning path
- `--main-cpu` flag for main thread affinity
- `--on-message` mode for multi-symbol benchmarks

#### CI/CD
- GitHub Actions pipeline with ARM64 runners and performance gates

### Changed

#### Network Transport (`net`)
- Replaced fixed-interval reconnection with exponential backoff + jitter
- Replaced `Probe` architecture with per-message timestamp system
- Replaced `string` errors in `ws::decode_frame` with typed `DecodeError` enum
- Replaced symbol dedup with generic `on_frame_filter` callback
- Decoupled `LastOnlyDeliver` from `EvictingQueue`
- Cached `write_seq()` locals and documented masking
- Default `skip_utf8_validation` set to `true` for lower hot-path latency
- Marked `RttStats` and `TransportStats` helpers `constexpr`

#### DPDK (`dpdk`)
- Split `ConnectorConfig` into `DpdkEndpoint` + `ConnectorOptions` (breaking) -- see below
- Removed deprecated `SharedRxDispatcher`
- Deduplicated `format_mac` into `net_header.hpp`
- Switched DPDK to vcpkg package source

#### FIX/ITCH
- Unified `error_name` return types to `string_view`
- Auto-derived ITCH size constants
- Split `set()` into validated and trusted paths for performance
- Replaced magic numbers with tag constants in benchmarks

#### General
- Modernized with concepts, ranges, and improved observability throughout
- Replaced `_mm_pause` with portable `cpu_relax`
- Replaced regex with `from_chars` for parsing
- Unified namespaces and fixed hugepage allocator size tracking
- Added `[[nodiscard]]` sweep across all modules
- Enabled `-Wall -Wextra` and resolved all warnings
- Used `EPH_ENABLE_TIMESTAMPS` macro instead of template params
- Reorganized tests and benchmarks into module subdirectories

### Fixed

#### Network Transport (`net`)
- Resolved deep concurrency and protocol correctness issues
- Fixed WebSocket Close handshake race condition
- Flushed pending TCP ACK after handshake in `deferred_start` mode
- Drained TX queue on `stop()` to prevent message loss
- Added DNS resolution timeout to prevent hanging connects
- Added total deadline to `SocketTransport::send()` to prevent unbounded retry
- Capped WebSocket upgrade response buffer at 64KB
- Guarded TLS decrypt against `payload_len` underflow
- Corrected TX stats on coalesced TLS send failure
- Guarded TLS shutdown in destructor against broken connections
- Used protocol-aware default port for WS upgrade Host header
- Rejected CR/LF in `ws_subprotocol` to prevent HTTP header injection
- Sanitized host field in `SocketConfig::to_json()` to prevent JSON injection
- Added WS-level reassembly buffer for frames spanning TLS records
- Validated null data pointer in Transport send path
- Removed `const_cast` in `RateLimiter::available()`
- Added defensive guards for payload overflow and buffer underflow
- Corrected AEAD decrypt `max_out_len` to exclude auth tag
- Logged `close()`, `shutdown()`, and `setsockopt` failures in teardown paths
- Checked `setsockopt`/`getsockopt` return values in SocketTransport

#### DPDK (`dpdk`)
- Addressed AWS deployment issues from real hardware testing
- Fixed static DPDK build on ARM64 (force-include `rte_config.h`, whole-archive PMDs)
- Hardened packet parsing, fixed mbuf leak, atomicized TSC
- Used random ephemeral port instead of hardcoded 32768
- Implemented `ConnectorOptions::operator==` for `rte_ether_addr`
- Included DNS config in `ConnectorOptions` serialization
- Deferred ACK to after TLS decrypt for lower RX latency
- Hardened DNS resolver against malicious packets
- Added ARM fallback for CPU topology parsing
- Checked `RAND_bytes` return value in ISN generation

#### FIX/ITCH
- Prevented integer overflow in BodyLength parsing loop
- Added integer overflow detection to `MessageView::get_int()`
- Validated dates and rejected pre-epoch timestamps in `get_timestamp()`
- Fixed `NaN`/`Infinity` guard for `set_double()`
- Handled rounding carry in `MessageBuilder::format_double()`
- Prevented UB in `set_raw()` with null data
- Escaped JSON special chars in `to_json()`
- Handled `count=0` in `get_group()`
- Added SOH validation to FIX `set()`
- Corrected ITCH `dispatch()` to pass full message pointer to handlers
- Fixed NOII message size and hardened framers

#### Book
- Added epsilon-tolerant price matching to `MapBook`

#### Containers
- Corrected `EvictingQueueBytesStats::throughput()` on delta snapshots
- Relaxed `BoundedQueue` `static_assert` to allow `Capacity=1`
- Used actual read count in `EvictingQueue<T,1>::overwrite_count_approx()`
- Added missing `#include <memory>` for `std::construct_at`

#### Utilities
- Preserved retired thread skipped counts in `ConcurrentRecorder`
- Hardened `for_each_linear()` overflow and `record_corrected()` efficiency
- Added thread-safety warning to `compute_and_reset`

#### Build
- Resolved OpenSSL/aws-lc header conflict for `eph-fix` + `eph-dpdk`
- Moved PMD whole-archive to binary targets
- Prevented crash on duplicate logger name registration

#### General
- Resolved 40+ audit findings across all modules
- Hardened new modules with bounds checks, null guards, and `[[nodiscard]]`
- Hardened KillSwitch and ConsoleSink thread safety
- Added logging for silent TLS decrypt failures

### Breaking Changes

- `ConnectorConfig` split into `DpdkEndpoint` + `ConnectorOptions` -- existing DPDK connection setup code must be updated

### Documentation
- Project README with quickstart, module overview, and integration guide
- Binance protocol guide (JSON/SBE/FIX with ephemeral mapping)
- DPDK setup guide (hugepages, NIC bind, EAL, NUMA)
- Custom framer guide with `MessageFramer` concept tutorial
- WebSocket-via-proxy example (SOCKS5 and HTTP CONNECT)
- Multi-connection coordination guide (4 patterns)
- Production configuration guide with three deployment profiles
- Operations runbook (metrics, alerts, incident playbooks)
- Troubleshooting guide with error matrix and diagnosis steps
- FIX 4.4 trading demo with build, parse, and price encoding
- ITCH 5.0 feed demo with synthetic order book sequence
- Benchmark metrics documentation (TX/RX/RTT measurement points)
- Binance live traffic monitoring reports and surge profile benchmark
- TODO.md with prioritized roadmap
