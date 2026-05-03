#pragma once

/// @file stream_metrics.hpp
/// Compile-time indexed metric set shared by all Stream / Datagram backends.
///
/// Two-layer observability architecture (see
/// .artifacts/discuss-20260418-181343-metrics-sink-architecture.md for the
/// full design rationale):
///
///   Layer 1 (hot path): each Stream / Datagram backend embeds an
///                       `alignas(64) std::atomic<uint64_t>` array indexed
///                       by `StreamMetric`. The hot path increments via
///                       a private `inc_<M>()` template member; readers
///                       query via a public `metric(M)` accessor. Writes
///                       and reads use `std::memory_order_relaxed` —
///                       single `lock add` / `mov` on x86 with no virtual
///                       dispatch and no string handling.
///
///   Layer 2 (reader):   `publish_metrics(stream, sink, tags)` reads every
///                       counter from the stream and pushes it into any
///                       `eph::core::MetricsSink` implementation
///                       (NullSink / ConsoleSink / user-supplied
///                       PrometheusSink / OtelSink / RecordingSink).
///                       Application code chooses when to publish
///                       (typically every 100 ms-1 s).
///
/// Naming follows OpenTelemetry conventions (`net.stream.<dimension>`) so
/// upstream sinks can map metric names directly. N/A metrics for a given
/// backend (e.g. `kReasmOverflows` on UDP) remain at 0 — `publish_metrics`
/// emits all entries unconditionally; sinks decide what to surface.
///
/// Adding a new metric:
///   1. extend `StreamMetric` enum (before `kCount`)
///   2. extend `kStreamMetricNames` with the matching OTel-style name
///   3. (optional) wire `inc_<NewMetric>()` into the relevant stream's
///      hot path
/// The `static_assert` below catches enum/name-table drift at compile time.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "eph/core/metrics_concept.hpp"

namespace eph::net {

/// @brief Compile-time indexed metric set for `Stream` / `Datagram` backends.
///
/// Members are public counters that hot path code increments via an
/// `inc_<M>()` template member on the stream. Readers query via the
/// public `metric(M)` accessor. The set is intentionally small and
/// shared across all four backends; per-backend N/A entries stay at 0.
enum class StreamMetric : std::size_t {
    /// Bytes successfully sent to the wire. For TLS streams this counts
    /// plaintext bytes the application asked to send (the Stream contract
    /// returns plaintext-relative byte counts).
    kBytesSent,

    /// Bytes successfully received from the wire into the reasm / inbound
    /// staging buffer. For TLS this counts ciphertext bytes (pre-decrypt);
    /// for plaintext it equals plaintext bytes. Counted at the byte source.
    kBytesRecv,

    /// Application frames delivered to `on_message` / `on_datagram` —
    /// i.e. bytes that successfully completed codec decoding (post-TLS
    /// for TLS streams).
    kFramesDecoded,

    /// TCP only: reassembly buffer hit capacity and the connection was
    /// reset to recover. Should normally be 0 in production; non-zero
    /// indicates either a slow consumer or a peer flooding past the
    /// configured `reasm_capacity`. UDP backends never increment this.
    kReasmOverflows,

    /// `codec.decode` returned `Err` (protocol violation, oversized
    /// frame, etc.). Stream typically tears the session down on this
    /// signal so the reconnect policy can take over.
    kCodecErrors,

    /// DPDK + TLS only: a WebSocket frame's payload spanned a TLS record
    /// boundary, forcing the slow-path memcpy into `tls_codec_pending_`
    /// (one extra copy + heap touch vs the zero-copy in-place path).
    /// Non-zero is normal but rare under typical broker traffic
    /// (record cap 16 KiB, app frames < 1 KiB); a sudden rise indicates
    /// the upstream changed its TLS write strategy.
    kTlsCrossRecordFrames,

    /// TLS streams only (kernel + DPDK): a `send()` call encrypted `N`
    /// plaintext bytes into the TLS send buffer, advancing the TLS write
    /// sequence counter, but the subsequent hand-off to the underlying
    /// socket / `TcpSession::send` failed partway through — leaving the
    /// peer's AEAD nonce/sequence state permanently out of sync with
    /// ours. The stream latches `tls_corrupt_` and every further
    /// `send()` / `process_burst_` returns `Error::Disconnected` so the
    /// reconnect loop replaces the session. Any non-zero value here
    /// means "this connection was correctly torn down before it could
    /// silently spoil its peer's crypto state"; it should be paired
    /// with `kTcpResetsReceived` or a reconnect metric on the app side.
    /// Never expected in steady state — TX queue saturation on a
    /// loopback-free HFT path is a design-level anomaly. Bumped by
    /// `KernelTcpStream::send` and `DpdkTcpStream::send` symmetrically;
    /// non-TLS streams and UDP backends emit 0.
    kTlsSendDesyncs,

    // ── DPDK TCP session-specific counters (pulled from
    //    TcpSession::Stats at read time; other backends emit 0). ──

    /// Peer-initiated RSTs received on the session. UDP backends emit 0.
    kTcpResetsReceived,

    /// TCP segments that arrived with a gap relative to rcv_nxt.
    /// DPDK only; kernel backend and UDP emit 0.
    kTcpOutOfOrderSegments,

    /// Out-of-order segments that were successfully buffered + later
    /// drained once the gap filled. DPDK TCP only.
    kTcpReorderBufferHits,

    /// Reorder buffer was already full when an out-of-order segment
    /// arrived — triggers session reset. DPDK TCP only.
    kTcpReorderBufferOverflows,

    /// Keepalive probes emitted by `tick_keepalive`. Non-zero means the
    /// peer went idle past `keepalive_interval`. DPDK TCP only.
    kTcpKeepaliveProbesSent,

    /// Keepalive ticks where the probe could not be transmitted —
    /// `rte_pktmbuf_alloc` returned null (mempool exhausted) or
    /// `rte_eth_tx_burst` returned 0 (TX queue full / driver
    /// back-pressure / link down). Each failure still consumes a
    /// "miss slot" inside `tick_keepalive`, so a stuck NIC eventually
    /// times out the connection rather than silently looping forever.
    /// Disjoint from `kTcpKeepaliveProbesSent`: a single tick increments
    /// exactly one of the two. Persistently non-zero is the canonical
    /// "TX path is wedged" signal — pair with `kTcpKeepaliveProbesSent`
    /// to compute the failure ratio. Kernel backends emit 0 (the kernel
    /// owns its own keepalive state machine and never surfaces this).
    /// DPDK TCP only.
    kTcpKeepaliveSendFailures,

    /// SYN-ACK carried a peer MSS option that clamped our effective MSS
    /// below the configured local value. Should be very low in steady
    /// state; a sudden spike indicates the upstream changed MTU.
    /// DPDK TCP only.
    kTcpMssNegotiationApplied,

    /// ICMP Type 3 Code 4 (Fragmentation Needed) messages acted on by
    /// the session. Normal on paths where PMTU shrinks mid-flight
    /// (VPN / tunnel / changed route); persistently non-zero means the
    /// MSS was not recorded correctly.
    kIcmpFragNeededReceived,

    /// DPDK TCP only: duplicate or past-window segments — peer
    /// re-delivered bytes we already ACKed (retransmit from peer, or
    /// a delayed segment that raced a new one). Distinct from
    /// `kTcpOutOfOrderSegments` (forward gap). Low non-zero is
    /// expected on lossy paths; sustained rise means the peer is
    /// retransmitting a lot or a delayed-ACK path is misconfigured.
    /// Kernel backends and UDP emit 0.
    kTcpDupSegments,

    /// The stream layer proactively tore down a stalled RX-side session
    /// from THIS side, rather than waiting for a silent stall. Two
    /// triggers, both backends:
    ///   1. `drain(timeout)` expired without observing the peer's
    ///      FIN-ACK (kernel + DPDK): peer never orderly-closed within
    ///      the configured budget.
    ///   2. DPDK TCP only — `TcpSession::process_rx` / `poll_rx` returned
    ///      an error (reorder-buffer overflow on genuine packet loss
    ///      being the production-observed trigger).
    /// Distinct from `kTcpResetsReceived` (peer-initiated RST) and from
    /// a plain connection close. Expect ~0 in healthy deployments; a
    /// sustained rise indicates either misbehaving peers (case 1) or
    /// upstream loss / NIC reordering beyond `ReorderSlots` (case 2).
    kRxSessionResets,

    /// DPDK only: aggregate counter for any RX bad-checksum drop
    /// (IP header or L4 payload). Kept **after TD-1 split** as the sum
    /// `kRxIpChecksumBad + kRxL4ChecksumBad`, read lazily via
    /// `metric(kRxBadChecksum)`. Prefer the split counters below for
    /// new code — retaining this one only to keep existing dashboards
    /// and `publish_metrics` emitters working unmodified.
    ///
    /// Gated by `PlatformConfig::enable_rx_checksum_offload` — when
    /// opt-in is off the NIC never marks packets BAD and this counter
    /// stays at 0. When opt-in is on, non-zero means either a genuine
    /// transmission error (optical bit flip, faulty switch) or a
    /// deliberately crafted packet; either way the packet was dropped
    /// before reaching the codec. Kernel backends emit 0 (the kernel
    /// UDP/TCP stack silently drops bad-cksum packets before userspace
    /// sees them).
    kRxBadChecksum,

    /// DPDK only: RX packet flagged `RTE_MBUF_F_RX_IP_CKSUM_BAD` —
    /// the NIC's IPv4 header checksum verification failed. Typically
    /// indicates a switch misbehaving mid-flight or a physical-layer
    /// bit flip in the L2/L3 header region. Operational response:
    /// check the switch path and optical modules, not the sender.
    ///
    /// Disjoint from `kRxL4ChecksumBad` when counting a single mbuf —
    /// but a single mbuf CAN bump both if both bits are set (rare but
    /// possible). Always `<= kRxBadChecksum` per invariant
    /// `kRxBadChecksum == kRxIpChecksumBad + kRxL4ChecksumBad`.
    /// Kernel backends emit 0.
    kRxIpChecksumBad,

    /// DPDK only: RX packet flagged `RTE_MBUF_F_RX_L4_CKSUM_BAD` —
    /// the NIC's UDP or TCP checksum verification failed. Typically
    /// indicates a bit flip in the payload region or a mid-path NAT
    /// that rewrote L3 addresses but forgot to fix up the L4 pseudo-
    /// header. Operational response: check for rogue middleboxes or
    /// bit-error signals on the upstream link.
    ///
    /// Disjoint from `kRxIpChecksumBad` when counting a single mbuf;
    /// a single mbuf CAN bump both if both bits are set. Kernel
    /// backends emit 0.
    kRxL4ChecksumBad,

    /// DPDK only (TCP + UDP): catch-all drop counter for any RX packet
    /// rejected before codec dispatch and NOT attributable to a more
    /// specific counter. Currently covers:
    ///   * `parse_udp_packet` / TCP parsing returned null for reasons
    ///     other than IP fragment (non-IPv4 ethertype, truncated frame,
    ///     bad IHL, multi-segment mbuf, UDP/TCP length mismatch).
    ///   * UDP `connect_to()` filter rejected the source address
    ///     (fixed-peer semantics — packets from non-configured peers
    ///     are dropped).
    ///   * TCP segments routed to the wrong session by an unrelated
    ///     4-tuple (TD-5 attribution, see `tcp.hpp::Stats::packets_dropped`).
    /// Disjoint from `kRxBadChecksum` (checksum-specific), `kFragmentRejected`
    /// (fragment-specific), and `kCodecErrors` (post-parse decode failure).
    /// Kernel backends emit 0. Non-zero in production points to upstream
    /// misconfiguration, incorrect flow steering, or adversarial traffic.
    kPacketsDropped,

    /// DPDK only (TCP + UDP): RX packet was an IPv4 fragment (MF=1 or
    /// non-zero fragment_offset) and therefore rejected by
    /// `parse_ip_header` (HFT workloads set DF + negotiate MSS; fragments
    /// are either hostile or indicate the path MTU is wrong). Dedicated
    /// counter because the operational response differs from the generic
    /// `kPacketsDropped` — a rise here typically means "check DF/MTU
    /// on the path", not "check flow-steering config". TCP also bumps
    /// this on `parse_ip_header` rejecting a fragment (TD-5 symmetry).
    /// Kernel backends emit 0 (the kernel stack reassembles or silently
    /// drops fragments before userspace sees them).
    kFragmentRejected,

    /// WebSocket only: total bytes of *compressed* payload fed into the
    /// permessage-deflate inflater (RFC 7692). Counts the on-wire size
    /// of every RSV1=1 data frame after handshake negotiation succeeded.
    /// Stays at 0 for non-WS streams and for WS streams where deflate
    /// was not negotiated. Pair with `kWsDeflateBytesOut` to compute
    /// the savings ratio (`out / in`) over a window — useful for
    /// confirming a venue is actually compressing the stream as expected.
    kWsDeflateBytesIn,

    /// WebSocket only: total bytes of *plaintext* (post-inflate) payload
    /// produced by the permessage-deflate inflater. Counts the bytes the
    /// downstream codec/application sees, which is what dashboards usually
    /// care about for "throughput" measurements. Ratio
    /// `kWsDeflateBytesOut / kWsDeflateBytesIn` is the achieved
    /// compression factor (typically 4-10x on JSON market data).
    kWsDeflateBytesOut,

    /// TLS only: TLS 1.3 handshakes that completed as **resumptions**
    /// (PSK / abbreviated, 1-RTT savings). Incremented once per
    /// successful `KernelTcpStream` / `DpdkTcpStream` create when
    /// `SSL_session_reused` returns true. Stays at 0 for non-TLS
    /// streams and for TLS streams without a `tls_resumption_ticket`
    /// in the StreamConfig.
    ///
    /// Pair with `kTlsHandshakeCount` to compute the resumption hit
    /// rate (`resume / (resume + full)`) over a window — useful to
    /// confirm the venue actually accepted the ticket and reconnects
    /// are saving the +1 RTT cert exchange.
    kTlsResumeCount,

    /// TLS only: TLS 1.3 handshakes that completed as **full**
    /// handshakes (cert exchange, +1 RTT). Incremented once per
    /// successful `KernelTcpStream` / `DpdkTcpStream` create with
    /// TLS enabled when `SSL_session_reused` returns false (or when
    /// no ticket was supplied). Stays at 0 for non-TLS streams.
    ///
    /// A non-zero `kTlsResumeCount` with `kTlsHandshakeCount == 0`
    /// after the first session indicates resumption is working as
    /// intended; `kTlsHandshakeCount > 0` after a deliberate ticket
    /// rotation or expiry is normal.
    kTlsHandshakeCount,

    kCount   ///< Sentinel — always last.
};

/// @brief OTel-style hierarchical names for each `StreamMetric`. Index
///        position MUST match the enum order; the `static_assert` below
///        catches drift at compile time.
inline constexpr std::array<std::string_view,
                            static_cast<std::size_t>(StreamMetric::kCount)>
kStreamMetricNames = {
    "net.stream.bytes_sent",
    "net.stream.bytes_recv",
    "net.stream.frames_decoded",
    "net.stream.reasm_overflows",
    "net.stream.codec_errors",
    "net.stream.tls.cross_record_frames",
    "net.stream.tls.send_desyncs",
    "net.stream.tcp.resets_received",
    "net.stream.tcp.out_of_order_segments",
    "net.stream.tcp.reorder_buffer_hits",
    "net.stream.tcp.reorder_buffer_overflows",
    "net.stream.tcp.keepalive_probes_sent",
    "net.stream.tcp.keepalive_send_failures",
    "net.stream.tcp.mss_negotiation_applied",
    "net.stream.icmp.frag_needed_received",
    "net.stream.tcp.dup_segments",
    "net.stream.dpdk.rx_session_resets",
    "net.stream.rx.bad_checksum",
    "net.stream.rx.ip_checksum_bad",
    "net.stream.rx.l4_checksum_bad",
    "net.stream.rx.packets_dropped",
    "net.stream.rx.fragment_rejected",
    "net.stream.ws.deflate_bytes_in",
    "net.stream.ws.deflate_bytes_out",
    "net.stream.tls.resume_count",
    "net.stream.tls.handshake_count",
};

static_assert(kStreamMetricNames.size() ==
              static_cast<std::size_t>(StreamMetric::kCount),
              "StreamMetric enum and kStreamMetricNames table out of sync — "
              "every enum entry must have a corresponding metric name");

/// @brief Read every counter from `source` and push it into `sink` as a
///        Prometheus-style monotonic counter, optionally tagged.
///
/// The function is `noexcept` and allocation-free: it iterates the fixed
/// `kCount` enum and forwards plain integers to the sink. `tags` are
/// supplied by the caller (typically `{venue, symbol, ...}`) and forwarded
/// unchanged — the stream itself carries no business semantics.
///
/// `Stream` is duck-typed and must expose
///   `[[nodiscard]] std::uint64_t metric(StreamMetric) const noexcept`.
/// All four built-in backends (`KernelTcpStream`, `KernelUdpSocket`,
/// `DpdkTcpStream`, `DpdkUdpSocket`) satisfy this. The test mocks
/// `eph::net::test::FakeStream` / `FakeDatagram` deliberately do
/// **not** — they exist to satisfy the `Stream` / `Datagram` concept
/// (no `metric()` requirement) for codec / poller unit tests, and
/// observability is not in their scope. If a unit test needs to
/// drive `publish_metrics`, wrap the fake or use a built-in backend.
///
/// Naming: `publish` matches Prometheus push gateway / OpenTelemetry
/// `MetricExporter` conventions. Parameter names `source` / `sink` mirror
/// the data-flow direction.
template <typename Stream, ::eph::core::MetricsSink Sink>
void publish_metrics(const Stream& source, Sink& sink,
                     std::span<const ::eph::core::MetricTag> tags = {}) noexcept {
    for (std::size_t i = 0;
         i < static_cast<std::size_t>(StreamMetric::kCount); ++i) {
        sink.push_counter(
            kStreamMetricNames[i],
            static_cast<std::int64_t>(
                source.metric(static_cast<StreamMetric>(i))),
            tags);
    }
}

/// @brief Stable metric name for the derived WS deflate compression ratio.
///        Surfaced as a separate constant so dashboards can reference it
///        without depending on the helper that publishes it (T2.11).
inline constexpr std::string_view kWsDeflateRatioMetricName =
    "net.stream.ws.deflate_ratio";

/// @brief Push the *derived* WebSocket permessage-deflate compression ratio
///        `bytes_out / bytes_in` to `sink` as a gauge with the stable name
///        `net.stream.ws.deflate_ratio`.
///
/// The ratio is **derived**, not stored: we read the two atomic counters
/// `kWsDeflateBytesIn` / `kWsDeflateBytesOut` and divide on the publisher
/// thread. This avoids a third hot-path counter (and the float math that
/// would imply) while still giving dashboards a stable, easily-aggregable
/// time-series.
///
/// Semantics:
///   * `bytes_in == 0` → gauge value 0.0. We deliberately do NOT emit
///     NaN: most TSDBs (Prometheus, OTel SDK) treat NaN samples as
///     missing-data markers and will alert spuriously when a new stream
///     is reported before its first deflate frame arrives. Zero is
///     unambiguously "no compressed data observed yet" and integrates
///     cleanly with `rate(...)` queries.
///   * `bytes_out > bytes_in` → ratio > 1, emitted verbatim. This is
///     legal in pathological cases (very small payloads where deflate
///     overhead exceeds savings) and useful as a signal that the
///     upstream's compression strategy is degrading.
///   * `bytes_out < bytes_in` → ratio in (0, 1) — the typical operating
///     point (e.g. 0.15 for JSON market data ≈ 6.7x compression).
///
/// Read order: bytes_in first, then bytes_out. Under concurrent updates
/// the two counters may snapshot at slightly different points; that's
/// fine for a 100ms-1s publish cadence — the next sample corrects any
/// transient skew.
///
/// `Stream` is duck-typed and must expose
///   `[[nodiscard]] std::uint64_t metric(StreamMetric) const noexcept`.
template <typename Stream, ::eph::core::MetricsSink Sink>
void publish_ws_deflate_ratio(const Stream& source, Sink& sink,
                              std::span<const ::eph::core::MetricTag> tags = {}) noexcept {
    const auto bytes_in  = source.metric(StreamMetric::kWsDeflateBytesIn);
    const auto bytes_out = source.metric(StreamMetric::kWsDeflateBytesOut);
    // Divide-by-zero guard: emit 0.0 (NOT NaN) so dashboards with naive
    // "is this stream healthy" checks don't trip on uninitialised data.
    const double ratio =
        (bytes_in == 0)
            ? 0.0
            : static_cast<double>(bytes_out) / static_cast<double>(bytes_in);
    sink.push_gauge(kWsDeflateRatioMetricName, ratio, tags);
}

} // namespace eph::net
