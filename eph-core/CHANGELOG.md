# eph-core changelog

## [Unreleased] — Phase 9 Recovery (2026-04-10)

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
