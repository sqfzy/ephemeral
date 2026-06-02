# Production Configuration Guide

Recommended config-struct values for common deployment scenarios. Values are
tuned for AWS Graviton (ARM64); adjust for your hardware.

The post-v3.3 type names are:

- **Kernel backend**: `eph::net::kernel::StreamConfig` /
  `eph::net::kernel::UdpConfig` /
  `eph::net::kernel::PollerConfig`
  (see `eph-net-kernel/include/eph/net/kernel/config.hpp`)
- **DPDK backend**: `eph::dpdk::PlatformConfig` (NIC bring-up, in
  `eph-net-dpdk/include/eph/dpdk/platform.hpp`) +
  `eph::net::dpdk::StreamConfig` (turnkey factory args, in
  `eph-net-dpdk/include/eph/net/dpdk/config.hpp` — backend-shared
  knobs at the top level + a `dpdk.wire` substruct of type
  `eph::dpdk::TcpConfig` for the wire-level fields)

All fallible APIs return `std::expected<T, eph::core::ErrorInfo>`; see
[`troubleshooting.md`](troubleshooting.md) for the error taxonomy.

---

## Profiles

### Low-latency single-symbol (kernel TCP + WS + TLS)

Order entry / FIX gateway / single-symbol market data. Optimised for
minimum per-message latency.

```cpp
namespace en = eph::net::kernel;
namespace ec = eph::codec;

en::StreamConfig cfg{
    .remote = en::SocketAddr::resolve("fix-gateway.exchange.com", 443).value(),

    // Tight handshake budget — local-DC venues complete in <50ms
    .connect_timeout = std::chrono::milliseconds{1000},

    // TCP_NODELAY mandatory for low-latency
    .tcp_nodelay = true,

    // 64KB reassembly is enough for FIX / JSON order acks
    .reasm_capacity = 64 * 1024,

    // TLS 1.3 (only consulted when EnableTls=true template param is set)
    .tls = {
        .hostname     = "fix-gateway.exchange.com",
        .verify_peer  = true,
        .ca_cert_path = "/etc/ssl/certs/ca-bundle.crt",
    },

    // WS upgrade (transparent: handshake happens inside create()).
    // Post-T3.19 sub-struct shape — flat ws_path/ws_host/ws_timeout/
    // ws_permessage_deflate were folded into the WsConfig sub-struct.
    .ws = {
        .path                = "/ws",
        .host                = "fix-gateway.exchange.com",
        .timeout             = std::chrono::milliseconds{1000},
        .permessage_deflate  = false,  // disable for order ack RTT
    },
};

auto stream = en::KernelTcpStream<ec::WsCodec, /*EnableTls=*/true>::create(cfg);
```

Pin the polling thread to an isolated, NUMA-local core via
`pthread_setaffinity_np` before calling `Poller::poll()` in a loop. The
kernel backend has no internal `tx_cpu` / `rx_cpu` knob — pinning is the
caller's responsibility (see CLAUDE rule "Bench 每个线程必须绑独立 CPU").

**Key decisions:**
- `connect_timeout=1000ms` — fail fast; reconnect via `eph::utils::ExponentialBackoff`.
- `ws.permessage_deflate=false` — order acks are tiny; deflate adds
  decode CPU without saving bytes.
- `verify_peer=true` always in production. Only flip to `false` for
  isolated dev/staging.

### High-throughput multi-symbol (kernel TCP + WS + TLS)

Consolidated bookTicker / aggTrade across many symbols, single
connection. Throughput-biased.

```cpp
en::StreamConfig cfg{
    .remote          = en::SocketAddr::resolve("stream.exchange.com", 443).value(),
    .connect_timeout = std::chrono::milliseconds{5000},

    // Larger reassembly — bookTicker bursts can exceed 32KB
    .reasm_capacity  = 256 * 1024,

    .tls = {
        .hostname     = "stream.exchange.com",
        .verify_peer  = true,
    },

    .ws = {
        .path                = "/stream?streams=btcusdt@bookTicker/...",
        .host                = "stream.exchange.com",
        .timeout             = std::chrono::milliseconds{5000},
        // Enable deflate — multi-symbol JSON compresses well
        .permessage_deflate  = true,
    },
};
```

**Key decisions:**
- `ws.permessage_deflate=true` — multi-symbol JSON compresses 3–5×;
  CPU savings from less TLS-decrypt work usually outweigh inflate cost.
- Larger `reasm_capacity` so a slow consumer doesn't stall reassembly
  on a deflate-expanded burst.

### DPDK kernel-bypass (sub-microsecond TCP)

For sub-µs latency on DPDK-capable NICs (Intel X710, AWS ENA, …). Bring-up
is a two-step pattern: build the platform once, then create-and-attach
streams against it.

```cpp
namespace ed = eph::dpdk;

// 1. NIC bring-up (once per process) — handles RSS / mempools / queues.
//    Per-stream IPs/MACs live on the StreamConfig::dpdk.wire
//    sub-struct (4-tuple + MAC), not on PlatformConfig — Platform owns
//    the port, not any L3 identity.
ed::PlatformConfig pcfg{
    .port_id          = 0,
    .nb_rx_queues     = 4,
    .nb_tx_queues     = 4,
    // Mempool sizing: pool size must be 2^k - 1 (eg. 4095, 8191, 16383).
    // validate() enforces is_power_of_two_minus_one(mbuf_pool_size).
    .mbuf_pool_size   = 16383,
    .mbuf_cache_size  = 256,
    .enable_rss       = true,
};
auto plat = ed::Platform::create(pcfg).value();  // Hard-fails on RSS
                                                  // bring-up failure (no
                                                  // silent collapse to q0).

// 2. Per-stream config — eph::net::dpdk::StreamConfig (post-T3.19 shape).
//    Backend-shared knobs (connect_timeout / tls / ws / keepalive) sit at
//    the top level so the surface is symmetric with the kernel twin; DPDK-
//    only wire-level knobs live inside the `dpdk` substruct.
namespace end = eph::net::dpdk;
end::StreamConfig scfg{
    .connect_timeout = std::chrono::milliseconds{500},
    .tls       = { .hostname = "exchange.com", .verify_peer = true },
    // .ws.path / .ws.host / .ws.timeout same as kernel surface
    .keepalive = {
        .interval = std::chrono::seconds{5},   // optional; 0 = disabled
        .probes   = 3,                         // optional
    },
    .dpdk = {
        .wire = {
            // 4-tuple, MAC, MSS, recv_window, port/queue IDs.
            // .mss is left at the default (1460) for Ethernet; negotiated
            // down on Frag-Needed ICMP via TcpSession::on_icmp_frag_needed.
        },
        .pool = mempool_ptr,
        // .pin_to_queue: nullopt = RSS-decides / FlowDirector RR / SW q0.
        // RssPartitioned needs an explicit pin + measured src_port; see
        // docs/cpu-no-cross-core.md.
    },
};

// 3. Turnkey: handles src_port allocation, RSS hash rebinding, TCP / TLS / WS
//    handshakes, Poller attach, FlowDirector rule install, and ICMP
//    registration.
auto stream = ed::DpdkTcpStream<ec::WsCodec, true>::create_and_attach(scfg, *plat);
```

**Key decisions:**
- `connect_timeout=500ms` — DPDK SYN/SYN-ACK is much faster than the
  kernel path; tighter timeout fails over to backup faster.
- `nb_rx_queues>1` requires `enable_rss=true`. The pair `enable_rss=false
  && nb_rx_queues>1` hard-fails with a recovery hint (see CLAUDE.md, RSS
  bring-up section).
- `cfg.keepalive.interval` is optional; the tick fires inside
  `DpdkPoller::poll` via the `on_poll_tick_` hook, so single-stream users
  driving `poll_once_` directly must `tick_keepalive(now_tsc)` themselves.
  The user-facing field is lowered into `dpdk.wire.keepalive_*`
  at factory time — do not set the wire-level fields directly.
- For multi-process (primary + secondary) deployments, see
  `eph-net-dpdk/docs/dpdk-multiprocess.md` — partitioning src_port across
  processes is the **caller's** responsibility.

### Plain UDP (kernel)

Multicast market data feeds (MoldUDP64) or order send-only paths.

```cpp
en::UdpConfig cfg{
    .bind       = en::SocketAddr{en::Ipv4Addr{0,0,0,0}, 13000},
    .reuse_addr = true,                  // mandatory for multicast
    .rcv_buf    = 16 * 1024 * 1024,      // 16 MB — burst absorption
};
auto sock = en::KernelUdpSocket<ec::Mold64Codec>::create(cfg);
```

For DPDK UDP, `eph::net::dpdk::DpdkUdpSocket<Codec>::create_and_attach`
mirrors the TCP pattern — handles queue selection and Poller attach in
one call.

---

## Socket / NIC Buffer Tuning

| Knob                          | Field                                | Recommended           |
|-------------------------------|--------------------------------------|-----------------------|
| TCP_NODELAY                   | `StreamConfig::tcp_nodelay`          | `true` always         |
| Reassembly buffer (kernel)    | `StreamConfig::reasm_capacity`       | 64KB low-lat, 256KB throughput |
| UDP recv buffer (kernel)      | `UdpConfig::rcv_buf`                 | 16 MB for multicast bursts |
| Mempool size (DPDK)           | `PlatformConfig::mbuf_pool_size`     | 2^k - 1 (e.g. 4095, 16383, 65535); ≥ 2 × (rx_q × rx_desc + tx_q × tx_desc) |
| RSS queues (DPDK)             | `PlatformConfig::nb_rx_queues`       | 1 for single-symbol; 4–8 for fan-out |
| TCP MSS (DPDK)                | `StreamConfig::dpdk.wire.mss` | 1460 (Ethernet); negotiated down on Frag-Needed ICMP |
| epoll burst (kernel)          | `PollerConfig::max_events_per_wait`  | 64 (default)          |

The previous `tx_burst_size` / `rx_burst_size` / `tx_cpu` / `rx_cpu`
fields from the retired `TransportConfig` no longer exist — burst sizing
in DPDK is implicit (each `DpdkPoller::poll()` cycle drains up to
`RTE_ETH_RX_BURST_DEF` packets per queue), and CPU pinning is the
caller's responsibility.

---

## Common Pitfalls

| Pitfall | Symptom | Fix |
|---------|---------|-----|
| `verify_peer=false` shipped to prod | MITM exposure | Hard-set `true` in deployment config |
| `enable_rss=false` with `nb_rx_queues>1` | `Platform::create` hard-fails | Either enable RSS or set `nb_rx_queues=1` |
| `proxy.host` set on DPDK backend | `Error::InvalidConfig: proxy on DPDK backend` | Kernel only — drop proxy on DPDK path |
| `ws.path` set without `ws.host` and without TLS | Falls back to numeric `Host:` | Set `ws.host` explicitly |
| `connect_timeout=0` | Stream stalls indefinitely | Always set a positive deadline |
| Polling thread not pinned | p99 spike from cross-core migration | `pthread_setaffinity_np` before poll loop |
| DPDK secondary started before primary | `rte_mempool_lookup` returns nullptr | Order primary-first; see `dpdk-multiprocess.md` |
| Same `src_port` across DPDK MP processes | Connection state collision | Caller partitions src_port range |
| `cfg.keepalive.interval` set but `poll_once_` driven directly | Idle timeouts never fire | Call `tick_keepalive(now_tsc)` per cycle |

---

## Validation Checklist Before Production

1. `xmake -m release` builds clean (no warnings)
2. All tests covering modified config paths pass
3. `verify_peer=true` in deployment config (grep the deploy artifact)
4. CPU pinning verified via `taskset -p <pid>` after start
5. NUMA locality verified: `cat /sys/class/net/<iface>/device/numa_node`
   matches the pinned core's NUMA node
6. For DPDK: `dpdk-devbind.py --status` shows NIC bound to vfio-pci before
   process start
7. Hugepages reserved: `cat /proc/meminfo | grep HugePages_Free` ≥ what
   the process needs
8. For DPDK MP: src_port ranges across the cooperating processes
   are disjoint. `EalConfig` has no `src_port` field — partitioning
   is the **caller's** responsibility (eph-net-dpdk does not
   auto-allocate src_port and has no global view to enforce
   disjointness). See `eph-net-dpdk/docs/dpdk-multiprocess.md`.
9. Idempotent setup script (`eph-net-dpdk/tools/dpdk-setup.sh`) runs
   green on a fresh host

For deeper operational guidance see `docs/operations-runbook.md`,
`eph-net-dpdk/docs/dpdk-setup.md`, and `eph-net-dpdk/docs/dpdk-multiprocess.md`.
