# eph-codec changelog

## [Unreleased]

### Added (2026-04-28)
- `WsCodec` gains RFC 7692 `permessage-deflate` inflate support for
  inbound data frames:
  - `WsCodecConfig::permessage_deflate` (bool, default false) and
    `WsCodecConfig::server_no_context_takeover` (bool, default false)
    — typically left at default and toggled by
    `WsCodec::enable_permessage_deflate(server_no_ctx_takeover)`,
    which the WS-aware `KernelTcpStream` / `DpdkTcpStream` factories
    invoke after the HTTP upgrade response confirms the extension was
    negotiated.
  - Backed by a private `eph::codec::detail::WsInflater` thin RAII
    wrapper over zlib (`raw inflate`, no zlib stream wrapper). The
    inflater is constructed lazily on the first compressed frame and
    reused (with optional per-message reset under
    `server_no_context_takeover`).
  - Decompression-bomb cap: inflated payload is bounded by
    `WsCodecConfig::max_message_size`; exceeding it returns
    `CodecOverflow` mid-message instead of inflating to RAM
    exhaustion.
  - Spec compliance: `RSV1=1` on a control frame or on a non-leading
    continuation (RFC 7692 §6.1) is rejected with `WsFrameBad`.
    Mixed compressed / uncompressed messages on a single connection
    are supported — each message's deflate state is decided by its
    first frame's `RSV1` bit.
  - Outbound deflate (compressing client → server) is intentionally
    NOT implemented — crypto venues never require client-side
    compression and adding it would double the codec's surface.
- `tests/test_ws_codec_deflate.cpp` — 10-case suite covering basic
  deflate inflate, the `RSV1` rejection rules, the bomb cap,
  fragmented compressed messages, mixed compressed/uncompressed,
  and a malformed-deflate poison-pill regression added in batch 1.

### Build
- New public `add_syslinks("z", { public = true })` in `xmake.lua`
  for the zlib dependency. System zlib chosen over libdeflate
  because it is universally available on Linux (kernel + util-linux
  already pull it in) and the hot path inflates a few KB/s of
  bookticker JSON, well below libdeflate's regime of advantage.

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
