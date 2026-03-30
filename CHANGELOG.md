# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

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
