# eph-codec changelog

## v3.3 (2026-04-10) — module introduced

`eph-codec` was created during v3.3 Phase 1 to hold stateful codec implementations
separate from networking backends. See
`.artifacts/design-eph-v3.3-architecture-20260410.md` for the full design.

### Added
- `eph/codec/ws_codec.hpp` — RFC 6455 WebSocket codec. Owns reassembly buffers and
  the ping/pong/close FSM. Auto-responds to control frames via `OutputBuffer`.
- `eph/codec/raw_stream_codec.hpp` — passthrough `StreamCodec`.
- `eph/codec/length_prefix_codec.hpp` — 2-byte BE length prefix `StreamCodec`.
  Distinct from the legacy `eph::core::LengthPrefixFramer`: the v3.3 codec is
  stateful and takes a `PacketView&` + `OutputBuffer&`.
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
