# eph-codec changelog

## [Unreleased]

### Docs
- Corrected `LengthPrefixCodec` description across README / CHANGELOG / summary
  from "2-byte BE length prefix, ≤65 535 byte payloads" to the actual
  implementation: **4-byte big-endian length prefix with `kMaxFrameLen =
  16 MiB`** (see `length_prefix_codec.hpp`). The wire format never changed —
  the docs had drifted from the source on day one.
- `summary.md`: updated `RawDatagramCodec::decode` sink parameter to
  `const std::function<void(Frame)>&` (by-const-ref, matching the source),
  added the default-constructed `Mold64Codec` / `WsCodec` forms, and
  documented `WsCodecConfig` / `Mold64Config` fields.
- `README.md` / `docs/ONBOARDING.md`: added `test_packet_view`,
  `test_raw_datagram_codec`, and `test_ws_codec_edge` to the runnable-test
  list (they were already auto-globbed by xmake; only the docs were missing).
- `docs/ONBOARDING.md`: "adding a benchmark" note now states that the
  `benchmarks/` directory is empty by default and must be created before
  adding a `bench_*.cpp` (xmake auto-globs it once present).

## v3.3 (2026-04-10) — module introduced

`eph-codec` was created during v3.3 Phase 1 to hold stateful codec implementations
separate from networking backends. See
`.artifacts/design-eph-v3.3-architecture-20260410.md` for the full design.

### Added
- `eph/codec/ws_codec.hpp` — RFC 6455 WebSocket codec. Owns reassembly buffers and
  the ping/pong/close FSM. Auto-responds to control frames via `OutputBuffer`.
- `eph/codec/raw_stream_codec.hpp` — passthrough `StreamCodec`.
- `eph/codec/length_prefix_codec.hpp` — 4-byte BE length prefix `StreamCodec`
  with `kMaxFrameLen = 16 MiB` for slow-loris DoS resistance. Distinct from the
  legacy `eph::core::LengthPrefixFramer` (which was 2-byte): the v3.3 codec is
  stateful, takes a `PacketView&` + `OutputBuffer&`, and widens the prefix.
- `eph/codec/raw_datagram_codec.hpp` — one frame per datagram `DatagramCodec`.
- `eph/codec/mold64_codec.hpp` — NASDAQ MoldUDP64 `DatagramCodec` with sequence +
  gap detection; emits N ITCH messages per packet through the sink callback.
- `eph/codec/detail/span_packet_view.hpp` — helper `PacketView` for tests.

### Notes
- Codecs were previously expressed as `MessageFramer` implementations spread across
  `eph-core` and `eph-transport`. v3.3 consolidated them here and changed their
  shape — `decode()` is now stateful (non-`const`) and takes an `OutputBuffer&`.
- The old `eph::transport::WsFramer` was a stateless RFC 6455 codec that pushed
  control-frame handling up to the transport layer. `WsCodec` absorbs that FSM so
  users never write WS control-frame handling code again.
