# eph-net Library Separation Design

## Problem

eph-dpdk bundles protocol-layer code (WebSocket, HTTP, TLS) that has no DPDK dependency alongside DPDK-specific code (EAL, platform, TCP, net headers). This coupling prevents:

1. Testing TLS handshake logic without DPDK hardware (tls_session.hpp BIO calls `rte_eth_rx_burst` directly)
2. Reusing protocol code with other I/O backends (io_uring, kernel sockets)
3. Running protocol-layer tests in standard CI environments

## Solution

Split protocol-layer code into a new **eph-net** library. Define a `TcpTransport` C++20 concept in eph-net. eph-dpdk's `DpdkTcpSession` satisfies the concept. Templates ensure zero runtime overhead via monomorphization.

## Architecture

### Dependency Graph

```
app → eph-dpdk → eph-net → eph-base, eph-utils, eph-containers
```

### File Distribution

**eph-net/include/eph/net/** (new library):
- `tcp_concept.hpp` — NEW: `TcpTransport` concept definition
- `websocket.hpp` — MOVE from eph-dpdk, namespace `eph::net::ws`
- `http.hpp` — MOVE from eph-dpdk, namespace `eph::net::http`
- `tls_session.hpp` — MOVE + TEMPLATE: `TlsSession<TcpImpl>`
- `tls_record.hpp` — MOVE from eph-dpdk, namespace `eph::net`
- `transport.hpp` — MOVE + TEMPLATE: `Transport<TcpImpl, MaxPayload, QueueDepth>`

**eph-dpdk/include/eph/dpdk/** (keeps DPDK-specific code):
- `eal.hpp` — KEEP
- `platform.hpp` — KEEP
- `net_header.hpp` — KEEP
- `tcp.hpp` — KEEP + REFACTOR: add `poll_rx()`, satisfy `TcpTransport`
- `types.hpp` — NEW: `using DpdkTransport = net::Transport<TcpSession>`

### TcpTransport Concept

```cpp
template <typename T>
concept TcpTransport = requires(T& t,
    const void* data, size_t len,
    std::chrono::milliseconds timeout) {
    { t.connect(timeout) } -> std::same_as<std::expected<void, std::string>>;
    { t.send(data, len) } -> std::same_as<std::expected<size_t, std::string>>;
    { t.close() } -> std::same_as<std::expected<void, std::string>>;
    { t.reset() } -> std::same_as<void>;
    { t.mss() } -> std::convertible_to<uint16_t>;
    { t.is_established() } -> std::same_as<bool>;
    // poll_rx constrained via requires expression on callback
};
```

### RX Path Refactoring

Current: `transport.hpp::rx_loop` calls `rte_eth_rx_burst` directly, passes `rte_mbuf**` to `tcp_->process_rx()`.

After: `DpdkTcpSession::poll_rx(callback)` encapsulates `rte_eth_rx_burst + process_rx`. Transport calls `tcp_->poll_rx(callback)`. Template inlining produces identical machine code.

### Build System

```lua
target("eph-net")
    set_kind("headeronly")
    add_includedirs("eph-net/include", { public = true })
    add_deps("eph-base", "eph-utils", "eph-containers", { public = true })
    add_packages("spdlog", "aws-lc", { public = true })

target("eph-dpdk")
    set_kind("headeronly")
    add_deps("eph-net", { public = true })
    add_packages("dpdk", { public = true })
```

## Testing Impact

- `test_websocket.cpp`, `test_http.cpp`, `test_tls_record.cpp` → depend on eph-net (no DPDK needed)
- `test_net_header.cpp`, `test_dpdk_platform.cpp` → keep depending on eph-dpdk
- `bench_ws_pipeline.cpp` → update includes and namespaces

## Verification

1. `xmake build` compiles successfully
2. All existing tests pass
3. Benchmarks show no performance regression (compare with `objdump -d` if needed)

## Zero-Cost Guarantee

Header-only + C++20 concepts + templates + `-O2` = compiler monomorphizes all generic code. `Transport<DpdkTcpSession>` produces the same machine code as the current non-templated `Transport`. No virtual functions, no indirect calls.
