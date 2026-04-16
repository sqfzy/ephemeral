# eph-codec

Header-only C++23 module holding the stateful `StreamCodec` / `DatagramCodec`
implementations for the architecture.

Codecs are independent of any networking backend — they operate on a templated
`PacketView` parameter, so the same instantiation works against kernel
(`SpanView`) and DPDK (`MbufView`) streams without any runtime branching.

## Codecs provided

| Codec | Kind | Notes |
|---|---|---|
| `eph::codec::WsCodec` | `StreamCodec` | RFC 6455 WebSocket. Owns reassembly + ping/close FSM. Auto-responds ping/close via `OutputBuffer`. |
| `eph::codec::RawStreamCodec` | `StreamCodec` | Passthrough — frames are whole receive buffers. |
| `eph::codec::LengthPrefixCodec` | `StreamCodec` | 2-byte big-endian length prefix, ≤65 535 byte payloads. |
| `eph::codec::RawDatagramCodec` | `DatagramCodec` | One frame per datagram. |
| `eph::codec::Mold64Codec` | `DatagramCodec` | NASDAQ MoldUDP64. Tracks sequence + gap count, emits N ITCH messages per packet through the sink. |

All five satisfy `eph::core::Codec`.

## Usage

```cpp
#include "eph/codec/ws_codec.hpp"
#include "eph/net/kernel/tcp_stream.hpp"

using WsClient = eph::net::kernel::KernelTcpStream<eph::codec::WsCodec, /*Tls=*/true>;

auto client = WsClient::create({
    .remote_host = "ws.example.com",
    .remote_port = 443,
    .ws_path     = "/stream",
}).value();
```

The codec is a template parameter of the stream type, so the wire format, the TLS
state, and the socket state are all monomorphised into one class. There is no
runtime dispatch on the hot path.

## Building and testing

```bash
xmake build eph-codec
xmake build -g tests
xmake run test_ws_codec
xmake run test_mold64_codec
xmake run test_raw_stream_codec
xmake run test_length_prefix_codec
xmake run test_packet_view
```

Per-file targets are auto-globbed from `tests/test_*.cpp`.

## Writing a new codec

See [`docs/custom-codec.md`](../docs/custom-codec.md) in the repo root for the full
walkthrough.

## Dependencies

- `eph-core` — the `StreamCodec` / `DatagramCodec` concepts, `Error`, `ErrorInfo`,
  `OutputBuffer`, `PacketView`.

No dependency on any networking module. Application code links `eph-codec` plus
whichever backend it needs (`eph-net-kernel` or `eph-net-dpdk`).

## See also

- [`docs/architecture.md`](../docs/architecture.md) — concept model
- [`docs/custom-codec.md`](../docs/custom-codec.md) — writing a new codec
- `.artifacts/design-eph-v3.3-architecture-20260410.md` — frozen design spec
