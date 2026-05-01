# ephemeral

Header-only C++23 ultra-low-latency library for high-frequency trading.

Dual networking backends — POSIX sockets (**eph-net-kernel**) and DPDK kernel-bypass
(**eph-net-dpdk**) — behind a single `Stream` / `Datagram` / `Poller` concept layer.
Every per-connection type is a monomorphized `TcpStream<Codec>` or `UdpSocket<Codec>`
that owns its wire state, TLS state, and codec state inline. Zero virtual dispatch on
the hot path.

## Why ephemeral

- **Zero-copy everywhere.** FIX, ITCH, JSON, and WebSocket codecs all work against a
  `PacketView` that allows in-place mutation. The DPDK path TLS-decrypts straight into
  the mbuf so the codec sees plaintext without a single memcpy.
- **Compile-time polymorphism.** C++20 concepts replace virtual dispatch. The
  `eph::net::Stream` concept lets you swap kernel / DPDK / test backends without
  touching application code. One `Poller` drives any mix of `TcpStream` and `UdpSocket`
  instances — single-connection and multi-connection programs use the same API.
- **Tokio-aligned naming.** `TcpStream` / `UdpSocket` / `Poller` mirror
  `tokio::net::{TcpStream,UdpSocket}` / `mio::Poll`. The concept names and lifetimes
  read the same way in Rust and C++.
- **Production building blocks.** TLS 1.3 (aws-lc), WebSocket (RFC 6455), HTTP/1.1,
  MoldUDP64, FIX 4.4, ITCH 5.0, OUCH, order books, latency histograms, CPU pinning,
  hugepage allocators, audit logs — all header-only and composable.
- **Physical kernel/DPDK separation.** The DPDK build weight lives entirely in
  `eph-net-dpdk`. CI hosts never need DPDK installed to build or test the kernel path.

## Architecture

```
                        ┌───────────────┐
                        │   eph-core    │  StreamCodec / DatagramCodec / ErrorInfo
                        └───────┬───────┘
                                │
                    ┌───────────┼───────────┐
                    ↓           ↓           ↓
             ┌─────────┐  ┌──────────┐  ┌─────────────────────┐
             │eph-codec│  │ eph-net  │  │ eph-fix / eph-itch  │
             │WsCodec  │  │Stream /  │  │ eph-json / eph-book │
             │Mold64   │  │Datagram /│  │ (satisfy Codec      │
             │Raw/…    │  │Poller    │  │  concept)           │
             └─────────┘  │concepts  │  └─────────────────────┘
                          │+ test    │
                          │ mocks    │
                          └────┬─────┘
                               │
                      ┌────────┴────────┐
                      ↓                 ↓
              ┌──────────────┐   ┌───────────────┐
              │eph-net-kernel│   │ eph-net-dpdk  │
              │  epoll +     │   │  lcore burst +│
              │  KernelTcp/  │   │  DpdkTcp/Udp/ │
              │  Udp/Poller  │   │  Poller + Eal │
              └──────────────┘   └───────────────┘
```

See [`docs/architecture.md`](docs/architecture.md) for the full story and
[`summary.md`](summary.md) for the module-by-module breakdown.

## Quick Start

### Prerequisites

- **GCC ≥ 13** or **Clang ≥ 17** (C++23: `std::expected`, `std::format`)
- [xmake](https://xmake.io) build system
- `aws-lc` (auto-fetched via xmake) for TLS / HMAC / CSPRNG
- **Optional**: system libdpdk via pkg-config for `eph-net-dpdk` + `*_dpdk` targets
  (`sudo pacman -S dpdk` / `sudo apt install libdpdk-dev`, or build from source)
- **Optional**: `numactl` for NUMA-aware allocation

### Build

```bash
git clone https://github.com/sqfzy/ephemeral.git
cd ephemeral

xmake -m release          # release build
xmake -m debug            # debug build (SPDLOG_LEVEL_TRACE)
xmake build -g tests      # build all tests
xmake run  -g tests       # run all tests
xmake build -g examples   # build all examples
xmake build -g benchmarks # build all microbenchmarks
```

DPDK builds use the distribution's libdpdk via pkg-config (`sudo pacman -S dpdk`,
`sudo apt install libdpdk-dev`, or build from source). No compiler wrapper is
required. See [`docs/dpdk-setup.md`](docs/dpdk-setup.md) for hugepages, vfio-pci
binding, and EAL runtime setup.

## Usage Examples

### Example 1 — kernel WebSocket client (CI / dev / test hosts)

Connect to a TLS WebSocket endpoint, hand the bytes to `WsCodec`, emit frames on a user
callback.

```cpp
#include "eph/net/kernel/poller.hpp"
#include "eph/net/kernel/tcp_stream.hpp"
#include "eph/codec/ws_codec.hpp"

namespace en = eph::net::kernel;
namespace ec = eph::codec;
using namespace std::chrono_literals;

int main() {
    auto poller = en::KernelPoller::create({}).value();

    // EnableTls is the second template parameter (default false). Setting
    // it true folds the TLS 1.3 handshake into create(). Resolve the
    // host's IP via your usual mechanism (sync getaddrinfo or
    // eph::dpdk::dns::resolve on the DPDK path) and pass it as a
    // SocketAddr.
    auto stream = en::KernelTcpStream<ec::WsCodec, /*EnableTls=*/true>::create({
        .remote = en::SocketAddr{ /* resolved IPv4 */, 443 },
        .tls    = { .hostname = "fstream.binance.com" },
        .ws     = { .path = "/ws/btcusdt@bookTicker" },
    }).value();

    stream->on_message = [](std::span<const uint8_t> app_frame) {
        std::string_view msg{
            reinterpret_cast<const char*>(app_frame.data()),
            app_frame.size()};
        spdlog::info("market data ({} bytes): {}", app_frame.size(), msg);
    };

    poller->add(stream.get()).value();

    while (running) {
        poller->poll(100ms);        // epoll_wait under the hood
    }
}
```

### Example 2 — DPDK HFT client with TCP order channel + UDP market data

One `DpdkPoller` drives a TLS WebSocket `TcpStream` (order channel) and a multicast
`UdpSocket` running a `Mold64Codec` (ITCH feed) on the same lcore.

```cpp
#include "eph/net/dpdk/eal.hpp"
#include "eph/net/dpdk/poller.hpp"
#include "eph/net/dpdk/tcp_stream.hpp"
#include "eph/net/dpdk/udp_socket.hpp"
#include "eph/codec/ws_codec.hpp"
#include "eph/codec/mold64_codec.hpp"

namespace en = eph::net::dpdk;
namespace ec = eph::codec;

int main(int argc, char** argv) {
    en::Eal eal{argc, argv};      // RAII DPDK EAL init

    auto poller = en::DpdkPoller<>::create({
        .port_id  = 0,
        .queue_id = 0,
        .lcore    = 4,
    }).value();

    // Order channel — TLS 1.3 WebSocket over DPDK TCP
    auto order_ch = en::DpdkTcpStream<ec::WsCodec>::create({
        .remote_host = "fix.exchange.example",
        .remote_port = 443,
    }).value();
    order_ch->on_message = handle_exec_report;
    poller->add(order_ch.get()).value();

    // Market data — CME ITCH multicast
    auto md = en::DpdkUdpSocket<ec::Mold64Codec>::create({
        .bind_addr = eph::net::SocketAddr{{0,0,0,0}, 30000},
    }).value();
    md->on_datagram = handle_itch_message;
    md->join_multicast({{233,54,12,111}, 30001}).value();
    poller->add(md.get()).value();

    // Single loop drives both TCP and UDP on the same lcore
    while (running) {
        poller->poll();
    }
}
```

### Example 3 — unit-testing codec + app logic without syscalls

```cpp
#include "eph/net/test/fake_stream.hpp"
#include "eph/net/test/test_poller.hpp"

namespace ent = eph::net::test;

TEST(MyApp, RoutesWsPingToPong) {
    auto poller = ent::TestPoller<ent::FakeStream>::create();
    auto fake   = ent::FakeStream::create();
    poller->add(fake.get());

    fake->inject_rx({0x89, 0x00});      // WS ping frame
    poller->poll();

    auto tx = fake->collect_tx();
    EXPECT_EQ(tx[0], 0x8A);              // WS pong frame emitted by codec
}
```

More examples live under `examples/` and the integration tests under
`tests/integration/`. See [`docs/poller-guide.md`](docs/poller-guide.md) for
multi-connection patterns and heterogeneous `TcpStream` + `UdpSocket` on one poller.

## Modules

| Module | Headers | Description |
|---|---|---|
| **eph-core** | `error`, `codec`, `error_traits`, `metrics_concept`, `framer_concept`, `length_prefix_framer`, `parse_number`, `packet_view` | `Error` / `ErrorInfo` / `StreamCodec` / `DatagramCodec` / `PacketView` contract plus legacy framer primitives still consumed by the parser modules. |
| **eph-utils** | `time` (TSC), `cpu`, `hugepage`, `hdr_histogram`, `audit_log`, `recorder`, `system_stats`, `ema`, `alignment` | TSC timing, CPU pinning, hugepage allocator, histograms, audit logging, recorders. |
| **eph-containers** | `bounded_queue`, `evicting_queue`, `ring_buffer`, `*_bytes` variants | Lock-free SPSC queues and byte-level variants used by the transports. |
| **eph-codec** | `ws_codec`, `raw_stream_codec`, `length_prefix_codec`, `raw_datagram_codec`, `mold64_codec` | Stateful codecs satisfying `StreamCodec` / `DatagramCodec`. WS auto-responds ping/close; Mold64 emits N ITCH frames per datagram. |
| **eph-net** | `concepts`, `socket_addr`, `reconnect_policy`, `tcp_state`, `test/fake_stream`, `test/test_poller`, `test/fake_datagram`, `detail/tls_session`, `detail/websocket` | The `Stream` / `Datagram` / `Pollable` / `Poller` concepts, shared value types, test mocks, and the shared TLS / WS wire helpers both backends use. |
| **eph-net-kernel** | `tcp_stream`, `udp_socket`, `poller`, `config` | `KernelTcpStream<C,Tls>`, `KernelUdpSocket<C>`, `KernelPoller` (epoll). Contiguous `SpanView` `PacketView`. |
| **eph-net-dpdk** | `tcp_stream`, `udp_socket`, `poller`, `eal`, `config`, plus internal `dpdk/*` primitives (arp, dns, flow_steering, packet templates, net_header, multicast) | `DpdkTcpStream<C,Tls>`, `DpdkUdpSocket<C>`, `DpdkPoller<>`, `Eal`. `MbufView` `PacketView` with in-place TLS decrypt. |
| **eph-fix** | `parser`, `builder`, `framer`, `session`, `orders`, `order_manager`, `risk_check`, `position`, `execution_report`, `tags` | FIX 4.4 zero-copy parser/builder, session management, order helpers, risk checks. |
| **eph-itch** | `parser`, `framer`, `messages`, `moldudp64`, `soupbintcp`, `ouch` | ITCH 5.0 / OUCH zero-copy parser, MoldUDP64 and SoupBinTCP framers. |
| **eph-json** | `parser`, `framer`, adapters: `binance`, `bybit`, `okx` | Zero-copy JSON parser with exchange-specific adapters. |
| **eph-book** | `array_book`, `map_book`, `binance_adapter`, `itch_adapter`, `signals` | L2 order books (array / map), exchange adapters, signals. |

## Benchmarks

Microbenchmarks use Google Benchmark and live per-module under `<module>/benchmarks/`.
End-to-end latency benchmarks live under `benchmarks/latency/`: one
`lat_<scenario>[_dpdk]` client binary per scenario, all served by the single
`benchmarks/mockex/mockex` binary (`mockex --scenario <name>` dispatches to the
matching handler). The mock side is **always kernel** so the kernel-vs-DPDK
comparison is inherently fair — only the client path differs (see
[`docs/latency-benchmark-fairness.md`](docs/latency-benchmark-fairness.md)).

```bash
xmake build lat_tcp lat_udp lat_ws lat_ex_market lat_ex_order lat_ex_md_udp
xmake build lat_tcp_dpdk lat_udp_dpdk lat_ws_dpdk \
            lat_ex_market_dpdk lat_ex_order_dpdk lat_ex_md_udp_dpdk

# Edit benchmarks/latency/config.toml once, then:
sudo ./benchmarks/latency/lat tcp                # raw TCP RTT, kernel client
sudo ./benchmarks/latency/lat udp --dpdk         # raw UDP RTT, DPDK client
sudo ./benchmarks/latency/lat ex_market          # exchange bookTicker push
```

Each `lat_*` binary reports a 4-leg breakdown (RTT / TX / RX / SRV) in TSC nanoseconds,
computed from four timestamps stamped into the payload: `client_send`, `server_recv`,
`server_send`, `client_recv`. See `benchmarks/latency/core/tsc_protocol.hpp` and
`core/runner.hpp` for the exact measurement points.

## Documentation

- [`summary.md`](summary.md) — architecture overview, module map, data flow
- [`docs/architecture.md`](docs/architecture.md) — concept model for new contributors
- [`docs/poller-guide.md`](docs/poller-guide.md) — the Poller concept with examples
- [`docs/custom-codec.md`](docs/custom-codec.md) — writing a new Codec
- [`docs/dpdk-setup.md`](docs/dpdk-setup.md) — DPDK environment setup
- [`docs/production-config.md`](docs/production-config.md) — production deployment
- [`docs/operations-runbook.md`](docs/operations-runbook.md) — operations runbook
- [`docs/troubleshooting.md`](docs/troubleshooting.md) — troubleshooting guide
- [`docs/latency-benchmark-fairness.md`](docs/latency-benchmark-fairness.md) — kernel-vs-DPDK methodology
- [`docs/multi-connection.md`](docs/multi-connection.md) — multi-connection patterns
- [`docs/binance-protocols.md`](docs/binance-protocols.md) — Binance protocol details
- `.artifacts/design-eph-v3.3-architecture-20260410.md` — frozen design spec

## License

See [LICENSE](LICENSE) for details.
