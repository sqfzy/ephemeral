# Production Configuration Guide

Recommended `TransportConfig` settings for common deployment scenarios. All values are tuned for AWS Graviton (ARM64) — adjust for your hardware.

> **Pre-v3.3 archive notice** — the snippets below use the retired
> `eph::net::TransportConfig` struct. The post-v3.3 surface is
> `eph::net::kernel::StreamConfig` (kernel backend) and
> `eph::dpdk::Config` + per-stream config (DPDK backend); see each
> module's `README.md` and `summary.md` for the current field set.
> Field names have largely been preserved (`remote`, `tls.hostname`,
> `tls.verify_peer`, `tcp_nodelay`, `connect_timeout`, `ws_path`,
> `ws_host`, `ws_timeout`, `ws_permessage_deflate`, …) so the
> recommended **values** below are still accurate for the new struct
> shape — just substitute the type name. The numerical guidance
> (timeouts, queue sizes, MTU, CPU pinning targets) reflects the
> current production profile and remains the source of truth. A
> focused rewrite tracking the new struct names is a future followup.

## Profiles

### Low-Latency (Single Symbol, Order Execution)

Optimized for minimum per-message latency. Typical use: FIX order gateway, single-symbol market data.

```cpp
eph::net::TransportConfig cfg{
    .remote_host = "fix-gateway.exchange.com",
    .remote_port = 443,
    .ws_path     = "/ws",

    // TLS: verify in production, skip UTF-8 for speed
    .use_tls     = true,
    .verify_peer = true,
    .skip_utf8_validation = true,

    // Tight timeouts — fail fast on stale connections
    .tcp_timeout = std::chrono::milliseconds{1000},
    .tls_timeout = std::chrono::milliseconds{2000},
    .ws_timeout  = std::chrono::milliseconds{1000},

    // Small burst size reduces per-batch latency at cost of throughput
    .tx_burst_size = 8,
    .rx_burst_size = 8,

    // Fast reconnect with limited retries
    .reconnect_interval    = std::chrono::milliseconds{50},
    .max_reconnect_backoff = std::chrono::milliseconds{500},
    .max_reconnect_attempts = 5,

    // Aggressive keepalive — detect dead connections quickly
    .ping_interval = std::chrono::seconds{5},
    .pong_timeout  = std::chrono::seconds{3},

    // Pin TX/RX to isolated cores (use perf_tuning_basics to find good cores)
    .tx_cpu = 2,
    .rx_cpu = 3,
};
```

**Key decisions:**
- `tx/rx_burst_size = 8`: Smaller batches mean the TX thread wakes up more often but each wake processes fewer messages, reducing tail latency.
- `pong_timeout = 3s`: Detects dead connections within 8s (5s ping interval + 3s wait). Without this, a silently dead connection wastes minutes.
- `skip_utf8_validation = true`: Saves ~50ns per text frame. Only safe if you control the payload format.

### High-Throughput (Multi-Symbol, Market Data)

Optimized for maximum messages/second. Typical use: consolidated ticker feed, 50+ symbols on one connection.

```cpp
eph::net::TransportConfig cfg{
    .remote_host = "stream.exchange.com",
    .remote_port = 443,
    .ws_path     = "/stream",

    .use_tls     = true,
    .verify_peer = true,
    .skip_utf8_validation = true,

    // Relaxed timeouts — server may be slow during market open
    .tcp_timeout = std::chrono::milliseconds{5000},
    .tls_timeout = std::chrono::milliseconds{10000},
    .ws_timeout  = std::chrono::milliseconds{5000},

    // Large burst size maximizes throughput per wake cycle
    .tx_burst_size = 64,
    .rx_burst_size = 64,

    // Patient reconnect — don't hammer the server
    .reconnect_interval    = std::chrono::milliseconds{500},
    .max_reconnect_backoff = std::chrono::milliseconds{5000},
    .max_reconnect_attempts = 20,

    // Moderate keepalive
    .ping_interval = std::chrono::seconds{30},
    .pong_timeout  = std::chrono::seconds{10},

    // Pin to cores, but RX is more critical than TX for market data
    .tx_cpu = 4,
    .rx_cpu = 5,

    // Throttle drop logging — at high throughput, per-drop logs would flood
    .drop_log_interval = 10000,
};

// Use frame filter for latest-per-symbol delivery (drops stale updates)
cfg.on_frame_filter = eph::net::make_twophase_filter(my_symbol_hash);
```

**Key decisions:**
- `tx/rx_burst_size = 64`: Amortizes syscall/atomic overhead across more messages per batch.
- `drop_log_interval = 10000`: At 100K msg/s, logging every drop would generate 1000+ log lines/sec if the consumer can't keep up.
- `on_frame_filter`: Essential for multi-symbol feeds — delivers only the latest update per symbol, discarding stale intermediate frames.

### DPDK Kernel-Bypass

For sub-microsecond latency. Requires DPDK-capable NIC (Intel X710, AWS ENA, etc.).

```cpp
// DPDK endpoint configuration
eph::dpdk::DpdkEndpoint ep{
    .local_ip  = "10.0.0.2",
    .gateway   = "10.0.0.1",
    .port_id   = 0,
};

// Transport config (same structure, DPDK-specific tuning)
eph::net::TransportConfig cfg{
    .remote_host = "exchange.com",
    .remote_port = 443,

    .use_tls     = true,
    .verify_peer = true,
    .skip_utf8_validation = true,

    .tcp_timeout = std::chrono::milliseconds{500},
    .tls_timeout = std::chrono::milliseconds{1000},
    .ws_timeout  = std::chrono::milliseconds{500},

    .tx_burst_size = 32,
    .rx_burst_size = 32,

    .reconnect_interval     = std::chrono::milliseconds{10},
    .max_reconnect_backoff  = std::chrono::milliseconds{100},
    .max_reconnect_attempts = 3,  // DPDK reconnect is fast, fewer retries needed

    .ping_interval = std::chrono::seconds{5},
    .pong_timeout  = std::chrono::seconds{2},

    // DPDK: pin to NUMA-local cores adjacent to the NIC
    .tx_cpu = 2,
    .rx_cpu = 3,
};

auto result = eph::dpdk::connect(ep, cfg);
```

**Key decisions:**
- Tighter timeouts across the board — DPDK connections establish faster.
- Fewer reconnect attempts — if the path is broken, retry won't help; escalate to monitoring.
- NUMA-local CPU pinning is critical — cross-NUMA memory access adds ~100ns per cache miss.

## Socket Tuning (SocketConfig)

```cpp
eph::net::SocketConfig sock_cfg{
    .host = "exchange.com",
    .port = 443,

    // TCP_NODELAY: disable Nagle's algorithm (mandatory for low-latency)
    .tcp_nodelay = true,

    // Buffer sizes: larger = more kernel buffering = higher throughput
    // but also higher memory and potentially higher latency
    .send_buf_size = 65536,   // 64KB (default is OS-dependent, often 128KB)
    .recv_buf_size = 262144,  // 256KB for market data bursts
};
```

## Common Pitfalls

| Pitfall | Symptom | Fix |
|---------|---------|-----|
| `pong_timeout = 0` (default) | Dead connections persist for minutes | Set `pong_timeout` to 2-3x ping_interval |
| `skip_utf8_validation = false` with binary data | ~50ns overhead per message for no benefit | Set `true` if payload is binary/JSON |
| `tx_cpu = rx_cpu` (same core) | TX and RX threads contend for L1 cache | Use adjacent cores on same NUMA node |
| `drop_log_interval = 0` | No visibility into queue drops | Set to 1000-10000 for production |
| Missing `on_state_change` callback | Silent disconnects | Always register for monitoring |
| `verify_peer = false` in production | MITM vulnerability | Only disable for local testing |
