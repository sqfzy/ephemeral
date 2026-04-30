# Custom Codec guide

How to implement a custom `StreamCodec` or `DatagramCodec` for a proprietary wire
protocol. This is the replacement for the old `MessageFramer` concept — the
differences are: codecs are stateful (they can own reassembly buffers, FSMs, sequence
counters), and they can inject auto-responses via an `OutputBuffer&` so control-plane
logic stays out of user code.

## The concepts

Defined in `eph-core/include/eph/core/codec.hpp`:

```cpp
// TCP-style streaming codec — incremental decode, may need more data
template <class T>
concept StreamCodec = requires(T& t, typename T::PacketViewRef view,
                               core::OutputBuffer& out_sink,
                               uint8_t* out_buf, size_t out_cap) {
    typename T::Frame;
    typename T::PacketViewRef;
    // `view` carries application-layer plaintext; `out_sink` is the
    // auto-response staging buffer.
    { t.decode(view, out_sink) } -> std::same_as<
        std::expected<std::optional<typename T::Frame>, core::ErrorInfo>>;
    // Encode a single frame into `out_buf` (writable destination, capacity
    // `out_cap`). Returns bytes written or BufferFull / CodecBad.
    { t.encode(out_buf, out_cap, std::declval<typename T::Frame>()) }
        -> std::same_as<std::expected<size_t, core::ErrorInfo>>;
    { T::max_overhead } -> std::convertible_to<size_t>;
    { T::is_streaming } -> std::convertible_to<bool>;  // true
};

// UDP-style datagram codec — one complete packet in, 0/1/N frames out
template <class T>
concept DatagramCodec = requires(T& t, typename T::PacketViewRef dgram,
                                 core::OutputBuffer& out_sink,
                                 std::function<void(typename T::Frame)> sink,
                                 uint8_t* out_buf, size_t out_cap) {
    typename T::Frame;
    typename T::PacketViewRef;
    { t.decode(dgram, out_sink, sink) } -> std::same_as<
        std::expected<size_t, core::ErrorInfo>>;       // returns frame count
    { t.encode(out_buf, out_cap, std::declval<typename T::Frame>()) }
        -> std::same_as<std::expected<size_t, core::ErrorInfo>>;
    { T::max_overhead } -> std::convertible_to<size_t>;
    { T::is_streaming } -> std::convertible_to<bool>;  // false
};

template <class T>
concept Codec = StreamCodec<T> || DatagramCodec<T>;
```

`decode()` is a non-`const` instance method, unlike the old stateless `MessageFramer`.
The codec can carry per-connection state between calls.

## Built-in codecs

| Codec | Kind | Wire format | Notes |
|---|---|---|---|
| `eph::codec::WsCodec` | stream | RFC 6455 | Auto-responds ping/close, reassembles fragments |
| `eph::codec::RawStreamCodec` | stream | passthrough | Zero overhead, frames are whole buffers |
| `eph::codec::LengthPrefixCodec` | stream | 2-byte BE length prefix | Max payload 65 535 bytes |
| `eph::codec::RawDatagramCodec` | datagram | one frame per packet | Zero overhead |
| `eph::codec::Mold64Codec` | datagram | NASDAQ MoldUDP64 | Sequence + gap detection, emits N ITCH frames per packet |

## Example: a proprietary fixed-header stream codec

Protocol on the wire:

```
[msg_type : 1 byte] [length : 3 bytes big-endian] [payload : N bytes]
```

```cpp
#include <cstring>
#include <expected>
#include <optional>
#include <span>

#include "eph/core/codec.hpp"
#include "eph/core/error.hpp"

class FixedHeaderCodec {
public:
    static constexpr size_t kHeaderSize = 4;
    static constexpr size_t kMaxPayload = (1 << 24) - 1;  // 16 MiB
    static constexpr size_t max_overhead = kHeaderSize;
    static constexpr bool   is_streaming = true;

    struct Frame {
        uint8_t                     msg_type;
        std::span<const uint8_t>    payload;
    };

    // The codec is templated on PacketView so the same instantiation works
    // for both kernel (SpanView) and DPDK (MbufView).
    using PacketViewRef = eph::core::PacketView&;

    template <class PacketView>
    std::expected<std::optional<Frame>, eph::core::ErrorInfo>
    decode(PacketView& view, eph::core::OutputBuffer& /*out*/) {
        if (view.length() < kHeaderSize) {
            return std::nullopt;  // need more bytes
        }

        const uint8_t* p = view.data();
        const uint8_t  msg_type = p[0];
        const uint32_t payload_len =
            (static_cast<uint32_t>(p[1]) << 16) |
            (static_cast<uint32_t>(p[2]) << 8)  |
            (static_cast<uint32_t>(p[3]));

        if (payload_len > kMaxPayload) {
            return std::unexpected{eph::core::ErrorInfo{
                eph::core::Error::CodecOverflow, "payload > 16 MiB"}};
        }
        if (view.length() < kHeaderSize + payload_len) {
            return std::nullopt;  // need more bytes
        }

        Frame f{
            .msg_type = msg_type,
            .payload  = {p + kHeaderSize, payload_len},
        };

        // Consume the bytes from the view so the next decode() picks up after us
        view.trim_front(kHeaderSize + payload_len);
        return std::optional{f};
    }

    std::expected<size_t, eph::core::ErrorInfo>
    encode(uint8_t* buf, size_t cap, Frame f) {
        if (cap < kHeaderSize + f.payload.size()) {
            return std::unexpected{eph::core::ErrorInfo{
                eph::core::Error::BufferFull, "encode buffer too small"}};
        }
        buf[0] = f.msg_type;
        buf[1] = static_cast<uint8_t>((f.payload.size() >> 16) & 0xFF);
        buf[2] = static_cast<uint8_t>((f.payload.size() >> 8)  & 0xFF);
        buf[3] = static_cast<uint8_t>( f.payload.size()        & 0xFF);
        std::memcpy(buf + kHeaderSize, f.payload.data(), f.payload.size());
        return kHeaderSize + f.payload.size();
    }
};

static_assert(eph::core::StreamCodec<FixedHeaderCodec>);
```

## Plugging it into a Stream

```cpp
#include "eph/net/kernel/tcp_stream.hpp"

auto stream = eph::net::kernel::KernelTcpStream<FixedHeaderCodec, /*EnableTls=*/true>::create({
    .remote = eph::net::SocketAddr{ /* resolved IPv4 */, 9000 },
    .tls    = { .hostname = "gateway.example" },
}).value();
```

Compile-time template instantiation picks up the codec. No runtime registration, no
vtable, no indirection.

## Key requirements

1. **`decode()` must be non-const** — the codec owns per-connection state and mutates it.
2. **`decode()` must consume bytes via `view.trim_front(n)`** — the remaining bytes
   will be presented to the next decode call. Returning `std::nullopt` without
   trimming means "need more bytes, same view again."
3. **`decode()` may write to `OutputBuffer&`** — for auto-responses (WS pong, close
   ack, MoldUDP64 retransmit request). The Stream will flush the buffer after the
   decode call returns.
4. **`encode()` must fit within `cap`** — return `BufferFull` if not. Streams
   pre-allocate `payload + max_overhead` bytes.
5. **Zero-copy** — the `Frame::payload` span should point into the input `PacketView`,
   not into an owned buffer. Do not allocate on the hot path.
6. **`max_overhead` is a static constexpr size_t**, not a method. `is_streaming` is a
   static constexpr bool — `true` for `StreamCodec`, `false` for `DatagramCodec`.

## Datagram codec shape

`DatagramCodec::decode()` has a slightly different signature because one UDP packet
can yield multiple frames (MoldUDP64 wraps many ITCH messages in one datagram):

```cpp
template <class PacketView>
std::expected<size_t, eph::core::ErrorInfo>
decode(PacketView& dgram, eph::core::OutputBuffer& out,
       std::function<void(Frame)> sink)
{
    size_t emitted = 0;
    while (dgram.length() > 0) {
        auto f = parse_one_frame(dgram);
        if (!f) return std::unexpected{f.error()};
        sink(*f);
        ++emitted;
        dgram.trim_front(f->wire_size);
    }
    return emitted;
}
```

The framework will call `sink()` once per frame. The return value is purely the
count, for bookkeeping / metrics.

## Testing a codec without a live socket

Pair `FakeStream` + `TestPoller` to drive the codec from raw bytes:

```cpp
#include "eph/net/test/fake_stream.hpp"
#include "eph/net/test/test_poller.hpp"

TEST(FixedHeaderCodec, RoundTrip) {
    using Stream = eph::net::test::FakeStreamWithCodec<FixedHeaderCodec>;
    auto poller = eph::net::test::TestPoller<Stream>::create();
    auto fake   = Stream::create();
    poller->add(fake.get());

    uint8_t encoded[256];
    FixedHeaderCodec::Frame out{.msg_type = 0x42, .payload = {/*…*/}};
    auto n = FixedHeaderCodec{}.encode(encoded, sizeof(encoded), out);

    fake->inject_rx(std::span{encoded, *n});
    poller->poll();
    // assert on_message captured the decoded frame
}
```

## See also

- `docs/architecture.md` — the three concept layer
- `docs/poller-guide.md` — driving the stream with a Poller
- `eph-codec/include/eph/codec/ws_codec.hpp` — the canonical reference implementation
  for a stateful codec with auto-responses
- `eph-codec/include/eph/codec/mold64_codec.hpp` — the canonical DatagramCodec example
