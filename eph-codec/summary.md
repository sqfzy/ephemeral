# eph-codec summary

## Public API surface

Namespace: `eph::codec`. All types are header-only templates or stateful classes
that satisfy `eph::core::StreamCodec` or `eph::core::DatagramCodec`.

### `WsCodec` - RFC 6455 WebSocket + RFC 7692 deflate (StreamCodec)

```cpp
struct WsCodecConfig {
    std::size_t max_message_size = 1u << 20;  // 1 MiB reassembled payload cap
    bool        auto_pong        = true;      // auto-emit pong on ping
    bool        auto_close_ack   = true;      // auto-emit close-ack on close

    // RFC 7692 permessage-deflate inflate (inbound). Typically NOT set by
    // hand — leave both at default and let the WS-aware TcpStream factory
    // call `enable_permessage_deflate()` after the handshake confirms the
    // extension was negotiated (keeps the two flags in sync).
    bool permessage_deflate         = false;  // false until handshake-time enable
    bool server_no_context_takeover = false;  // reset inflater per message
};

class WsCodec {
public:
    using Frame         = std::span<const uint8_t>;
    using PacketViewRef = SpanPacketView&;

    static constexpr std::size_t max_overhead = 14;  // RFC 6455 max header
    static constexpr bool        is_streaming = true;

    WsCodec() noexcept;                              // default config
    explicit WsCodec(WsCodecConfig cfg) noexcept;

    /// Mirror the handshake's negotiated permessage-deflate result.
    /// Sets `permessage_deflate=true` and the matching
    /// `server_no_context_takeover` flag in one shot.
    void enable_permessage_deflate(bool server_no_ctx_takeover = false) noexcept;

    template <class PacketView>
    std::expected<std::optional<Frame>, core::ErrorInfo>
    decode(PacketView& view, core::OutputBuffer& out) noexcept;

    // Encodes a binary frame (opcode=kBinary, FIN=1) into buf.
    std::expected<std::size_t, core::ErrorInfo>
    encode(uint8_t* buf, std::size_t cap, Frame payload) noexcept;
};
```

Owns an internal reassembly buffer (for fragmented frames) and a control-frame
FSM. On `decode()`:

- Data frame (single, FIN=1): returned as `optional<Frame>`. If the peer sent it
  masked, the payload is unmasked into the reassembly buffer and the returned
  span is valid until the next `decode()` call.
- Ping frame: consumed; if `auto_pong`, a pong is written to `OutputBuffer`
  echoing the (unmasked) ping payload; returns `nullopt`.
- Pong frame: consumed; returns `nullopt`.
- Close frame: if `auto_close_ack`, a close-ack is written (with the peer's
  status code substituted for `kProtocolError` if the code is reserved /
  invalid per RFC 6455 §7.4.1); returns `Err(WsCloseReceived)`.
- Continuation frame: accumulated into the reassembly buffer, enforcing
  `max_message_size`; oversized reassembled payloads return `CodecOverflow`.
- Protocol violations (reserved bits, fragmented control frames, oversized
  control payloads, invalid opcode / length encoding / close code) return
  `WsFrameBad`. Codec state is NOT reset — the caller is expected to drop the
  connection.

When `permessage_deflate` is enabled (typically by the TcpStream factory
after handshake), data frames with `RSV1=1` are inflated through a private
zlib `z_stream` before being treated as plaintext. The inflated payload is
capped at `max_message_size` to bound decompression-bomb impact; exceeding
the cap returns `CodecOverflow` mid-message. Malformed deflate streams
return `CodecBad`. `RSV1=1` on a control frame or on a non-leading
continuation (RFC 7692 §6.1) is rejected with `WsFrameBad`. Mixed
compressed / uncompressed messages on a single connection are supported —
each message's deflate state is decided by its first frame's `RSV1` bit.
Outbound deflate (compressing client → server) is intentionally NOT
implemented.

### `LengthPrefixCodec` - 4-byte BE length prefix (StreamCodec)

```cpp
class LengthPrefixCodec {
public:
    using Frame         = std::span<const uint8_t>;
    using PacketViewRef = SpanPacketView&;

    static constexpr std::size_t max_overhead = 4;                     // 4-byte BE prefix
    static constexpr bool        is_streaming = true;
    static constexpr std::size_t kMaxFrameLen = 16u * 1024u * 1024u;   // 16 MiB

    constexpr LengthPrefixCodec() noexcept = default;

    template <class PacketView>
    std::expected<std::optional<Frame>, core::ErrorInfo>
    decode(PacketView& view, core::OutputBuffer& out) noexcept;

    std::expected<std::size_t, core::ErrorInfo>
    encode(uint8_t* buf, std::size_t cap, Frame payload) noexcept;
};
```

Wire format: `[BE uint32 payload_len][payload...]`. Widened from the legacy
2-byte `eph::core::LengthPrefixFramer` to a 4-byte prefix with an explicit
`kMaxFrameLen = 16 MiB` cap — oversized prefixes are rejected with
`CodecOverflow` rather than causing the decoder to wait forever for bytes that
may never arrive (slow-loris DoS resistance). Stateless; the stream state lives
entirely in the caller's PacketView window.

### `RawStreamCodec` - passthrough (StreamCodec)

Frames are whole receive buffers. Zero overhead. Useful for raw TCP protocols
and tests. Stateless; empty-view `decode()` returns `Ok(None)` so the caller
can re-poll.

### `RawDatagramCodec` - one frame per datagram (DatagramCodec)

```cpp
class RawDatagramCodec {
public:
    using Frame         = std::span<const uint8_t>;
    using PacketViewRef = SpanPacketView&;

    static constexpr std::size_t max_overhead = 0;
    static constexpr bool        is_streaming = false;

    constexpr RawDatagramCodec() noexcept = default;

    template <class PacketView>
    std::expected<std::size_t, core::ErrorInfo>
    decode(PacketView& dgram, core::OutputBuffer& out,
           const std::function<void(Frame)>& sink) noexcept;

    std::expected<std::size_t, core::ErrorInfo>
    encode(uint8_t* buf, std::size_t cap, Frame payload) noexcept;
};
```

Always emits exactly one frame per datagram; fully consumes the input via
`trim_front(len)`. Empty datagrams are rejected with `CodecBad`.

### `Mold64Codec` - NASDAQ MoldUDP64 (DatagramCodec)

```cpp
struct Mold64Config {
    uint64_t initial_expected_seq = 1;   // first delivered msg expected at seq 1
};

class Mold64Codec {
public:
    struct Frame {
        uint64_t                 seq_num;
        std::span<const uint8_t> payload;    // one ITCH message
    };
    using PacketViewRef = SpanPacketView&;

    static constexpr std::size_t max_overhead = 20;   // MoldUDP64 fixed header
    static constexpr bool        is_streaming = false;

    constexpr Mold64Codec() noexcept = default;
    constexpr explicit Mold64Codec(Mold64Config cfg) noexcept;

    template <class PacketView>
    std::expected<std::size_t, core::ErrorInfo>
    decode(PacketView& dgram, core::OutputBuffer& out,
           const std::function<void(Frame)>& sink) noexcept;

    // Test-oriented single-message encoder (NOT a full producer: no batching,
    // retransmit, or session-id support). Applications wanting a real producer
    // build the datagram themselves.
    std::expected<std::size_t, core::ErrorInfo>
    encode(uint8_t* buf, std::size_t cap, Frame frame) noexcept;

    uint64_t expected_seq() const noexcept;
    uint64_t gap_count()    const noexcept;
};
```

Tracks the next expected sequence number; forward jumps increment `gap_count()`,
duplicate / replay (seq < expected) is still delivered but does not rewind the
counter. End-of-session marker (`message_count == 0xFFFF`) delivers zero frames.
Malformed headers return `CodecBad` (the datagram is consumed regardless, per
the `DatagramCodec` contract). Each datagram may contain many ITCH messages;
the sink is called once per message with its absolute sequence number.

## Dependencies

- `eph-core` (public) - the Codec concepts, Error types, OutputBuffer, PacketView.
- `eph-net`  (public) - TLS detail headers + WS wire helpers consumed by
  `WsCodec` (handshake / framing / control-frame state machine).
- `eph-itch` (public) - `parse_moldudp64` (Mold64Codec is a thin wrapper).
- `aws-lc`   (public) - SHA-1 / random for the `Sec-WebSocket-Accept` path
  reused via `eph::net::detail::websocket`.
- `zlib`     (public, syslink `z`) - raw-deflate inflater backing the
  RFC 7692 `permessage-deflate` path. System zlib chosen over libdeflate
  because it is universally available on Linux (kernel + util-linux
  already pull it in) and the hot path inflates a few KB/s of bookticker
  JSON, well below libdeflate's regime of advantage.

## See also

- `README.md`
- `docs/ONBOARDING.md`
- `../docs/custom-codec.md` - writing a new codec
