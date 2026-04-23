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

    /// DPDK + TLS only: a `send()` call encrypted `N` plaintext bytes into
    /// one `tls_send_buf_`, advancing the TLS write sequence counter, but
    /// the subsequent chunked hand-off to `TcpSession::send` failed partway
    /// through — leaving the peer's AEAD nonce/sequence state permanently
    /// out of sync with ours. The stream latches `tls_corrupt_` and every
    /// further `send()` / `process_burst_` returns `Error::Disconnected`
    /// so the reconnect loop replaces the session. Any non-zero value here
    /// means "this connection was correctly torn down before it could
    /// silently spoil its peer's crypto state"; it should be paired with
    /// `kTcpResetsReceived` or a reconnect metric on the app side. Never
    /// expected in steady state — TX queue saturation on a loopback-free
    /// HFT path is a design-level anomaly.
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

    /// DPDK TCP only: the stream layer observed an error return from
    /// `TcpSession::process_rx` / `poll_rx` (reorder-buffer overflow on
    /// genuine packet loss being the production-observed trigger) and
    /// proactively put the session into `Closed` so the app's reconnect
    /// policy can take over. Distinct from `kTcpResetsReceived` (peer-
    /// initiated RST) and from a plain connection close: this signals
    /// "RX-side session torn down from THIS side before silent stall".
    /// Expect ~0 in healthy deployments; a sustained rise indicates
    /// upstream loss or NIC reordering beyond the configured
    /// `ReorderSlots` capacity.
    kRxSessionResets,

    /// DPDK only: a received packet's L3 (IPv4 header) or L4 (UDP / TCP)
    /// checksum was flagged BAD by the NIC's RX offload. Gated by
    /// `PlatformConfig::enable_rx_checksum_offload` — when opt-in is off
    /// the NIC never marks packets BAD and this counter stays at 0. When
    /// opt-in is on, non-zero means either a genuine transmission error
    /// (optical bit flip, faulty switch) or a deliberately crafted packet;
    /// either way the packet was dropped before reaching the codec. The
    /// counter combines L3 and L4 BAD flags — split into separate IP /
    /// L4 counters if operators need to disambiguate. Kernel backends
    /// emit 0 (the kernel UDP/TCP stack silently drops bad-cksum packets
    /// before userspace sees them). DpdkTcpStream does not yet wire this
    /// (symmetric fix is a follow-up, see eph-net-dpdk CHANGELOG TD-3).
    kRxBadChecksum,

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
    "net.stream.tcp.mss_negotiation_applied",
    "net.stream.icmp.frag_needed_received",
    "net.stream.dpdk.rx_session_resets",
    "net.stream.rx.bad_checksum",
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
/// `DpdkTcpStream`, `DpdkUdpSocket`) satisfy this.
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

} // namespace eph::net
