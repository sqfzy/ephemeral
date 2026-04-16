# eph-codec summary

## Public API surface

Namespace: `eph::codec`. All types are header-only templates or stateful classes
that satisfy `eph::core::StreamCodec` or `eph::core::DatagramCodec`.

### `WsCodec` - RFC 6455 WebSocket (StreamCodec)

```cpp
class WsCodec {
public:
    using Frame         = std::span<const uint8_t>;
    using PacketViewRef = core::PacketView&;

    static constexpr size_t max_overhead = 14;
    static constexpr bool   is_streaming = true;

    explicit WsCodec(WsCodecConfig cfg = {});

    template <class PacketView>
    std::expected<std::optional<Frame>, core::ErrorInfo>
    decode(PacketView& view, core::OutputBuffer& out);

    std::expected<size_t, core::ErrorInfo>
    encode(uint8_t* buf, size_t cap, Frame payload);
};
```

Owns an internal reassembly buffer (for fragmented frames) and a control-frame
FSM. On `decode()`:

- Data frame: returned via `optional<Frame>`.
- Ping frame: consumed; pong written to `OutputBuffer`; returns `nullopt`.
- Close frame: close ack written to `OutputBuffer`; returns `Err(WsCloseReceived)`.
- Continuation frame: accumulated into reassembly buffer.

### `RawStreamCodec` - passthrough (StreamCodec)

Frames are whole receive buffers. Zero overhead. Useful for raw TCP protocols
and tests.

### `LengthPrefixCodec` - 2-byte BE length prefix (StreamCodec)

```cpp
class LengthPrefixCodec {
public:
    using Frame         = std::span<const uint8_t>;
    using PacketViewRef = core::PacketView&;

    static constexpr size_t max_overhead = 2;
    static constexpr bool   is_streaming = true;

    template <class PacketView>
    std::expected<std::optional<Frame>, core::ErrorInfo>
    decode(PacketView& view, core::OutputBuffer& out);

    std::expected<size_t, core::ErrorInfo>
    encode(uint8_t* buf, size_t cap, Frame payload);
};
```

Successor to the legacy `eph::core::LengthPrefixFramer`. Stateful and
PacketView-aware.

### `RawDatagramCodec` - one frame per datagram (DatagramCodec)

```cpp
class RawDatagramCodec {
public:
    using Frame = std::span<const uint8_t>;
    static constexpr size_t max_overhead = 0;
    static constexpr bool   is_streaming = false;

    template <class PacketView>
    std::expected<size_t, core::ErrorInfo>
    decode(PacketView& dgram, core::OutputBuffer& out,
           std::function<void(Frame)> sink);
};
```

### `Mold64Codec` - NASDAQ MoldUDP64 (DatagramCodec)

```cpp
class Mold64Codec {
public:
    struct Frame {
        uint64_t seq_num;
        std::span<const uint8_t> payload;    // one ITCH message
    };
    static constexpr size_t max_overhead = 20;
    static constexpr bool   is_streaming = false;

    explicit Mold64Codec(Mold64Config cfg);

    template <class PacketView>
    std::expected<size_t, core::ErrorInfo>
    decode(PacketView& dgram, core::OutputBuffer& out,
           std::function<void(Frame)> sink);

    std::expected<size_t, core::ErrorInfo>
    encode(uint8_t* buf, size_t cap, Frame frame);

    uint64_t expected_seq() const noexcept;
    uint64_t gap_count()    const noexcept;
};
```

Tracks the next expected sequence number and emits gap counts via `gap_count()`.
Each datagram may contain many ITCH messages; the sink is called once per
message.

## Dependencies

- `eph-core` (public) - the Codec concepts, Error types, OutputBuffer, PacketView.

## See also

- `README.md`
- `docs/ONBOARDING.md`
- `../docs/custom-codec.md` - writing a new codec
