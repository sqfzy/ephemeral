# Custom Framer Guide

How to implement a custom `MessageFramer` for proprietary wire protocols.

## The MessageFramer Concept

Every Transport is parameterized by a framer that handles encoding/decoding between application payloads and wire-format bytes. The concept requires three operations:

```cpp
template <typename F>
concept MessageFramer = requires(F f, ...) {
    { f.encode(out, in, len, msg_type) } noexcept -> std::same_as<size_t>;
    { F::decode(in, len) } noexcept -> std::same_as<std::expected<DecodedFrame, FrameError>>;
    { F::max_overhead() } noexcept -> std::convertible_to<size_t>;
};
```

## Built-in Framers

| Framer | Wire Format | Use Case |
|--------|-------------|----------|
| `WsFramer` | RFC 6455 WebSocket frames | Standard WebSocket servers |
| `RawFramer` | No framing (passthrough) | Raw TCP/TLS with app-level framing |
| `LengthPrefixFramer` | 2-byte big-endian length prefix | ITCH (MoldUDP64), binary protocols |

## Example: Fixed-Header Framer

A custom framer for a proprietary protocol with a 4-byte header:
```
[msg_type: 1 byte] [length: 3 bytes big-endian] [payload: N bytes]
```

```cpp
#include "eph/core/framer_concept.hpp"

class FixedHeaderFramer {
public:
    static constexpr size_t kHeaderSize = 4;  // 1 byte type + 3 bytes length
    static constexpr size_t kMaxPayload = (1 << 24) - 1;  // 16MB (3-byte length)

    /// Encode: write header + copy payload.
    size_t encode(uint8_t* out, const uint8_t* data, size_t len,
                  uint8_t msg_type) noexcept {
        if (len > kMaxPayload || !data || !out) return 0;

        // Header: [msg_type] [len_hi] [len_mid] [len_lo]
        out[0] = msg_type;
        out[1] = static_cast<uint8_t>((len >> 16) & 0xFF);
        out[2] = static_cast<uint8_t>((len >> 8) & 0xFF);
        out[3] = static_cast<uint8_t>(len & 0xFF);

        std::memcpy(out + kHeaderSize, data, len);
        return kHeaderSize + len;
    }

    /// Decode: parse header, return zero-copy view into buffer.
    [[nodiscard]] static std::expected<eph::net::DecodedFrame, eph::net::FrameError>
    decode(const uint8_t* data, size_t len) noexcept {
        if (len < kHeaderSize)
            return std::unexpected(eph::net::FrameError::kIncomplete);

        uint32_t payload_len =
            (static_cast<uint32_t>(data[1]) << 16) |
            (static_cast<uint32_t>(data[2]) << 8) |
            static_cast<uint32_t>(data[3]);

        if (payload_len > kMaxPayload)
            return std::unexpected(eph::net::FrameError::kPayloadTooLarge);
        if (len < kHeaderSize + payload_len)
            return std::unexpected(eph::net::FrameError::kIncomplete);

        return eph::net::DecodedFrame{
            .payload     = data + kHeaderSize,
            .payload_len = payload_len,
            .msg_type    = data[0],
            .is_control  = false,  // No control frames in this protocol
            .total_len   = kHeaderSize + payload_len,
        };
    }

    /// Maximum overhead: 4 bytes header.
    static constexpr size_t max_overhead() noexcept { return kHeaderSize; }
};

// Compile-time verification
static_assert(eph::net::MessageFramer<FixedHeaderFramer>);
```

## Using with Transport

```cpp
// Socket backend with custom framer
using MyTransport = eph::net::Transport<
    eph::net::SocketTransport,
    FixedHeaderFramer,     // <-- your custom framer
    65536,                 // MaxPayload
    1024                   // QueueDepth
>;

auto result = MyTransport::create(tcp_factory, transport_cfg);
```

## Key Requirements

1. **`encode()` must be `noexcept`** — called from the TX thread hot path
2. **`decode()` must be `static noexcept`** — called from the RX thread hot path
3. **`decode()` must handle partial data** — return `kIncomplete` if the buffer doesn't contain a full frame; Transport will call again with more data
4. **`decode()` must be zero-copy** — return a `DecodedFrame` pointing into the input buffer, not a copy
5. **`max_overhead()` must be accurate** — Transport pre-allocates `payload + max_overhead()` bytes for each encode call
6. **`is_control` is WS-specific** — set to `false` for non-WebSocket protocols; Transport uses it to bypass frame filtering for control frames
