# ephemeral

Header-only C++23 ultra-low-latency WebSocket/TLS client library for high-frequency trading.

Dual networking backends — POSIX sockets and DPDK kernel-bypass — behind a single `Transport<TcpImpl>` template. Same application code, compile-time backend selection, zero virtual dispatch.

## Performance

Measured on AWS Graviton (ARM64), 64-byte payload:

| Metric | Socket | DPDK |
|--------|--------|------|
| TX latency | ~350 ns | ~164 ns |
| RX latency | ~300 ns | ~139 ns |
| WS decode | 1.8 ns | 1.8 ns |
| ITCH parse | 1.4 ns | 1.4 ns |
| FIX parse (NewOrder) | 222 ns | 222 ns |

## Quick Start

### Requirements

- GCC >= 13 or Clang >= 17 (C++23: `std::expected`, `std::format`)
- [xmake](https://xmake.io) build system
- Optional: DPDK (via vcpkg) for kernel-bypass backend

### Build

```bash
# Clone
git clone https://github.com/sqfzy/ephemeral.git
cd ephemeral

# Build (release mode)
xmake -m release

# Build and run tests
xmake build -g tests
xmake run test_fix
xmake run test_websocket
# ... or run all non-DPDK tests:
for t in test_alignment test_bounded_queue test_bounded_queue_bytes \
         test_cpu test_evicting_queue test_evicting_queue_bytes \
         test_fix test_framer test_hdr_histogram test_http test_itch \
         test_proxy test_record test_recorder test_socket_transport \
         test_system_stats test_tcp_concept test_time test_tls_record \
         test_transport test_transport_types test_version test_websocket \
         test_hugepage; do
  xmake run $t
done

# Build and run benchmarks
xmake build bench_fix_parse
xmake run bench_fix_parse
```

### Minimal Example

Connect to a WebSocket server, send a message, receive the echo:

```cpp
#include "eph/net/socket_transport.hpp"

int main() {
    // Configure
    eph::net::SocketConfig sock_cfg{
        .host = "echo.websocket.org", .port = 443, .tcp_nodelay = true};
    eph::net::TransportConfig transport_cfg{
        .remote_host = "echo.websocket.org",
        .remote_port = 443,
        .use_tls     = true,
    };

    // TCP factory
    auto tcp_factory = [&]()
        -> std::expected<std::unique_ptr<eph::net::SocketTransport>, std::string> {
        auto tcp = std::make_unique<eph::net::SocketTransport>(sock_cfg);
        auto result = tcp->connect(std::chrono::milliseconds{5000});
        if (!result) return std::unexpected(result.error());
        return tcp;
    };

    // Connect (TCP -> TLS -> WebSocket upgrade)
    auto result = eph::net::Transport<eph::net::SocketTransport>::create(
        std::move(tcp_factory), transport_cfg);
    if (!result) return 1;
    auto& tp = **result;

    // Send
    std::string msg = "hello ephemeral";
    tp.send_text(msg.data(), msg.size());

    // Receive
    tp.recv([](const uint8_t* data, size_t len) {
        spdlog::info("Received: {}", std::string_view(
            reinterpret_cast<const char*>(data), len));
    });

    tp.stop();
}
```

See [`examples/`](examples/) for more — from minimal clients to production HFT setups with DPDK.

## Modules

| Module | Description |
|--------|-------------|
| **eph-utils** | TSC timing, CPU topology, HdrHistogram, hugepage allocator |
| **eph-containers** | Lock-free SPSC queues: `BoundedQueue` (backpressure), `EvictingQueue` (drop-old) |
| **eph-net** | WebSocket/TLS transport over POSIX sockets |
| **eph-dpdk** | DPDK kernel-bypass TCP backend (same Transport API) |
| **eph-fix** | FIX 4.4 zero-copy parser, builder, framer |
| **eph-itch** | ITCH 5.0 zero-copy parser, framer |

## Integration

ephemeral is header-only. Add the include paths to your build system:

```lua
-- xmake.lua
add_requires("spdlog", "aws-lc")
target("my_app")
    set_kind("binary")
    add_files("src/*.cpp")
    add_includedirs(
        "path/to/ephemeral/eph-utils/include",
        "path/to/ephemeral/eph-containers/include",
        "path/to/ephemeral/eph-net/include")
    add_packages("spdlog", "aws-lc")
```

Or use xmake's dependency mechanism:

```lua
add_deps("eph-net")  -- pulls in eph-utils, eph-containers transitively
```

## Examples

| Example | Backend | Description |
|---------|---------|-------------|
| `minimal_ws_client` | Socket | Simplest possible WebSocket client |
| `production_client` | Socket | Reconnection, latency histogram, CPU pinning |
| `simple_hft` | Socket | Binance market data with nanosecond timing |
| `simple_hft_dpdk` | DPDK | Same as above, kernel-bypass |
| `dpdk_quickstart` | DPDK | DPDK connection helper with DNS resolution |
| `ws_echo_client` | Socket | Full-featured echo client with CLI |
| `ws_echo_client_dpdk` | DPDK | DPDK variant of echo client |
| `spsc_queue_demo` | — | BoundedQueue + EvictingQueue usage |
| `framer_showcase` | — | WsFramer, RawFramer, LengthPrefixFramer |
| `perf_tuning_basics` | — | TSC calibration, CPU affinity, hugepages |

## Documentation

- [`summary.md`](summary.md) — Architecture overview, module map, data flow
- [`docs/latency-benchmark-fairness.md`](docs/latency-benchmark-fairness.md) — Socket vs DPDK benchmark methodology

## License

See [LICENSE](LICENSE) for details.
