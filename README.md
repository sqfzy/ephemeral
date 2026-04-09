# ephemeral

Header-only C++23 ultra-low-latency library for high-frequency trading.

Dual networking backends -- POSIX sockets and DPDK kernel-bypass -- behind compile-time templated transports. Three transport variants for different latency/threading tradeoffs: `Transport` (threaded TX+RX), `DirectTxTransport` (direct TX, threaded RX), and `DirectTransport` (no threads, app polls). Zero virtual dispatch.

## Why ephemeral

- **Zero-copy everywhere** -- FIX, ITCH, and JSON parsers operate directly on receive buffers with no intermediate allocations.
- **Compile-time polymorphism** -- Concepts and templates replace virtual dispatch. The `TcpTransport` concept lets you swap Socket/DPDK backends without touching application logic. Three transport variants (`Transport`, `DirectTxTransport`, `DirectTransport`) let you choose the threading model that fits your latency budget.
- **Production building blocks** -- Circuit breakers, kill switches, rate limiters, HMAC signing, audit logs, and risk checks are included, not bolted on.
- **Single-header modules** -- Each module is header-only with clean dependency edges. Use only what you need.

## Performance

Measured on AWS Graviton (ARM64), 6-round average +/- sigma:

| Metric | Socket | DPDK | Speedup |
|--------|--------|------|---------|
| TX queue (p50) | 2,544 +/- 431 ns | 360 +/- 14 ns | **7.1x** |
| RX pipeline (p50) | 3,369 +/- 130 ns | 423 +/- 12 ns | **8.0x** |
| Handshake | 47.4 +/- 1.2 ms | 13.5 +/- 1.6 ms | **3.5x** |
| JSON parse (bookTicker) | 104 ns | -- | -- |
| JSON -> Book -> BBO | 163 ns | -- | -- |
| FIX parse (NewOrder) | 22.5 ns | -- | -- |
| ITCH parse | 1.4 ns | -- | -- |
| Book update | 2.9 ns | -- | -- |

## Quick Start

### Prerequisites

- **GCC >= 13** or **Clang >= 17** (C++23: `std::expected`, `std::format`)
- [xmake](https://xmake.io) build system
- **Optional:** DPDK (via vcpkg) for kernel-bypass backend
- **Optional:** aws-lc for TLS support
- **Optional:** numactl for NUMA-aware allocation

### Build

```bash
git clone https://github.com/sqfzy/ephemeral.git
cd ephemeral

# Release build
xmake -m release

# Debug build
xmake -m debug
```

### Run Tests

```bash
xmake build -g tests
xmake run test_fix
xmake run test_websocket

# Run all non-DPDK tests:
for t in test_alignment test_bounded_queue test_bounded_queue_bytes \
         test_cpu test_evicting_queue test_evicting_queue_bytes \
         test_fix test_framer test_hdr_histogram test_http test_itch \
         test_proxy test_record test_recorder test_socket_transport \
         test_system_stats test_tcp_concept test_time test_tls_record \
         test_transport test_transport_types test_version test_websocket \
         test_hugepage test_audit_log test_gateway test_kill_switch \
         test_metrics_concept test_itch_adapter test_binance_adapter; do
  xmake run $t
done
```

### Run Benchmarks

```bash
# Microbenchmarks (Google Benchmark)
xmake build -g benchmarks
xmake run bench_fix_parse
xmake run bench_itch_parse
xmake run bench_json_parse
xmake run bench_array_book

# End-to-end latency benchmarks (self-contained, dual-NIC back-to-back)
# Each lat_* binary forks its own kernel mock and runs the bench client.
xmake build lat_tcp lat_udp lat_ws lat_ex_market lat_ex_order lat_ex_md_udp
xmake build lat_tcp_dpdk lat_udp_dpdk lat_ws_dpdk \
            lat_ex_market_dpdk lat_ex_order_dpdk lat_ex_md_udp_dpdk

# Edit benchmarks/latency/bench.conf once with your NIC/IP/CPU layout, then:
sudo ./scripts/lat tcp                # raw TCP RTT (kernel client)
sudo ./scripts/lat udp --dpdk         # raw UDP RTT (DPDK client)
sudo ./scripts/lat ex_market          # exchange bookTicker push
```

## Modules

| Module | Headers | Description |
|--------|---------|-------------|
| **eph-core** | `tcp_concept`, `framer_concept`, `metrics_concept`, `error_traits`, `transport_errors` | Shared concepts and traits -- `TcpTransport`, `MessageFramer`, `ErrorEnum` |
| **eph-utils** | `time`, `cpu`, `hugepage`, `hdr_histogram`, `audit_log`, `recorder`, `system_stats`, `ema`, `alignment` | TSC timing, CPU pinning, hugepage allocator, histogram, audit logging |
| **eph-containers** | `bounded_queue`, `evicting_queue`, `ring_buffer`, `*_bytes` variants | Lock-free SPSC queues: `BoundedQueue` (backpressure), `EvictingQueue` (drop-oldest) |
| **eph-transport** | `transport`, `direct_tx_transport`, `direct_transport`, `transport_core`, `tx_worker`, `rx_worker`, `frame_processor`, `reconnect_policy`, `tls_session`, `websocket`, `http`, `transport_types`, `presets` | Composable WebSocket/TLS transport with 3 threading variants, built from independent components (TransportCore, TxWorker, RxWorker, FrameProcessor, ReconnectPolicy) |
| **eph-net** | `socket_transport`, `socket_config`, `http_client`, `gateway`, `circuit_breaker`, `kill_switch`, `rate_limiter`, `proxy`, `hmac` | Socket backend, HTTP client, HMAC signing, connection lifecycle management |
| **eph-dpdk** | `tcp`, `arp`, `dns`, `reactor`, `flow_steering`, `eal`, `connector` | DPDK kernel-bypass TCP backend (same Transport API) |
| **eph-fix** | `parser`, `builder`, `framer`, `session`, `orders`, `order_manager`, `risk_check`, `position`, `execution_report`, `tags` | FIX 4.4 zero-copy parser/builder, session management, order helpers, risk checks |
| **eph-itch** | `parser`, `framer`, `messages`, `moldudp64`, `soupbintcp`, `ouch` | ITCH 5.0 / OUCH zero-copy parser, MoldUDP64 and SoupBinTCP framers |
| **eph-json** | `parser`, `framer`, adapters: `binance`, `bybit`, `okx` | Zero-copy JSON parser with exchange-specific adapters |
| **eph-book** | `array_book`, `map_book`, `binance_adapter`, `itch_adapter`, `signals` | L2 order book (array and map variants), exchange adapters |

### Dependency Graph

```
eph-core  (concepts, error traits)
  |
  +-- eph-utils  (time, cpu, hugepage, histogram, audit)
  |     |
  |     +-- eph-containers  (SPSC queues, ring buffer)
  |           |
  |           +-- eph-transport  (Transport, DirectTxTransport, DirectTransport, TLS, WebSocket)
  |           |     |
  |           |     +-- eph-net  (socket backend, HTTP client, gateway)
  |           |     |
  |           |     +-- eph-dpdk  (DPDK kernel-bypass backend)
  |           |
  |           (eph-net and eph-dpdk provide TcpTransport implementations for eph-transport)
  |
  +-- eph-fix  (FIX protocol)
  +-- eph-itch  (ITCH/OUCH protocol)
  +-- eph-json  (JSON parser, exchange adapters)

eph-book  (order book -- standalone, integrates with eph-json and eph-itch)
```

## Usage Examples

### Minimal WebSocket Client

Connect to a WebSocket server, send a message, receive the echo:

```cpp
#include "eph/net/socket_transport.hpp"
#include "eph/transport/transport.hpp"

int main() {
    // 1. Configure
    eph::net::SocketConfig sock_cfg{
        .host = "echo.websocket.org", .port = 443, .tcp_nodelay = true,
        .bind_device = "",  // set to e.g. "ens35" for SO_BINDTODEVICE (NIC pinning)
    };
    eph::transport::TransportConfig transport_cfg{
        .remote_host = "echo.websocket.org",
        .remote_port = 443,
        .use_tls     = true,
    };

    // 2. TCP factory
    auto tcp_factory = [&sock_cfg]()
        -> std::expected<std::unique_ptr<eph::net::SocketTransport>, std::string> {
        auto tcp = std::make_unique<eph::net::SocketTransport>(sock_cfg);
        auto result = tcp->connect(std::chrono::milliseconds{5000});
        if (!result) return std::unexpected(result.error());
        return tcp;
    };

    // 3. Connect (TCP -> TLS handshake -> WebSocket upgrade)
    //    Transport: threaded TX+RX (also available: DirectTxTransport, DirectTransport)
    auto result = eph::transport::Transport<eph::net::SocketTransport>::create(
        std::move(tcp_factory), transport_cfg);
    if (!result) return 1;
    auto& tp = **result;

    // 4. Send and receive
    std::string msg = "hello ephemeral";
    tp.send_text(msg.data(), msg.size());
    tp.recv([](const uint8_t* data, size_t len) {
        spdlog::info("Received: {}", std::string_view(
            reinterpret_cast<const char*>(data), len));
    });

    tp.stop();
}
```

### Binance Order Book (WebSocket -> JSON -> Book -> BBO)

```cpp
#include "eph/net/socket_transport.hpp"
#include "eph/json/parser.hpp"
#include "eph/json/adapters/binance.hpp"
#include "eph/book/binance_adapter.hpp"

// ... transport setup same as above, connecting to fstream.binance.com ...

eph::book::BinanceBookAdapter<5> adapter;

tp.recv([&](const uint8_t* data, size_t len) {
    auto json   = eph::json::parse(data, len);
    auto ticker = eph::json::binance::BookTicker::from(json.value());
    if (!ticker) return;

    adapter.update_from_ticker(*ticker);

    const auto& book = adapter.book();
    auto mid    = book.mid_price();
    auto spread = book.spread();
    // mid and spread are std::optional<double>
});
```

## Examples

| Example | Backend | Description |
|---------|---------|-------------|
| `minimal_ws_client` | Socket | Simplest possible WebSocket client |
| `binance_book` | Socket | JSON -> BookTicker -> ArrayBook -> BBO (full pipeline) |
| `production_client` | Socket | Reconnection, latency histogram, CPU pinning |
| `simple_hft` | Socket | Binance market data with nanosecond timing |
| `simple_hft_dpdk` | DPDK | Same as above, kernel-bypass |
| `fix_trading_demo` | -- | FIX session, order management, risk checks |
| `itch_feed_demo` | -- | ITCH 5.0 message parsing |
| `ws_echo_client` | Socket | Full-featured echo client with CLI |
| `ws_echo_client_dpdk` | DPDK | DPDK variant of echo client |
| `dpdk_quickstart` | DPDK | DPDK connection helper with DNS resolution |
| `ws_via_proxy` | Socket | WebSocket through HTTP proxy |
| `spsc_queue_demo` | -- | BoundedQueue + EvictingQueue usage |
| `framer_showcase` | -- | WsFramer, RawFramer, LengthPrefixFramer |
| `perf_tuning_basics` | -- | TSC calibration, CPU affinity, hugepages |

Build and run any example:

```bash
xmake build minimal_ws_client
xmake run minimal_ws_client
xmake run minimal_ws_client --host myserver.com --port 8080 --no-tls
```

## Integration

ephemeral is header-only. Add the include paths to your build system:

```lua
-- xmake.lua
add_requires("spdlog", "aws-lc")
target("my_app")
    set_kind("binary")
    add_files("src/*.cpp")
    add_includedirs(
        "path/to/ephemeral/eph-core/include",
        "path/to/ephemeral/eph-utils/include",
        "path/to/ephemeral/eph-containers/include",
        "path/to/ephemeral/eph-transport/include",
        "path/to/ephemeral/eph-net/include")
    add_packages("spdlog", "aws-lc")
```

Or use xmake's dependency mechanism within a monorepo:

```lua
add_deps("eph-net")  -- pulls in eph-transport, eph-core, eph-utils, eph-containers transitively
```

## Benchmarks

Microbenchmarks live in `benchmarks/` and use Google Benchmark. End-to-end latency benchmarks are self-contained in `benchmarks/latency/` with a built-in mock WebSocket server (no Binance or other exchange dependency required).

Latency benchmarks (one binary per scenario, kernel + DPDK client variants):
- `lat_tcp` / `lat_udp` / `lat_ws` -- raw TCP / UDP / plain WebSocket RTT
- `lat_ex_market` -- exchange bookTicker push (1-leg)
- `lat_ex_order` -- exchange order RTT (N-inflight pipeline)
- `lat_ex_md_udp` -- exchange UDP market data RTT
- `scripts/lat <scenario> [--dpdk]` -- single-command runner that owns NIC-B state transitions (host kernel ↔ bench_ns ↔ vfio-pci) and execs the binary; the binary itself forks the kernel mock and runs the bench client

Each `lat_*` binary reports a 4-leg breakdown (RTT / TX / RX / SRV) in TSC nanoseconds, computed from four timestamps stamped into the payload: `client_send`, `server_recv`, `server_send`, `client_recv`. See `benchmarks/latency/core/tsc_protocol.hpp` and `core/runner.hpp` for the exact measurement points.

## Documentation

- [`summary.md`](summary.md) -- Architecture overview, module map, data flow
- [`docs/dpdk-setup.md`](docs/dpdk-setup.md) -- DPDK environment setup
- [`docs/production-config.md`](docs/production-config.md) -- Production deployment configuration
- [`docs/operations-runbook.md`](docs/operations-runbook.md) -- Operations runbook
- [`docs/troubleshooting.md`](docs/troubleshooting.md) -- Troubleshooting guide
- [`docs/latency-benchmark-fairness.md`](docs/latency-benchmark-fairness.md) -- Socket vs DPDK benchmark methodology
- [`docs/multi-connection.md`](docs/multi-connection.md) -- Multi-connection patterns
- [`docs/custom-framer.md`](docs/custom-framer.md) -- Writing custom message framers
- [`docs/binance-protocols.md`](docs/binance-protocols.md) -- Binance protocol details

## License

See [LICENSE](LICENSE) for details.
