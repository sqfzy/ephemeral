# eph-codec onboarding

## What's in this module

`eph-codec` holds the stateful codec implementations for v3.3. Five codecs total:

- `WsCodec` — RFC 6455, the most complex (reassembly + control FSM).
- `RawStreamCodec` — passthrough for raw TCP.
- `LengthPrefixCodec` — 2-byte BE length prefix.
- `RawDatagramCodec` — one frame per UDP datagram.
- `Mold64Codec` — NASDAQ MoldUDP64 with sequence + gap tracking.

All five satisfy `eph::core::Codec` and are templated on `PacketView` so the same
instantiation plugs into either kernel or DPDK streams.

## How to read the code

Start with `include/eph/codec/raw_stream_codec.hpp` — it's the simplest
implementation and shows the full concept-satisfaction pattern. Then read
`include/eph/codec/ws_codec.hpp` for a realistic stateful codec with auto-responses.
Finally read `include/eph/codec/mold64_codec.hpp` for the `DatagramCodec` variant.

## Testing a codec

Each codec has a dedicated test file under `tests/` that uses
`eph/codec/detail/span_packet_view.hpp` as a simple in-memory `PacketView`:

```cpp
#include "eph/codec/detail/span_packet_view.hpp"
#include "eph/codec/ws_codec.hpp"

TEST(WsCodec, DecodesSingleTextFrame) {
    std::vector<uint8_t> bytes = {0x81, 0x05, 'h','e','l','l','o'};
    eph::codec::detail::SpanPacketView view{bytes.data(), bytes.size()};
    eph::core::OutputBuffer out{1024};
    eph::codec::WsCodec codec;

    auto r = codec.decode(view, out);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ(std::string_view{(const char*)(*r)->data(), (*r)->size()}, "hello");
}
```

## Running tests

```bash
xmake build -g tests
xmake run test_ws_codec
xmake run test_mold64_codec
xmake run test_length_prefix_codec
xmake run test_raw_stream_codec
xmake run test_raw_datagram_codec
xmake run test_packet_view
```

## Adding a new codec

1. Create `include/eph/codec/<name>_codec.hpp`.
2. Define a class with `Frame`, `PacketViewRef`, `max_overhead`, `is_streaming`,
   `decode()`, `encode()`.
3. Add `static_assert(eph::core::StreamCodec<MyCodec>);` (or `DatagramCodec`).
4. Create `tests/test_<name>_codec.cpp`. It will be auto-globbed into the test build.
5. Add a `bench_<name>_codec.cpp` under `benchmarks/` if the hot path matters.

See `../docs/custom-codec.md` for the full walkthrough including the PacketView
contract and auto-response patterns.

## See also

- `README.md`
- `summary.md`
- `CHANGELOG.md`
- `../docs/custom-codec.md`
- `../docs/architecture.md`
