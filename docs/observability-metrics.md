# Observability Metrics — Reference

Catalog of every counter / gauge surfaced by `eph::net`. Pair this with
`docs/observability-guide.md` (which covers the two-layer architecture
and how to wire a sink); this page is the flat list.

Every entry is exposed via the OpenTelemetry-style hierarchical name in
the right column. The publish helpers
(`eph::net::publish_metrics`, `eph::net::publish_reconnect_metrics`,
`eph::net::publish_ws_deflate_ratio`) iterate the underlying enum and
forward each entry to your `MetricsSink` with the matching name.

Adding a new metric? See the "Adding a new metric" comment at the top of
`eph/net/stream_metrics.hpp` (Stream/Datagram metrics) or the
`ReconnectMetric` enum in `eph/net/reconnect_orchestrator.hpp`
(orchestrator metrics). Both files have a compile-time `static_assert`
that catches enum/name-table drift.

---

## Stream / Datagram metrics

Surfaced by `KernelTcpStream` / `KernelUdpSocket` /
`DpdkTcpStream` / `DpdkUdpSocket`. Defined in
`eph/net/stream_metrics.hpp` as `enum class StreamMetric`. Pushed to a
sink via `eph::net::publish_metrics(stream, sink, tags)`.

All entries are **counters** (monotonic) unless the type column says
otherwise. N/A entries for a given backend remain at 0 — the publisher
emits all rows unconditionally; sinks decide what to surface.

| Metric name | Type | Source | Description |
|---|---|---|---|
| `net.stream.bytes_sent` | counter | all 4 backends | Plaintext bytes successfully sent. For TLS this is plaintext-relative (the application-asked size, not the on-wire ciphertext). |
| `net.stream.bytes_recv` | counter | all 4 backends | Bytes received into the reasm/inbound staging buffer. For TLS counts ciphertext (pre-decrypt). |
| `net.stream.frames_decoded` | counter | all 4 backends | Application frames delivered to `on_message` / `on_datagram` (post-codec, post-TLS for TLS streams). |
| `net.stream.reasm_overflows` | counter | TCP only | TCP reassembly buffer hit capacity → session reset. UDP backends emit 0. |
| `net.stream.codec_errors` | counter | all 4 backends | `codec.decode()` returned `Err` — protocol violation, oversized frame, etc. Stream typically tears down on this signal. |
| `net.stream.tls.cross_record_frames` | counter | DPDK + TLS only | WS frame's payload spanned a TLS record boundary — slow-path memcpy into `tls_codec_pending_`. |
| `net.stream.tls.send_desyncs` | counter | DPDK + TLS only | Partial TLS send left peer's AEAD nonce/sequence out of sync; stream latches `tls_corrupt_` and tears down. |
| `net.stream.tcp.resets_received` | counter | DPDK TCP only | Peer-initiated RSTs received on the session. |
| `net.stream.tcp.out_of_order_segments` | counter | DPDK TCP only | TCP segments arrived with a forward-gap relative to `rcv_nxt`. |
| `net.stream.tcp.reorder_buffer_hits` | counter | DPDK TCP only | Out-of-order segments successfully buffered + later drained. |
| `net.stream.tcp.reorder_buffer_overflows` | counter | DPDK TCP only | Reorder buffer was full when an OoO segment arrived → session reset. |
| `net.stream.tcp.keepalive_probes_sent` | counter | DPDK TCP only | Keepalive probes emitted by `tick_keepalive`. Non-zero ≈ peer went idle past `keepalive_interval`. |
| `net.stream.tcp.mss_negotiation_applied` | counter | DPDK TCP only | SYN-ACK carried a peer MSS option that clamped our effective MSS below the configured local value. |
| `net.stream.icmp.frag_needed_received` | counter | DPDK TCP only | ICMP Type 3 Code 4 (Fragmentation Needed) messages acted on by the session. |
| `net.stream.tcp.dup_segments` | counter | DPDK TCP only | Duplicate / past-window segments — peer re-delivered already-ACKed bytes. |
| `net.stream.dpdk.rx_session_resets` | counter | DPDK TCP only | Stream layer proactively closed the session on a `process_rx` / `poll_rx` error (reorder-buffer overflow on real loss being the production trigger). |
| `net.stream.rx.bad_checksum` | counter | DPDK UDP + TCP | Aggregate of the two split counters below — `kRxIpChecksumBad + kRxL4ChecksumBad`. Read-on-demand sum (TD-1). |
| `net.stream.rx.ip_checksum_bad` | counter | DPDK UDP + TCP | NIC flagged IPv4 header cksum BAD. Gated by `PlatformConfig::enable_rx_checksum_offload`. |
| `net.stream.rx.l4_checksum_bad` | counter | DPDK UDP + TCP | NIC flagged UDP/TCP cksum BAD. Same gate. |
| `net.stream.rx.packets_dropped` | counter | DPDK UDP + TCP | Catch-all drop counter — parse fail / 4-tuple mismatch / `connect_to` filter rejection. |
| `net.stream.rx.fragment_rejected` | counter | DPDK UDP + TCP | RX packet was an IPv4 fragment (HFT workloads set DF — fragments are hostile or path-MTU is wrong). |
| `net.stream.ws.deflate_bytes_in` | counter | WS streams (kernel + DPDK) | Compressed payload bytes fed into the permessage-deflate inflater (RFC 7692). 0 for non-WS or non-negotiated streams. |
| `net.stream.ws.deflate_bytes_out` | counter | WS streams (kernel + DPDK) | Plaintext (post-inflate) bytes produced by the inflater. |
| `net.stream.tls.resume_count` | counter | TLS streams (kernel + DPDK) | TLS 1.3 handshakes that completed as resumptions (PSK / abbreviated). |
| `net.stream.tls.handshake_count` | counter | TLS streams (kernel + DPDK) | TLS 1.3 handshakes that completed as full handshakes (cert exchange, +1 RTT). |

### Derived gauges

These are NOT stored as atomic counters — the helper computes them on
the publisher thread by reading the underlying counters and forwarding
the result as a gauge. Stable names so dashboards can hard-code them.

| Metric name | Type | Helper | Description |
|---|---|---|---|
| `net.stream.ws.deflate_ratio` | gauge (double) | `eph::net::publish_ws_deflate_ratio(stream, sink, tags)` | `bytes_out / bytes_in`. Emits `0.0` (NOT `NaN`) when `bytes_in == 0` so TSDBs don't trip on missing-data alerts for fresh streams. Typical value ~0.15 on JSON market data (≈ 6.7x compression). Values > 1 are legal (deflate overhead exceeds savings on tiny payloads). |

---

## ReconnectOrchestrator metrics

Surfaced by `eph::net::ReconnectOrchestrator<S>`. Defined in
`eph/net/reconnect_orchestrator.hpp` as `enum class ReconnectMetric`.
Pushed to a sink via `eph::net::publish_reconnect_metrics(orch, sink, tags)`.

| Metric name | Type | Source | Description |
|---|---|---|---|
| `net.reconnect.count` | counter | orchestrator | Successful reconnects (the initial `start()` connect counts as 1). |
| `net.reconnect.failures` | counter | orchestrator | Factory invocation failures + attach hook failures. |
| `net.reconnect.duration_ns` | counter | orchestrator | Sum of per-cycle (disconnect → reconnect) ns durations across all successful reconnects. Pair with `count` for `avg = total / count`. |
| `net.reconnect.duration_ns.last` | gauge (double) | orchestrator | Most recent cycle's duration in ns. |
| `net.reconnect.subscribe_replay_count` | counter | application-asserted | Application calls `orch.note_subscribe_replay()` from inside the `OnReconnect` callback (or `OnReconnectEvent::Connected` handler) once it has finished re-sending — and, for ACK-bearing protocols, confirmed — the venue-specific subscribe payload. The orchestrator deliberately does NOT auto-bump this, because "stream connected" and "subscriptions restored" are distinct events: a venue can close on a malformed subscribe even though the TCP/TLS/WS stack is healthy. Pair with `count` to derive the replay success rate. |

---

## Sinks

The above helpers all forward to anything satisfying the
`eph::core::MetricsSink` concept (`push_counter`, `push_gauge`,
`push_histogram`, `flush`). Built-in sinks:

- `eph::core::NullSink` — discard everything (for tests).
- `eph::utils::ConsoleSink` — pretty-print to stderr (for dev).

User-supplied sinks (Prometheus, OpenTelemetry SDK, StatsD, ...) plug
in through duck typing — see `docs/observability-guide.md` for the
end-to-end recipe.

---

## Quick-start recipe

```cpp
#include "eph/net/kernel/tcp_stream.hpp"
#include "eph/net/reconnect_orchestrator.hpp"
#include "eph/net/stream_metrics.hpp"
#include "eph/utils/console_sink.hpp"

// ... build poller, stream, orchestrator ...

eph::utils::ConsoleSink sink;
std::array tags{eph::core::MetricTag{"venue", "binance"},
                eph::core::MetricTag{"symbol", "BTCUSDT"}};

// Periodic publisher (every 100 ms — 1 s):
eph::net::publish_metrics            (*stream, sink, tags);  // 25 stream rows
eph::net::publish_ws_deflate_ratio   (*stream, sink, tags);  // 1 derived gauge
eph::net::publish_reconnect_metrics  ( orch  , sink, tags);  // 5 orchestrator rows

// In your OnReconnect callback, after sending + ACK-confirming the subscribe:
orch.note_subscribe_replay();
```
