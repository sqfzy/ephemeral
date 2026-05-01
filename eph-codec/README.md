# eph-codec

Header-only C++23 module holding the stateful `StreamCodec` / `DatagramCodec`
implementations for the architecture.

Codecs are independent of any networking backend — they operate on a templated
`PacketView` parameter, so the same instantiation works against kernel
(`SpanView`) and DPDK (`MbufView`) streams without any runtime branching.

## Codecs provided

| Codec | Kind | Notes |
|---|---|---|
| `eph::codec::WsCodec` | `StreamCodec` | RFC 6455 WebSocket + RFC 7692 permessage-deflate. Owns reassembly + ping/close FSM, optional zlib inflater (lazy-init, zero cost when disabled). Auto-responds ping/close via `OutputBuffer`; deflate enable is plumbed through `enable_permessage_deflate()` (the kernel + DPDK backends call it for you when `StreamConfig::ws.permessage_deflate` is true — the default — and the server accepts the offer). |
| `eph::codec::RawStreamCodec` | `StreamCodec` | Passthrough — frames are whole receive buffers. |
| `eph::codec::LengthPrefixCodec` | `StreamCodec` | 4-byte big-endian length prefix, payloads up to 16 MiB (`kMaxFrameLen`). |
| `eph::codec::RawDatagramCodec` | `DatagramCodec` | One frame per datagram. Empty datagrams rejected with `CodecBad`. |
| `eph::codec::Mold64Codec` | `DatagramCodec` | NASDAQ MoldUDP64. Tracks next expected sequence, accumulates `gap_count()` on forward jumps, delivers end-of-session markers (message_count == 0xFFFF) as 0 frames. Emits one `Frame{seq_num, payload}` per contained ITCH message through the sink; provides a test-oriented `encode()` for single-message datagrams. |

All five satisfy `eph::core::Codec`.

## Usage

```cpp
#include "eph/codec/ws_codec.hpp"
#include "eph/net/kernel/tcp_stream.hpp"

using WsClient = eph::net::kernel::KernelTcpStream<eph::codec::WsCodec, /*Tls=*/true>;

auto client = WsClient::create({
    .remote = eph::net::SocketAddr{ /* resolved IPv4 */, 443 },
    .tls    = { .hostname = "ws.example.com", .verify_peer = true },
    .ws     = { .path = "/stream" },
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
xmake run test_ws_codec_deflate
xmake run test_ws_codec_edge
xmake run test_mold64_codec
xmake run test_raw_stream_codec
xmake run test_raw_datagram_codec
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
- `zlib` — system package, used by `WsCodec`'s RFC 7692 permessage-deflate
  inflater. Available out of the box on every modern Linux distro
  (`pacman -S zlib` / `apt install zlib1g-dev` / etc.). The inflater is
  lazy-initialized only when `enable_permessage_deflate()` is called and
  a compressed frame arrives, so non-deflate consumers pay nothing at
  runtime.

No dependency on any networking module. Application code links `eph-codec` plus
whichever backend it needs (`eph-net-kernel` or `eph-net-dpdk`).

## See also

- [`docs/architecture.md`](../docs/architecture.md) — concept model
- [`docs/custom-codec.md`](../docs/custom-codec.md) — writing a new codec
- `.artifacts/design-eph-v3.3-architecture-20260410.md` — frozen design spec
