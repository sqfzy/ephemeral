# Changelog

All notable changes to `eph-transport` are listed here. The format is
based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Dates follow ISO 8601 (YYYY-MM-DD).

## [Unreleased]

### Added

- `DirectTxTransport`: a new hybrid variant that performs TX
  synchronously on the application thread (no TX queue, no TX thread)
  while keeping a background RX thread and SPSC RX queue. Eliminates
  TX queue latency without giving up asynchronous receive.
- `DirectTransport`: a fully threadless, queueless variant for
  single-threaded event loops (Reactor, io_uring, DPDK poll-mode).
  Exposes `poll()` for all-in-one use and
  `feed_rx()` + `process_pending()` for split Reactor integration.
- `make_twophase_filter()`: a batch `FrameFilterFn` that keeps only the
  latest frame per symbol hash in a combined multi-symbol stream. Two
  overloads — one accepting a `std::function` extractor, one accepting
  a raw function pointer to avoid the `std::function` overhead on the
  hot path.
- `ReconnectPolicy`: an independent, testable exponential-backoff
  state machine with ±25% jitter, resolved `max_backoff` (16x base if
  unset), and a templated `attempt()` taking any callable returning
  `std::expected<void, ConnectionErrorInfo>`. Callers can abort
  further attempts via `on_reconnect_attempt` returning `false`.
- Certificate pinning: `TransportConfig::pinned_spki_sha256` holds
  base64-encoded SPKI SHA-256 hashes, checked after the TLS handshake.
  `on_pin_mismatch` callback can accept (soft-pin) or reject
  (hard-pin) the mismatch.
- Pong timeout detection: `TransportConfig::pong_timeout` triggers a
  reconnect when no pong arrives within the configured window after a
  ping, catching dead-but-established connections that TCP keepalive
  misses.
- `TransportConfig::thresholds`: periodic stats sampling with
  `on_breach` callback for rx-drop and RTT-p99 alerting from the RX
  loop, at configurable `check_interval` iterations.
- `TransportConfig::from_url()` and `to_url()`: parse and serialise
  `ws://` / `wss://` URLs, including IPv6 bracket notation, optional
  port, and path+query. Rejects control characters to prevent HTTP
  header injection (CWE-93/CWE-113).
- `TransportConfig::validate()` / `warnings()`: early-exit validation
  with actionable error strings, plus advisory warnings for likely
  misconfigurations (mismatched TLS/verify, oversized bursts,
  skip-utf8 without explicit opt-in, soft vs hard pinning).
- `TransportStats::operator-`: produce windowed metrics by subtracting
  snapshots, with counter fields differenced and percentile/mean
  values taken from the later snapshot.
- `TransportStats::to_json()`, `dump()`, plus rate helpers
  (`tx_pps`, `rx_pps`, `tx_mbps`, `rx_mbps`) for monitoring
  integration.
- `RttStats::to_json()`, `dump()`, and microsecond convenience
  accessors (`p50_us`, `p99_us`, `p999_us`, …).
- `TransportConfig::operator==`: value equality for non-callback
  fields, supporting config dedup and round-trip (`from_url ->
  to_url -> from_url`) testing.
- `ConnectionInfo`: aggregated snapshot (TLS version, cipher,
  subprotocol, remote IP, port) with `dump()` / `to_json()` and
  defaulted `operator==`.
- `TlsConfig::warnings()`: advisory diagnostics for TLS settings.
- Preset aliases: `DefaultTransport`, `SmallTransport`,
  `LargeTransport`, `EvictTransport`, `RawTransport`, plus matching
  `DirectTx*` and `Direct*` variants.
- Benchmark harness: `benchmarks/bench_transport_types.cpp` covering
  WS encode (64B / 512B), `apply_mask`, AES-GCM encrypt / decrypt
  (64B / 512B), HKDF, TLS record header parse, UTF-8 validation,
  `opcode_name`, `TransportStats::dump` / `to_json`,
  `RttStats::dump` / `to_json`, `ConnectionInfo::to_json`,
  `TransportConfig::warnings()` / `to_url` / `dump`, and
  `ConfigToUrl` / `ConfigDump` / `StatsDump` round-trip costs.
- Fuzzer harness: `fuzzers/fuzz_ws_decode.cpp` for `ws::decode_frame`.
- Comprehensive tests for `TlsConfig`, `tls_keygen`, `opcode_name`,
  `TlsHotState`, `ReconnectPolicy`, `FrameView`, `ThreadStats`
  equality and formatting, stats delta operators, WS header encoding,
  RSV bits, masking, close-code validation, RFC 6455 edge cases, and
  TLS crypto boundary / error paths.
- RX stats: `tcp_rx_packets` / `tcp_rx_bursts` from the TCP backend
  are now propagated into `TransportStats` and its JSON serialisation.
- HWM tracking for TX batch enqueue (`tx_queue_hwm` reflects peak
  occupancy including batch arrivals).

### Changed

- `Transport` has been split into three independent class templates
  (`Transport`, `DirectTxTransport`, `DirectTransport`), eliminating
  the old `TransportMode` enum and its `constexpr`/`requires`
  guarded members. Each variant composes a common
  `TransportCore` + `TxWorker` (threaded only) + `RxWorker` (threaded
  or DirectTx) + `ReconnectPolicy`. Selection is a type decision at
  instantiation, not a runtime flag.
- `FrameProcessor`, `TxWorker`, and `RxWorker` were extracted from
  `Transport` into independent components under
  `include/eph/transport/detail/`. Each owns a single responsibility
  (decode pipeline, TX thread and queue, RX thread and queue) and can
  be composed à la carte.
- `DirectTransport::poll()` now takes a zero-copy fast path for WS
  decoding: payloads that do not cross a TLS record boundary are
  delivered directly from the decrypt buffer without an intermediate
  WS reassembly copy.
- `ReconnectPolicy::attempt()` is now a template over the connection
  functor, taking anything returning
  `std::expected<void, ConnectionErrorInfo>`. This removed a
  `std::function` allocation from the reconnect hot path.
- `ReconnectPolicy` now binds to `core_.config` (the owned copy in
  `TransportCore`) rather than the caller's reference, preventing
  dangling references on reconnect attempts that outlive the original
  stack frame.
- `TxMessage` / `RxMessage` switched to `std::array<uint8_t, MaxPayload>`
  for the data buffer, aligning with `TrivialData` constraints on
  `BoundedQueue` / `EvictingQueue`.
- `TransportStats::operator-` now carries the latency histograms
  through the delta (previously zeroed, losing percentile info).
- `TransportStats::to_json()` uses in-place append for the buffer
  build, reducing allocations on the monitoring path.
- All logger functions now return raw `spdlog::logger*` (not
  `shared_ptr`) to avoid atomic ref-count traffic at log sites.
  `SPDLOG_LOGGER_*` macros remain the sole emission path for
  compile-time level filtering.
- WebSocket utility functions, TLS encrypt / decrypt, HTTP helpers,
  `FrameProcessor::process()`, `TransportCore::send_*_direct()`, and
  stats helpers are now marked `[[nodiscard]]` — ignored return values
  now produce compiler warnings.
- `detail/frame_filter.hpp` and `detail/logger.hpp` extracted from
  `transport_types.hpp` to reduce header coupling and build times.
- `detail/tls_constants.hpp` and `detail/tls_session.hpp` dependency
  direction corrected: constants no longer depend on session.
- `tls_const` constant duplication cleaned up: each record/AEAD
  constant has exactly one definition.
- `DirectTxTransport`: removed the deprecated
  `send_for` / `send_n_for` wrappers — callers should use `send` /
  `send_n` directly (these variants were synchronous anyway and
  offered no backpressure semantics).
- Modular `xmake.lua`: tests and benchmarks now follow the module
  pattern — one target per file — with `eph-test` / `eph-bench` rules,
  simplifying discovery and per-target iteration.
- Documentation: Doxygen-style `///` comments added to all public
  headers; README and `summary.md` expanded with the full API
  reference.

### Fixed

- Concurrency: `stop()` now waits for any in-progress reconnect
  before joining the TX/RX threads. Previously, `do_reconnect_()`
  could still be touching `crypto_` / `tcp_` while the thread-join
  path attempted to send the Close frame, producing a use-after-free
  race.
- `close_gracefully()` / `stop()`: the close code and reason are now
  written before the `close_requested` release-store and read after
  an acquire-load, so `stop()` sees the app-thread-provided values
  consistently (memory-order M9 fix).
- Null-pointer UB fixed in `ws::build_close_frame` /
  `ws::build_ping_frame` / `ws::build_pong_frame` for the
  empty-reason / empty-payload paths.
- `ReconnectPolicy` backoff no longer collapses to zero on repeated
  sub-millisecond intervals.
- `kEnableTimestamps` ODR violation fixed: now defined in exactly one
  place (`transport_types.hpp`).
- `DecodedFrame::close_status_code()` now correctly handles masked
  close frames.
- HTTP upgrade handshake: 101 status line, `Upgrade:` header, and
  `Connection:` header are now strictly validated (previously the
  server could silently deliver a non-101 response).
- HTTP upgrade response buffer is bounded to prevent resource
  exhaustion from unterminated server responses.
- HTTP header injection (CWE-93/CWE-113): `remote_host`, `ws_path`,
  and `ws_subprotocol` now reject control characters, including
  bare-LF (CR-less LF, which some HTTP parsers accept as a line
  terminator). Signed-char hostname check fixed.
- TLS version: handshake fallback to TLS ≤1.2 now emits a
  `SPDLOG_LOGGER_WARN` (previously silent).
- `EVP_AEAD_CTX` safety: double-free and use-after-move scenarios
  closed; hot state is scrubbed on destruction.
- RNG seeding strengthened for WebSocket mask keys and TLS client
  randoms.
- `bio_write` integer overflow closed on huge records.
- On-message delivery under `on_message` callback: previously some
  edge cases (fragmented delivery, filtered frames) would skip the
  callback while still counting stats.
- Packet stats and RTT tracking are now updated correctly on the
  filtered-frame dispatch path.
- Dangling `ReconnectPolicy` config reference fixed (now bound to
  `core_.config`, the transport-owned copy).
- `TransportStats` serialisation: previously missing
  `tcp_rx_packets` / `tcp_rx_bursts` are now included in both
  `dump()` and `to_json()`.
- `TransportStats::operator-` previously dropped latency histograms;
  they are now propagated to the resulting delta.
- `on_reconnected` and other callback exceptions are now caught and
  logged with full exception detail (previously silently swallowed).
- RX error paths now emit leveled logs (previously some failure
  branches were silent).
- TX crypto error paths now log the failure instead of returning a
  bare error code.
- Missing `[[nodiscard]]` annotations added to send APIs and batch
  `memcpy` bug fixed in batch enqueue.
- `static_assert` constraints on `RxMessage` brought to parity with
  `TxMessage`.
- Windows hugepage header `#include` path fix.
- GCC 14 linker path fix for `on_message` delivery.

### Removed

- The old single-class `Transport` with a runtime `TransportMode`
  enum and `constexpr`/`requires` guards — replaced by three
  independent class templates (see *Changed*). This is a breaking
  change: callers must pick `Transport`, `DirectTxTransport`, or
  `DirectTransport` explicitly.
- The `eph-net` module's transport layer — `eph-transport` is now an
  independent subproject extracted from `eph-net` (also a breaking
  change for consumers).
- Deprecated `DirectTxTransport::send_for` and `send_n_for` (they
  were synchronous no-op wrappers with no real backpressure
  semantics — use `send` / `send_n`).
- Superseded `detail/` files from the pre-split architecture.
- Dead includes from direct transport headers.
- Unused `<optional>` include from `transport_types.hpp` and
  `<fcntl.h>` from ancillary headers.
- Duplicate constants from the `tls_const` namespace (kept in one
  place).

## Notes

- eph-transport was split out of `eph-net` as an independent
  subproject on 2026-04-02 (breaking change for downstream consumers).
- The documentation additions on 2026-04-02 introduced Doxygen-style
  `///` comments across all public headers; the 2026-04-09 `/doc all`
  pass filled in the few remaining gaps (simple accessors on
  `DirectTransport`, `is_running`, RTT/latency stat helpers).
