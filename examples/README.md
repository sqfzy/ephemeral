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

DPDK-backed examples (`simple_hft_dpdk`, `binance_latency`) need a system
`libdpdk` (pkg-config) and a NIC bound to `vfio-pci`. See
`docs/dpdk-setup.md` and `eph-net-dpdk/scripts/dpdk-setup.sh`. DPDK
binaries must be run with `sudo`, and EAL args come before a literal `--`
separator.

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
| `binance_book.cpp`        | Kernel / pipeline   | Integration surface sketch: WsCodec → `on_message` → `eph::json::binance::parse_book_ticker` → `eph::book::BinanceBookAdapter`. The body currently just counts frames — the parser/book wiring is left as the exercise. | `eph-net-kernel`, `eph-codec`, `eph-json`, `eph-book` |
| `simple_hft_dpdk.cpp`     | DPDK / skeleton     | Plain-TCP `DpdkTcpStream<C, false>` using the strict `create(cfg)` factory with a hand-built `StreamConfig::legacy`. `scfg.pool=nullptr` forces an early `InvalidConfig` — a smoke-boot template, not a runnable probe. | `eph-net-dpdk`, `eph-codec`                |
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
| `ws_path`                   | non-empty string                          | Send an RFC 6455 client `Upgrade`, validate `Sec-WebSocket-Accept`, then return.         |

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
  (`--host`, `--dst-ip`) require a dotted-quad — DNS is kept out of the
  transport layer. The DPDK `binance_latency` example is the exception:
  it ships a DPDK-native ARP + DNS path and can resolve `--host` itself
  via `eph::dpdk::dns::resolve`.
* **Reconnect loops live in the caller**, not in `Stream::create`. The
  rationale (FIX Logon seq resync, kill-switch gating, multi-path
  failover) is documented at the top of `session_reconnect.cpp` and
  `production_client.cpp`.
* **`g_running` + SIGINT / SIGTERM** is the standard shutdown handle.
  All long-running examples honor Ctrl+C cleanly.
* **spdlog** is used throughout; `spdlog::set_level(level::info)` is the
  default. Examples that want payload dumps use compile-time-gated
  `SPDLOG_DEBUG` so release builds strip the call.
