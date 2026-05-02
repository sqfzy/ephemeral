# Troubleshooting Guide

Error diagnosis reference for ephemeral. Maps the post-v3.3 `eph::core::Error`
codes (as returned by `std::expected<T, ErrorInfo>` from every fallible API)
to causes and fixes.

The canonical error type is defined in
[`eph-core/include/eph/core/error.hpp`](../eph-core/include/eph/core/error.hpp):

```cpp
struct ErrorInfo {
    eph::core::Error code;     // typed category (programmatic match)
    const char*      detail;   // static-lifetime string (logging only)
};
```

`ErrorInfo` is allocation-free: `detail` is always a string literal, never an
owned `std::string`. Match on `.code` for control flow; log `.detail` for
human diagnosis. Stream insertion (`std::cout << err`), `std::format("{}", err)`
and `SPDLOG_LOGGER_ERROR(...)` all render as `CODE: detail`.

This guide covers the four backends that surface errors in production:

- `eph::net::kernel::KernelTcpStream<Codec, EnableTls>` /
  `KernelUdpSocket<Codec>` (epoll backend)
- `eph::net::dpdk::DpdkTcpStream<Codec, EnableTls>` /
  `DpdkUdpSocket<Codec>` (DPDK kernel-bypass backend)

`KernelPoller` / `DpdkPoller` only ever return `Error::NotAttached` or
`Error::InvalidConfig` from public APIs; runtime poll loops route per-stream
errors back through the stream's own state.

---

## Connection Lifecycle Errors

Returned by `KernelTcpStream::create()`, `DpdkTcpStream::create_and_attach()`,
and reconnection paths driven by `eph::net::ReconnectPolicy`.

### `Error::InvalidConfig`

**Cause**: `eph::net::kernel::StreamConfig` (kernel) or
`eph::net::dpdk::StreamConfig` (DPDK — including its
`dpdk.wire` wire-level `eph::dpdk::TcpConfig` substruct) failed
validation before any I/O.

| `detail` substring | Fix |
|--------------------|-----|
| `remote address empty` | Set `cfg.remote` to a valid `SocketAddr` |
| `ws.path set but ws.host empty` | Set `cfg.ws.host` for the RFC 6455 `Host:` header |
| `proxy.host empty` | Either clear `cfg.proxy` or fill `host` + `port` |
| `tls.hostname empty with verify_peer=true` | Set `cfg.tls.hostname` for SNI / cert verify |
| `client_cert without client_key` | Set both fields or neither (mTLS) |
| `proxy on DPDK backend` | Kernel only — DPDK rejects HTTP CONNECT |
| `enable_rss=false with nb_rx_queues>1` | Either enable RSS or set `nb_rx_queues=1` |

The DPDK platform also hard-fails (no silent collapse to queue 0) when RSS
bring-up fails on every path; see `eph::dpdk::Platform::create` and the
`rss_using_probed_key()` diagnostic getter.

### `Error::ConnectFailed`

**Cause**: TCP three-way handshake (kernel `connect(2)` or DPDK SYN/SYN-ACK)
did not complete.

| `detail` pattern | Likely cause | Fix |
|------------------|--------------|-----|
| `connection refused` | No listener on `host:port` | Verify endpoint, check firewall |
| `connect timeout` | Network unreachable / blackholed | Check VPC SG, route table, NAT |
| `name resolution failed` | DNS lookup failed (kernel only) | Verify DNS config, prefer IP literals |
| `network unreachable` | No route to host | Check default route, NIC up |
| `ARP failed` | DPDK only — gateway MAC not resolvable | Check `gateway_ip`, ARP request reaching gateway |

DPDK's connect is async by construction (`DpdkTcpStream::create_and_attach`
runs the handshake under the poll loop); kernel sets `O_NONBLOCK` and reports
the error from the first poll cycle's writability check.

### `Error::Disconnected`

**Cause**: Peer closed the connection (FIN, RST, or kernel-detected dead
socket).

**Diagnosis**:
- Check the cadence at which `ReconnectPolicy` is consuming attempts —
  is it periodic? See "Connection drops every N minutes" below. The
  policy's metrics live on `ReconnectMetric` (counters
  `kReconnectCount` for successful reconnects, `kReconnectFailures`
  for factory failures, `kReconnectDurationNs` for cumulative
  reconnect duration in nanoseconds) wired through
  `publish_reconnect_metrics`.
- DPDK: `TcpSession::Stats::resets_received` distinguishes RST events
  from graceful FIN closes (FIN closes leave `resets_received == 0`
  and are surfaced through the normal Closed state transition).

### `Error::Timeout`

**Cause**: Operation deadline exceeded. Used by handshake (TLS, WS) and
explicit `recv_for(timeout)` waits.

**Fix**: Increase the matching `*_timeout` in `StreamConfig`
(`connect_timeout` for TCP / handshake budget, `tls.handshake_timeout`
for the TLS 1.3 round trip, `ws.timeout` for the RFC 6455 client
upgrade) — defaults are tight (1-2 s) by design, since HFT venues
are local-DC. (Field paths post-T3.19; the flat `tls_timeout` /
`ws_timeout` fields were folded into the `tls` / `ws` substructs.)

### `Error::NotAttached`

**Cause**: `Stream::send` / `recv` called before `Poller::add(stream)`.

**Fix**: Always attach the stream to a poller before any I/O. The `create_and_attach`
factory on DPDK does this in one step; on kernel use the standard
`auto p = ...; auto s = KernelTcpStream::create(cfg).value(); p->add(s.get());`
pattern.

---

## TLS Errors

### `Error::TlsHandshakeFailed`

**Cause**: aws-lc handshake returned an error after TCP established.

| `detail` pattern | Likely cause | Fix |
|------------------|--------------|-----|
| `certificate verify failed` | Server cert not trusted | Set `cfg.tls.ca_cert_path`, check expiry |
| `alert handshake_failure` | Cipher / version mismatch | ephemeral requires TLS 1.3 |
| `handshake timeout` | Server slow to respond | Increase `cfg.tls.handshake_timeout` |
| `self-signed certificate` | Dev / staging | Set `cfg.tls.verify_peer=false` (dev only) |

**Diagnosis steps**:

```bash
# Probe TLS 1.3 directly
openssl s_client -connect host:port -tls1_3 -servername host

# Check cert expiry
openssl s_client -connect host:port 2>/dev/null \
    | openssl x509 -noout -dates

# Sanity-check system clock — TLS is time-sensitive
date -u
```

### `Error::TlsRecordBad`

**Cause**: Malformed TLS record received (bad version, length, or content
type). Often indicates protocol-layer corruption upstream of TLS — e.g.
HTTP CONNECT proxy that forgot to switch to tunnel mode.

**Fix**: Capture with `tcpdump -i <iface> -w trace.pcap host <peer>` and
inspect with Wireshark; the first non-TLS-shaped record will be obvious.

### `Error::TlsCipherFailed`

**Cause**: AEAD encrypt or decrypt rejected an authentication tag. After a
clean handshake this almost always indicates IV / sequence-number desync —
typically a middlebox rewriting bytes.

**Fix**: Bypass middleboxes; if unavoidable, log the sequence numbers via
`SPDLOG_LEVEL_TRACE` to confirm the desync direction.

---

## WebSocket Errors

### `Error::WsHandshakeFailed`

**Cause**: HTTP/1.1 upgrade request sent but the response was rejected,
malformed, or non-101.

The `detail` string is intentionally short (`ws_handshake: 429 status`,
`ws_handshake: missing Sec-WebSocket-Accept`, `ws_handshake: bad Upgrade
header`, etc.). For programmatic match, key on the substring:

```cpp
namespace en = eph::net::kernel;
namespace ec = eph::codec;

auto result = en::KernelTcpStream<ec::WsCodec>::create(cfg);
if (!result && result.error().code == eph::core::Error::WsHandshakeFailed) {
    // result.error().detail holds a string literal describing the wire-side
    // reason (e.g. "ws_handshake: 429 status"). Match on the substring or,
    // for stricter routing, parse the HTTP status line out of your reconnect
    // logger before reaching this branch — ErrorInfo intentionally carries no
    // integer http_status field (kept allocation-free, see eph/core/error.hpp).
    SPDLOG_ERROR("WS upgrade rejected: {}", result.error().detail);
}
```

| HTTP status (in `detail`) | Meaning | Fix |
|---------------------------|---------|-----|
| `400` | Malformed request | Check `cfg.ws.path`, `cfg.ws.extra_headers` |
| `401` / `403` | Auth required / forbidden | Add auth token to `cfg.ws.extra_headers` |
| `404` | Wrong path | Fix `cfg.ws.path` |
| `426` | Upgrade required (rare) | Server expects different protocol; check vendor docs |
| `429` | Rate limited | Backoff via `ReconnectPolicy`, reduce conn frequency |
| `503` | Server overloaded | Retry later |

### `Error::WsFrameBad`

**Cause**: Frame violates RFC 6455 — bad opcode, RSV bits set, fragmented
control frame, payload exceeds `WsCodec` max, etc.

**Fix**: A vendor sending non-conformant frames is a server bug — capture
the frame with `tcpdump` and report. ephemeral does not silently accept
malformed frames.

### `Error::WsCloseReceived`

**Cause**: Peer sent a Close frame. Not a fatal error per se — the codec
auto-acks the close and the stream transitions to closed.

**Fix**: Inspect close code in payload (RFC 6455 §7.4). Treat `1000` as
clean shutdown; `1011` / `1013` indicate server-side issues; `1008` /
`1009` indicate ephemeral sent something the server rejected.

---

## Codec / Application Protocol

### `Error::CodecNeedMoreData`

**Internal signal**, not surfaced to user code in the post-v3.3 API.
Streams loop on this until enough bytes arrive. If you see it leak, it is
a bug in a custom codec.

### `Error::CodecBad`

**Cause**: Application protocol violation — invalid FIX checksum, invalid
ITCH message type, JSON parse error mid-stream.

**Fix**: Match on the codec's domain-specific enum (`FrameError`, `FixError`,
`ItchError`) for fine-grained branching; the unified `Error::CodecBad` is
returned only when the codec routes through the generic `Stream::recv` path.

### `Error::CodecOverflow`

**Cause**: A decoded frame exceeded the codec's `MaxPayload` template
parameter.

**Fix**: Increase `MaxPayload`, or move to a streaming consumer pattern if
the protocol genuinely supports messages larger than expected.

---

## Send-side / Buffer Errors

### `Error::WouldBlock`

**Cause**: Non-blocking send would block (kernel `EAGAIN`), or DPDK
TX-burst returned 0.

**Fix**: Either retry on next poll cycle, or use the codec's framing buffer
(every codec exposes the `OutputBuffer&` injection path) to queue rather
than synchronously send.

### `Error::BufferFull`

**Cause**: TX queue saturated.

**Fix**:
- Drain via the poller faster — check `poll(timeout)` is being called
- For DPDK, check `tx_queue_id` is on a CPU not contending with other lcores
- Check application backpressure (downstream consumer not keeping up)

### `Error::NoData`

**Cause**: `recv()` returned zero packets. Not an error in poll-driven code
— it means "nothing this cycle." Tests assert on it; production code
treats it as a fast-path no-op.

---

## HTTP CONNECT Proxy Errors

Kernel backend only — `eph::net::dpdk::StreamConfig` has no
`proxy` field at all (removed post-T3.19), so attempting to set
`cfg.proxy.*` on the DPDK config is a compile error rather than a
runtime `InvalidConfig`.

| Error | Cause | Fix |
|-------|-------|-----|
| `ProxyConnectFailed` | TCP connect to proxy itself failed | Verify `cfg.proxy.host` / `port`, firewall |
| `ProxyHandshakeFailed` | Proxy returned non-200 / malformed | Check proxy logs, verify target host allowed |
| `ProxyAuthRequired` | Proxy returned 407 | Set `cfg.proxy.username` / `password` (Basic auth) |

---

## Registry / Lookup Errors

### `Error::NotFound`

**Cause**: Lookup miss — typically `Platform::register_icmp_target` followed
by an unregister of an already-removed handle. Distinct from `InvalidConfig`
(programmer error) — `NotFound` is recoverable state mismatch.

**Fix**: Callers may ignore `NotFound` from unregister paths; log at DEBUG
not WARN.

---

## Runtime Issues

### Connection drops every N minutes

**Symptom**: Reconnect callback fires periodically with `Error::Disconnected`.

**Diagnosis**:
1. DPDK: check `TcpSession::Stats::keepalive_probes_sent` and
   `keepalive_send_failures` — if probes are firing, the peer is not
   answering them, indicating the connection is half-open. Tune
   `cfg.keepalive.interval` / `cfg.keepalive.probes` on the user-facing
   `eph::net::dpdk::StreamConfig` (lowered into
   `dpdk.wire.keepalive_*` at factory time).
2. Kernel: tune `cfg.keepalive.interval` / `cfg.keepalive.probes` on
   `eph::net::kernel::StreamConfig` (wires `setsockopt(SO_KEEPALIVE / TCP_
   KEEPIDLE / TCP_KEEPINTVL / TCP_KEEPCNT)`), or rely on application-layer
   ping (WS ping/pong via codec).
3. Server-side idle timeout: many venues close after 5-30 min of no
   activity. Application-layer ping is the canonical fix.

### High tail latency (p99 spikes)

**Diagnosis**:

```bash
# CPU pinning sanity
taskset -p $(pidof your_app)

# NUMA locality (NIC vs CPU)
lscpu | grep NUMA
cat /sys/class/net/<iface>/device/numa_node

# Frequency / thermal
cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_cur_freq

# Context switches
perf stat -e context-switches,cs-migrations ./your_app
```

**Fixes**:
- Isolate cores: `isolcpus=2-7` in kernel cmdline
- Per-thread pin every bench / lcore thread (see CLAUDE rule "Bench 每个
  线程必须绑独立 CPU")
- DPDK: `nb_rx_queues` ≥ 2 with RSS, pin lcore per queue
- Disable turbo for consistency (Intel: `intel_pstate/no_turbo`)

### DPDK: connection fails immediately

**Diagnosis**:

```bash
# EAL init: hugepages mounted?
mount | grep hugetlb

# Hugepage availability
cat /proc/meminfo | grep -E 'HugePages_(Free|Total)'
# Need ≥ 1024 free 2MB pages, or enough 1G pages for your app

# NIC binding
dpdk-devbind.py --status

# Resource conflicts (per CLAUDE rule)
ps -ef | grep -E 'dpdk|lat_|mockex|dpdk_e2e'
sudo lsof | grep hugepages
```

**Fixes**:

```bash
# Hugepages
echo 1024 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages

# Bind NIC to vfio-pci
sudo modprobe vfio-pci
sudo dpdk-devbind.py -b vfio-pci 0000:00:05.0

# Verify
dpdk-devbind.py --status | head -20
```

The `eph-net-dpdk/scripts/dpdk-setup.sh` and `dpdk-teardown.sh` scripts
encapsulate this idempotently. The `benchmarks/latency/lat` dispatcher
handles transitions automatically.

### DPDK: RSS bring-up fails on ENA / aged PMD

**Symptom**: `Platform::create` returns
`InvalidConfig: rss_hash_update rejected; probe also failed`.

**Cause**: PMD doesn't support `rte_eth_dev_rss_hash_update` and the probe
via `rte_eth_dev_rss_hash_conf_get` also failed (driver doesn't expose
the active key).

**Fix**:
- For diagnostic: `Platform::rss_using_probed_key()` returns `true` if the
  fallback path resolved. If `Platform::create` hard-fails, neither path
  worked.
- Confirm PMD version (some ENA versions in 21.x predate
  `rss_hash_conf_get`).
- Workaround: drop to `nb_rx_queues=1` and disable RSS (single-queue mode).

### Path MTU shrinkage

**Symptom**: `effective_mss()` shrinks below the originally negotiated
`peer_mss_negotiated()` value mid-session.

**Cause**: Router-originated ICMP Type 3 Code 4 (Frag Needed) reached the
session via `Platform::register_icmp_target`. Expected behavior — TCP is
behaving correctly.

**Diagnosis**: `TcpSession::Stats::icmp_frag_needed_received` counter.

---

## Log Levels

ephemeral uses spdlog with **compile-time** level filtering via
`SPDLOG_ACTIVE_LEVEL`:

| Build mode | Level     | Where set        |
|------------|-----------|------------------|
| `release`  | `INFO`    | xmake.lua `net_log_level` |
| `debug`    | `TRACE`   | xmake.lua        |
| `asan` / `tsan` | `DEBUG` | xmake.lua  |

Use the macros (`SPDLOG_TRACE` / `DEBUG` / `INFO` / `WARN` / `ERROR`) so
suppressed levels compile out entirely. Runtime `spdlog::set_level()` only
filters within the compile-time band.

Most modules log through the spdlog default logger via the
`SPDLOG_*` macros — there is no per-module named-logger registry to
enumerate. The exceptions (lazy-created via
`spdlog::register_logger` on first use; visible via `spdlog::get(name)`):

- `net.http_client` — HTTP request/response logging
  (`eph-net/include/eph/net/http_client.hpp`).
- `utils.tsc` / `utils.cpu` / `utils.hugepage` / `utils.audit_log` /
  `utils.system_stats` / `utils.ema` / `utils.console_sink` —
  per-utility named loggers in `eph-utils/include/eph/utils/`.
- `codec.ws` — WebSocket codec (in `eph-codec`).
- Parser-module loggers: `fix.parser` / `fix.builder` / `fix.framer` /
  `fix.session` / `fix.orders` / `fix.ordmgr` / `fix.execrpt` /
  `fix.position`; `itch.parser` / `itch.ouch` / `itch.soupbintcp`;
  `json.binance` / `json.okx` / `json.bybit`.

Filter via standard spdlog APIs (e.g. `spdlog::get("net.http_client")
->set_level(spdlog::level::debug)`) — but remember it only narrows
within the compile-time `SPDLOG_ACTIVE_LEVEL` band.
- `net.http` — HTTP/1.1 parse (upgrade requests, CONNECT proxy)
- `fix.parser` / `fix.builder` — FIX validation
- `itch.parser` — ITCH parse errors
