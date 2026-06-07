# eph-core changelog

## [Unreleased] — post-v3.3 consolidation

### Added
- `Error::NotFound` (`eph/core/error.hpp`) — registry / lookup lifecycle
  signal, distinct from `InvalidConfig`. Used by the DPDK ICMP target
  registry and other `shared_ptr`-managed lookup tables where
  unregistering an absent key is a recoverable state mismatch rather than
  a caller programming error. Appended at the end of the enum to keep
  existing integer values stable.
- `eph/core/packet_view.hpp` — formal `concept PacketView` formalises the
  previously-informal five-member contract (`writable_data` / `data` /
  `length` / `trim_front` / `trim_back`). Downstream
  backends can now `static_assert(eph::core::PacketView<T>)` to get a
  compile-time conformance guarantee.
- `eph/core/metrics_concept.hpp` — `MetricTag` struct, `MetricsSink`
  concept (`push_counter` / `push_gauge` / `push_histogram` / `flush`,
  all noexcept), and `NullSink` (inline no-op sink used as the default
  template parameter where observability is opt-in).
- `eph/core/error_traits.hpp` — `ErrorEnum<E>` concept and
  `ErrorEnumFormatter<E>` one-liner `std::formatter` base. Replaces the
  per-enum boilerplate previously duplicated in every parser module.
  Also exposes `eph::net::ErrorEnum` / `eph::net::ErrorEnumFormatter`
  backward-compat aliases for pre-refactor call sites.
- `eph/core/detail/logger.hpp` — shared `make_logger(name)` factory.
  Single source of lazy, thread-safe spdlog instantiation used by
  `length_prefix_framer` and by every downstream `eph-*` module.
- `eph/core/tcp_state.hpp` consolidation — `TcpState` + `tcp_state_name`
  now have a single canonical definition here; `tcp_concept.hpp` and
  `eph/net/tcp_state.hpp` both forward to this header to avoid ODR
  conflicts when multiple modules include both paths.
- `eph/version.hpp` gained `kVersion` (packed integer
  `major*10000 + minor*100 + patch`) for cheap compile-time comparison;
  `version_at_least()` is now `consteval`.

### Changed
- `OutputBuffer::append` now takes `std::span<const uint8_t>` instead of
  a raw `(uint8_t*, size_t)` pair — `std::span` is the project-wide
  zero-copy convention and matches the `PacketView` contract.
- `ErrorInfo` rendering is now explicitly three-path: `std::formatter<>`
  for `std::format`, `format_as()` ADL hook for spdlog's bundled fmt,
  and `operator<<` for `std::ostream` / gtest. All three stay in
  lockstep ("CODE: detail", detail omitted when empty).

### Removed (BREAKING)
- `Error::CodecNeedMoreData` and `Error::NoData` (`eph/core/error.hpp`).
  Both had zero producers in production code and were internal signals
  only. The `error_name()` switch, `test_error.cpp` `EXPECT_NAMED`
  assertions, and the `docs/troubleshooting.md` sections that
  documented them are all removed in lockstep. Downstream code with
  exhaustive `switch` on `Error` must remove the corresponding cases;
  no production site ever returned these values, so runtime behavior
  is preserved everywhere except in source-level pattern matching.
- `arrival_tsc()` removed from the `PacketView` contract (`eph/core/
  packet_view.hpp`) and from all five implementations (`SpanView`,
  `MbufView`, `SpanPacketView`, `FakeStream`/`FakeDatagram` PacketView).
  It was dead scaffolding: zero production consumers (only test
  self-assertions), 3/4 backends hardcoded 0, and it never reached a
  user frame handler. The load-bearing keepalive timestamp line
  (`last_rx_burst_tsc_` / DPDK TCP `process_burst_` `rx_tsc` / poller
  `cycle_tsc`) is a *separate* path and is untouched. Any custom
  `PacketView` that exposed `arrival_tsc()` still compiles (the concept
  only shrank); no in-tree caller read it. If per-frame latency
  measurement is needed later it will be rebuilt as a frame-carried
  `RxMeta` delivered to the message handler, **not** as a method on
  `PacketView` (the drain loop already holds the recv/burst TSC, so the
  value never has to round-trip through the view).

## Phase 9 Recovery (2026-04-10)

### Added
- Three new values in `enum class Error` (`eph/core/error.hpp`) used by
  the `eph-net` HTTP CONNECT proxy path:
  - `ProxyConnectFailed` — TCP connect to proxy host failed.
  - `ProxyHandshakeFailed` — proxy returned non-2xx on CONNECT, or the
    response body was malformed.
  - `ProxyAuthRequired` — proxy returned `407 Proxy Authentication
    Required` and the supplied credentials were rejected (or absent).

## v3.3 (2026-04-10) — architecture refactor

The v3.3 architecture refactor (see
`.artifacts/design-eph-v3.3-architecture-20260410.md`) reshaped `eph-core` as the
leaf dependency for networking concepts.

### Added
- `eph/core/error.hpp` — unified `enum class Error` + `struct ErrorInfo` replacing
  the scattered pre-v3.3 error enums (`SendError`, `ConnectionError`, etc.).
- `eph/core/codec.hpp` — `StreamCodec` / `DatagramCodec` / `Codec` concepts, plus
  the `OutputBuffer` class used by codecs for auto-responses (WS pong, close ack).
- `eph/core/packet_view.hpp` — the `PacketView` zero-copy contract that both
  kernel (`SpanView`) and DPDK (`MbufView`) backends conform to.

### Removed
- `eph/core/fake_tcp_transport.hpp` — replaced by
  `eph::net::test::FakeStream` in `eph-net`.
- `eph/core/transport_errors.hpp` — `SendError` / `ConnectionError` /
  `ConnectionErrorInfo` consolidated into `eph/core/error.hpp`.

### Retained (legacy, still used by parser modules)
- `framer_concept.hpp`, `length_prefix_framer.hpp` — still consumed by
  `eph-fix`, `eph-itch`, `eph-json`. Their wire format is unchanged by v3.3.
- `tcp_concept.hpp` — still referenced by the internal-detail TCP session layer
  inside `eph-net-dpdk`. Users never see it directly.
- `parse_number.hpp`, `detail/base64.hpp`, `detail/json_escape.hpp`,
  `detail/string_checks.hpp` — unchanged utility helpers.
