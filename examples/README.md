# `eph` examples

End-to-end demos wiring the full `eph` stack — both networking backends
(`eph-net-kernel`, `eph-net-dpdk`), codecs (`eph-codec`), parsers (`eph-json`,
`eph-book`), observability (`eph-core` `MetricsSink` + `eph-utils`
`ConsoleSink`) and reconnect policy (`eph-net`).

Every example follows the same shape:

```
poller = Poller::create(...)
stream = TcpStream<Codec, Tls>::create(cfg)   (or DpdkTcpStream, or UdpSocket)
stream->on_message = [](span<const uint8_t>) { ... }
poller->add(stream.get())
while (running) poller->poll(...);
```

See `docs/poller-guide.md`, `docs/architecture.md` and `docs/custom-codec.md`
in the repository root for the concept model these examples rely on.

---

## Building and running

Examples are declared at the bottom of the root `xmake.lua` with
`set_default(false)` — they are **not** built by `xmake` alone. Build one
explicitly:

```bash
xmake build <target>            # e.g. xmake build minimal_ws_client
xmake build -g examples         # or: build all example targets at once
xmake run <target> [args...]    # runs build/<plat>/<arch>/<mode>/<target>
```

DPDK-backed examples (`simple_hft_dpdk`, `simple_hft_dpdk_mp`,
`simple_hft_dpdk_mp_dynamic`, `simple_hft_dpdk_rss`,
`multi_port_platform_demo`, `dpdk_multicast_md`,
`async_dns_multi_resolve`, `binance_latency`) need a system `libdpdk`
(pkg-config) and a NIC bound to `vfio-pci`. See `docs/dpdk-setup.md`
and `eph-net-dpdk/scripts/dpdk-setup.sh`. DPDK binaries must be run
with `sudo`, and EAL args come before a literal `--` separator.

Two examples cover DPDK multi-process. `simple_hft_dpdk_mp` is the
declarative path: same binary in two terminals with
`--role primary|secondary`, both passing the same `--file-prefix`.
`simple_hft_dpdk_mp_dynamic` is the autojoin path: same binary in
two terminals with no role / file-prefix flag — `Platform::join_dynamic`
detects who's first via `eal_init` and CAS-claims slot 0/1. See each
file header for the launch commands.

`simple_hft_dpdk_rss` is the single-process counterpart: one Platform
with `enable_rss=true` and `nb_rx_queues > 1`, one Poller per queue,
several `DpdkUdpSocket`s spawned via `create_and_attach` with
`pin_to_queue` set so each lands on a distinct RX queue via Toeplitz
reverse-pick. Pairs with `eph-net-dpdk/docs/rss-control-plane.md`.

---

## Index

| File                      | Category            | Shows                                                                                           | Modules linked                             |
|---------------------------|---------------------|-------------------------------------------------------------------------------------------------|--------------------------------------------|
| `minimal_ws_client.cpp`   | Kernel / WS         | Shortest possible `KernelTcpStream<WsCodec, false>` skeleton; `WsCodec` framing, no TLS, no WS Upgrade (see note below). | `eph-net-kernel`, `eph-codec`              |
| `ws_echo_client.cpp`      | Kernel / raw TCP    | TX+RX echo loop against a netcat-style echoer using `RawStreamCodec`.                           | `eph-net-kernel`, `eph-codec`              |
| `framer_showcase.cpp`     | Codec only          | In-process `encode` / `decode` round-trip for `RawStreamCodec`, `LengthPrefixCodec`, `WsCodec`. No sockets, no DPDK. | `eph-codec`                                |
| `session_reconnect.cpp`   | Kernel / reconnect  | Canonical outer-loop reconnect pattern using `eph::net::ReconnectPolicy` (exponential back-off with jitter). | `eph-net-kernel`, `eph-codec`              |
| `ws_via_proxy.cpp`        | Kernel / proxy      | HTTP CONNECT proxy via `StreamConfig::proxy` (`eph::net::ProxyConfig`). Kernel-only — DPDK rejects proxies by design. | `eph-net-kernel`, `eph-codec`              |
| `production_client.cpp`   | Kernel / production | Production-grade knobs: TLS, `TCP_NODELAY`, bounded reasm, signal shutdown, outer reconnect, periodic `publish_metrics`. | `eph-net-kernel`, `eph-codec`              |
| `observability_demo.cpp`  | Kernel / metrics    | `publish_metrics()` periodically from a `KernelTcpStream` into a `ConsoleSink`; loopback echo server included. | `eph-net-kernel`, `eph-codec`, `eph-utils` |
| `simple_hft.cpp`          | Kernel / template   | `KernelTcpStream<WsCodec, true>` with argparse; end-of-run metrics snapshot via `stream->metric(...)`. | `eph-net-kernel`, `eph-codec`              |
| `reconnect_orch_demo.cpp` | Kernel / orchestrator | `eph::net::ReconnectOrchestrator<KernelTcpStream<RawStreamCodec>>` against an in-process loopback echo server that drops the first client. Shows the factory + attach/detach + `tick()` shape and the `reconnect_count` ≥ 2 termination signal. No external deps. | `eph-net-kernel`, `eph-codec`              |
| `binance_book.cpp`        | Kernel / pipeline   | Integration surface sketch: WsCodec → `on_message` → `eph::json::binance::parse_book_ticker` → `eph::book::BinanceBookAdapter`. The body currently just counts frames — the parser/book wiring is left as the exercise. | `eph-net-kernel`, `eph-codec`, `eph-json`, `eph-book` |
| `coinbase_jwt_rest.cpp`   | Kernel / REST auth  | `Es256PrivateKey::from_pem` + `build_coinbase_jwt` (ES256 + JOSE P-1363 conversion) → `HttpClient<KernelTcpStream<RawStreamCodec, true>>` GET with `Authorization: Bearer <jwt>`. Default mode signs with a throwaway PEM and exits without network I/O; `--live` issues the real REST call. Also demonstrates HTTP/1.1 keep-alive on the same socket. | `eph-net-kernel`, `eph-codec`              |
| `binance_signed_rest.cpp` | Kernel / REST auth  | `HmacSha256Key` + `SignedRequest<BinanceSignTraits>::headers_for_query` → `HttpClient` GET with `X-MBX-SIGNATURE` / `X-MBX-TIMESTAMP` spliced in. Default mode pins ts and prints the signed query + headers; `--live` against testnet `api.binance.com` issues `/api/v3/account`. | `eph-net-kernel`, `eph-codec`              |
| `ws_deflate_demo.cpp`     | Kernel / WS deflate | RFC 7692 permessage-deflate over `KernelTcpStream<WsCodec, true>`. Reads `kWsDeflateBytesIn` / `kWsDeflateBytesOut` directly via `stream->metric(...)` AND publishes the `eph::net::publish_ws_deflate_ratio` derived gauge into a `ConsoleSink` once a second. `--no-deflate` opts out for venues that mis-implement the extension. | `eph-net-kernel`, `eph-codec`, `eph-utils` |
| `multi_port_platform_demo.cpp` | DPDK / aggregator | `eph::dpdk::MultiPortPlatform` over ≥ 2 `--pci` ports — atomic N-port bringup with rollback, indexed `port(i)` access, `find_index_by_port_id` lookup, one Poller per port. The aggregator is intentionally thin: it adds no fused state across ports (no merged ICMP registry, no fused Poller). See `eph/dpdk/multi_port_platform.hpp` for the rationale. | `eph-net-dpdk`                             |
| `dpdk_multicast_md.cpp`   | DPDK / multicast    | `eph::dpdk::MulticastReceiver` — RFC 1112 multicast MAC filter, `join_group` / `on_packet` / `start` / `stop` shape. Demonstrates `total_rx_packets` / `rx_unmatched_packets` diagnostics. `--rss-fail-test` exercises the RSS multi-queue safety gate (start() refuses with `rss_active_multi_queue=true` because the receiver cannot reverse-pick the sender's 5-tuple). | `eph-net-dpdk`                             |
| `simple_hft_dpdk.cpp`     | DPDK / skeleton     | Plain-TCP `DpdkTcpStream<C, false>` using the strict `create(cfg)` factory with a hand-built `StreamConfig::dpdk.tcp_low_level`. `scfg.dpdk.pool=nullptr` forces an early `InvalidConfig` — a smoke-boot template, not a runnable probe. | `eph-net-dpdk`, `eph-codec`                |
| `simple_hft_dpdk_mp.cpp`  | DPDK / multi-process | Single-NIC primary+secondary skeleton. One binary, role picked via `--role primary\|secondary`. Brings up `EalConfig` + `Platform::create_primary` / `create_secondary`, attaches a `DpdkUdpSocket<RawDatagramCodec>` from each process' owned RX queue range, drives `poll()` for 5 s. Demonstrates the create-vs-lookup mempool split + the secondary cleanup branch. See `eph-net-dpdk/docs/dpdk-multiprocess.md` for partitioning rules. | `eph-net-dpdk`, `eph-codec`            |
| `simple_hft_dpdk_mp_dynamic.cpp` | DPDK / mp autojoin | Same binary twice in two terminals; `Platform::join_dynamic` figures out primary-vs-secondary by who calls `eal_init` first and CAS-claims the lowest free slot. Zero coordination — no `--role`, no shared `--file-prefix` (auto-derived from `--pci`), no `--self-index`. Pairs with the lower-level declarative path in `simple_hft_dpdk_mp.cpp`. | `eph-net-dpdk`, `eph-codec`            |
| `simple_hft_dpdk_rss.cpp` | DPDK / RSS          | Single-process RSS multi-queue: `Platform::create_primary` with `enable_rss=true` + `nb_rx_queues=N`, prints the three diagnostic getters (`dispatch_mode` / `rss_using_probed_key` / `effective_rx_queue_range`), spawns one `DpdkPoller` per owned queue, then attaches several `DpdkUdpSocket`s via `create_and_attach` with `pin_to_queue` set per-connection — the helper reverse-picks an ephemeral src_port whose Toeplitz hash lands on the requested queue. Pairs with `eph-net-dpdk/docs/rss-control-plane.md`. | `eph-net-dpdk`, `eph-codec`            |
| `async_dns_multi_resolve.cpp` | DPDK / DNS      | Parallel DNS resolution via `eph::dpdk::dns::AsyncDnsResolver` driven from one `DpdkPoller<>` burst loop. Constructs one resolver per hostname, `start()` + `poller->add()` each, drives `poll()` until all reach a terminal state. Smoke-boot when mempool / gateway-MAC are absent. | `eph-net-dpdk`                             |
| `binance_latency.cpp`     | DPDK / production   | Full-stack DPDK probe to Binance: `Platform` bring-up, ARP + DPDK-native DNS, `DpdkTcpStream<WsCodec, true>` (TLS 1.3 + WS Upgrade), single-lcore burst loop, reconnect policy, `CLOCK_REALTIME` latency histogram via `eph::utils::Recorder`. | `eph-net-dpdk`, `eph-codec`, `eph-json`    |

---

## Transparent handshakes inside `create()`

The kernel and DPDK stream factories fold *three* optional handshakes into
a single `Stream::create(cfg)` call; which ones run is keyed entirely off
fields on `StreamConfig` (there is no separate helper you have to call):

| `StreamConfig` field        | When set                                  | Effect inside `create()`                                                                 |
|-----------------------------|-------------------------------------------|------------------------------------------------------------------------------------------|
| `proxy` (kernel only)       | `std::optional<ProxyConfig>` is populated | TCP-connect to the proxy, drive the HTTP CONNECT exchange, continue with the tunneled fd.|
| `EnableTls` template param  | `/*EnableTls=*/true`                      | Run the TLS 1.3 client handshake via aws-lc (SNI from `cfg.tls.hostname`).               |
| `ws.path`                   | non-empty string                          | Send an RFC 6455 client `Upgrade`, validate `Sec-WebSocket-Accept`, then return.         |

Most examples here set **none** of these (`minimal_ws_client.cpp`,
`ws_echo_client.cpp`, …) so `create()` does only a raw TCP connect.
`binance_latency.cpp` sets all three (on the DPDK backend) and is the
closest thing in this tree to a production client.

---

## Metrics and observability

Streams carry an `alignas(64) std::atomic<uint64_t>` counter array on the
hot path. Read directly at exit:

```cpp
stream->metric(eph::net::StreamMetric::kBytesSent);
stream->metric(eph::net::StreamMetric::kFramesDecoded);
```

Or push every counter into any `eph::core::MetricsSink` (`NullSink`,
`eph::utils::ConsoleSink`, or your own Prometheus / OTel adapter):

```cpp
eph::net::publish_metrics(*stream, sink, tags);
```

`observability_demo.cpp` wires the whole loop against a loopback echo
server so you can see the counters tick in real time; `simple_hft.cpp`
shows the direct-read pattern at shutdown; `production_client.cpp` shows
the periodic `publish_metrics` pattern with a swappable sink.

---

## Conventions across the examples

* **Plain IPv4 literals only.** Examples that take a host-like flag
  (`--host`, `--target-ip`) require a dotted-quad — DNS is kept out of
  the transport layer. **Exception**: `simple_hft_dpdk_rss.cpp`'s
  `--src-ip` / `--dst-ip` take a packed-uint32 **hex literal** (e.g.
  `0x0A000010` for 10.0.0.16) because they map directly onto the
  DPDK low-level `tuple` field; `simple_hft_dpdk.cpp` keeps the same
  literals hardcoded. The DPDK `binance_latency` example is a second
  exception going the other way: it ships a DPDK-native ARP + DNS path
  and can resolve `--host` itself via `eph::dpdk::dns::resolve`.
* **Reconnect loops live in the caller**, not in `Stream::create`. The
  rationale (FIX Logon seq resync, kill-switch gating, multi-path
  failover) is documented at the top of `session_reconnect.cpp` and
  `production_client.cpp`.
* **`g_running` + SIGINT / SIGTERM** is the standard shutdown handle.
  All long-running examples honor Ctrl+C cleanly.
* **spdlog** is used throughout; `spdlog::set_level(level::info)` is the
  default. Examples that want payload dumps use compile-time-gated
  `SPDLOG_DEBUG` so release builds strip the call.
